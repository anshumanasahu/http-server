#include "HttpParser.h"
#include <sstream>
#include <algorithm>
#include <cctype>
#include <string_view>

// Helper to trim trailing \r and whitespace
static inline void rtrim(std::string &s) {
    s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), s.end());
}

static inline char hexToChar(char h) {
    if (h >= '0' && h <= '9') return h - '0';
    if (h >= 'a' && h <= 'f') return h - 'a' + 10;
    if (h >= 'A' && h <= 'F') return h - 'A' + 10;
    return 0;
}

std::string HttpParser::urlDecode(const std::string& src) {
    std::string ret;
    ret.reserve(src.length());
    for (size_t i = 0; i < src.length(); ++i) {
        if (src[i] == '%' && i + 2 < src.length()) {
            char h1 = src[i + 1];
            char h2 = src[i + 2];
            if (std::isxdigit(static_cast<unsigned char>(h1)) && 
                std::isxdigit(static_cast<unsigned char>(h2))) {
                ret += static_cast<char>((hexToChar(h1) << 4) | hexToChar(h2));
                i += 2;
            } else {
                ret += '%';
            }
        } else if (src[i] == '+') {
            ret += ' ';
        } else {
            ret += src[i];
        }
    }
    return ret;
}

ParseResult HttpParser::parse_incremental(Connection& conn) {
    while (true) {
        if (conn.parse_state == ParseState::REQUEST_LINE) {
            std::string_view buf(conn.read_buf.data(), conn.read_buf.size());
            size_t crlf = buf.find("\r\n");
            if (crlf == std::string_view::npos) {
                // Check if \n\n is used instead
                size_t lflf = buf.find("\n\n");
                if (lflf != std::string_view::npos) {
                   // Not standard, but we handled it before. For strict incremental, we can skip or implement.
                   // Let's stick to \r\n for simplicity and correctness as per RFC.
                }
                return ParseResult::INCOMPLETE;
            }
            
            std::string line(buf.substr(0, crlf));
            conn.headers_bytes_seen += line.length() + 2;
            if (conn.headers_bytes_seen > 8192) {
                conn.parse_state = ParseState::ERROR;
                return ParseResult::ERROR;
            }
            
            std::istringstream stream(line);
            stream >> conn.current_request.method >> conn.current_request.path >> conn.current_request.version;
            conn.current_request.path = urlDecode(conn.current_request.path);
            
            conn.read_buf.erase(conn.read_buf.begin(), conn.read_buf.begin() + crlf + 2);
            conn.parse_state = ParseState::HEADERS;
        }
        
        if (conn.parse_state == ParseState::HEADERS) {
            while (true) {
                std::string_view buf(conn.read_buf.data(), conn.read_buf.size());
                size_t crlf = buf.find("\r\n");
                if (crlf == std::string_view::npos) {
                    return ParseResult::INCOMPLETE;
                }
                
                if (crlf == 0) {
                    // Blank line, end of headers
                    conn.headers_bytes_seen += 2;
                    if (conn.headers_bytes_seen > 8192) {
                        conn.parse_state = ParseState::ERROR;
                        return ParseResult::ERROR;
                    }
                    
                    conn.read_buf.erase(conn.read_buf.begin(), conn.read_buf.begin() + 2);
                    
                    auto te_it = conn.current_request.headers.find("Transfer-Encoding");
                    if (te_it != conn.current_request.headers.end() && te_it->second == "chunked") {
                        conn.parse_state = ParseState::BODY_CHUNKED;
                        conn.reading_chunk_size = true;
                    } else {
                        auto cl_it = conn.current_request.headers.find("Content-Length");
                        if (cl_it != conn.current_request.headers.end()) {
                            conn.parse_state = ParseState::BODY_CONTENT_LENGTH;
                        } else {
                            conn.parse_state = ParseState::COMPLETE;
                            return ParseResult::COMPLETE;
                        }
                    }
                    break;
                }
                
                std::string line(buf.substr(0, crlf));
                conn.headers_bytes_seen += line.length() + 2;
                conn.header_count++;
                if (conn.headers_bytes_seen > 8192 || conn.header_count > 100) {
                    conn.parse_state = ParseState::ERROR;
                    return ParseResult::ERROR;
                }
                
                size_t colon = line.find(':');
                if (colon != std::string::npos) {
                    std::string key = line.substr(0, colon);
                    std::string value = line.substr(colon + 1);
                    size_t start = value.find_first_not_of(" \t");
                    if (start != std::string::npos) value = value.substr(start);
                    else value = "";
                    conn.current_request.headers[key] = value;
                }
                
                conn.read_buf.erase(conn.read_buf.begin(), conn.read_buf.begin() + crlf + 2);
            }
        }
        
        if (conn.parse_state == ParseState::BODY_CONTENT_LENGTH) {
            auto it = conn.current_request.headers.find("Content-Length");
            int content_length = 0;
            try {
                content_length = std::stoi(it->second);
            } catch (...) {
                return ParseResult::ERROR;
            }
            
            if (conn.current_request.body.length() + conn.read_buf.size() >= content_length) {
                size_t needed = content_length - conn.current_request.body.length();
                conn.current_request.body.append(conn.read_buf.data(), needed);
                conn.read_buf.erase(conn.read_buf.begin(), conn.read_buf.begin() + needed);
                conn.parse_state = ParseState::COMPLETE;
                return ParseResult::COMPLETE;
            } else {
                conn.current_request.body.append(conn.read_buf.data(), conn.read_buf.size());
                conn.read_buf.clear();
                return ParseResult::INCOMPLETE;
            }
        }
        
        if (conn.parse_state == ParseState::BODY_CHUNKED) {
            while (true) {
                if (conn.reading_chunk_size) {
                    std::string_view buf(conn.read_buf.data(), conn.read_buf.size());
                    size_t crlf = buf.find("\r\n");
                    if (crlf == std::string_view::npos) {
                        return ParseResult::INCOMPLETE;
                    }
                    std::string hex_size(buf.substr(0, crlf));
                    try {
                        conn.chunk_bytes_left = std::stoi(hex_size, nullptr, 16);
                    } catch (...) {
                        return ParseResult::ERROR;
                    }
                    
                    conn.read_buf.erase(conn.read_buf.begin(), conn.read_buf.begin() + crlf + 2);
                    conn.reading_chunk_size = false;
                    
                    if (conn.chunk_bytes_left == 0) {
                        std::string_view trail_buf(conn.read_buf.data(), conn.read_buf.size());
                        if (trail_buf.length() >= 2 && trail_buf.substr(0, 2) == "\r\n") {
                            conn.read_buf.erase(conn.read_buf.begin(), conn.read_buf.begin() + 2);
                            conn.parse_state = ParseState::COMPLETE;
                            return ParseResult::COMPLETE;
                        } else if (trail_buf.length() < 2) {
                            return ParseResult::INCOMPLETE;
                        } else {
                            // Assume no complex trailers for now
                            return ParseResult::ERROR;
                        }
                    }
                } else {
                    if (conn.read_buf.size() >= conn.chunk_bytes_left + 2) {
                        conn.current_request.body.append(conn.read_buf.data(), conn.chunk_bytes_left);
                        conn.read_buf.erase(conn.read_buf.begin(), conn.read_buf.begin() + conn.chunk_bytes_left + 2);
                        conn.reading_chunk_size = true;
                    } else {
                        return ParseResult::INCOMPLETE;
                    }
                }
            }
        }
    }
}

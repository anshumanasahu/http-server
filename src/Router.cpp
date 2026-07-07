#include "Router.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cctype>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <iostream>
#include <cstring>
#include <zlib.h>

namespace fs = std::filesystem;

static std::string compress_gzip(const std::string& data) {
    if (data.empty()) return data;

    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (deflateInit2(&zs, Z_BEST_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return data;
    }

    zs.next_in = (Bytef*)data.data();
    zs.avail_in = data.size();

    int ret;
    char outbuffer[32768];
    std::string outstring;

    do {
        zs.next_out = reinterpret_cast<Bytef*>(outbuffer);
        zs.avail_out = sizeof(outbuffer);

        ret = deflate(&zs, Z_FINISH);

        if (outstring.size() < zs.total_out) {
            outstring.append(outbuffer, zs.total_out - outstring.size());
        }
    } while (ret == Z_OK);

    deflateEnd(&zs);

    if (ret != Z_STREAM_END) return data;
    return outstring;
}

void Router::addRoute(const std::string& method, const std::string& path, RouteHandler handler) {
    routes[method + " " + path] = handler;
}

void Router::addProxyRoute(const std::string& prefix, const std::vector<std::pair<std::string, int>>& backends) {
    ProxyRoute pr;
    pr.prefix = prefix;
    for (const auto& b : backends) {
        pr.backends.push_back({b.first, b.second, true, {}});
    }
    proxy_routes.push_back(pr);
}

void Router::setStaticDirectory(const std::string& dir) {
    static_dir = dir;
}

std::string Router::getMimeType(const std::string& filepath) const {
    size_t dot_pos = filepath.find_last_of('.');
    if (dot_pos == std::string::npos) {
        return "application/octet-stream";
    }
    std::string ext = filepath.substr(dot_pos);
    for (auto &c : ext) c = std::tolower(c);
    
    if (ext == ".html" || ext == ".htm") return "text/html";
    if (ext == ".css") return "text/css";
    if (ext == ".js") return "application/javascript";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif") return "image/gif";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".txt") return "text/plain";
    if (ext == ".json") return "application/json";
    
    return "application/octet-stream";
}

Response Router::route(const Request& request) {
    Response res;
    bool handled = false;

    // 1. Check exact registered routes first
    std::string key = request.method + " " + request.path;
    auto it = routes.find(key);
    if (it != routes.end()) {
        res = it->second(request);
        handled = true;
    }
    
    // 1.5. Check Proxy Routes
    if (!handled) {
        for (auto& pr : proxy_routes) {
            if (request.path.find(pr.prefix) == 0) {
                res = forwardProxy(request, pr);
                handled = true;
                break;
            }
        }
    }
    
    // 2. Fallback to static files for GET request if static_dir is configured
    if (!handled && !static_dir.empty() && request.method == "GET") {
        try {
            fs::path root = fs::weakly_canonical(fs::absolute(static_dir));
            std::string req_path = request.path;
            while (!req_path.empty() && (req_path[0] == '/' || req_path[0] == '\\')) {
                req_path.erase(0, 1);
            }
            fs::path target = fs::weakly_canonical(root / req_path);
            
            auto rel = fs::relative(target, root);
            if (rel.string().find("..") != std::string::npos || rel.is_absolute()) {
                res = Response(403, "403 Forbidden: Path traversal attempt\n");
                handled = true;
            } else {
                if (fs::is_directory(target)) {
                    target /= "index.html";
                }
                if (fs::exists(target) && fs::is_regular_file(target)) {
                    std::ifstream file(target, std::ios::binary);
                    if (file.is_open()) {
                        std::ostringstream ss;
                        ss << file.rdbuf();
                        res = Response(200, ss.str());
                        res.headers["Content-Type"] = getMimeType(target.string());
                        handled = true;
                    }
                }
            }
        } catch (const std::exception& e) {
            res = Response(500, "Internal Server Error during static file resolution\n");
            handled = true;
        }
    }
    
    // 3. SPA Fallback to index.html for unrecognized non-API GET routes
    if (!handled && request.method == "GET" && request.path.find("/api/") != 0 && !static_dir.empty()) {
        fs::path target = fs::weakly_canonical(fs::absolute(static_dir) / "index.html");
        if (fs::exists(target)) {
            std::ifstream file(target, std::ios::binary);
            if (file.is_open()) {
                std::ostringstream ss;
                ss << file.rdbuf();
                res = Response(200, ss.str());
                res.headers["Content-Type"] = "text/html";
                handled = true;
            }
        }
    }
    
    if (!handled) {
        res = Response(404, "404 Not Found\n");
    }

    // Compression Layer
    auto enc_it = request.headers.find("Accept-Encoding");
    if (!res.is_sse && enc_it != request.headers.end() && enc_it->second.find("gzip") != std::string::npos) {
        // Don't compress small bodies or already compressed content
        if (res.body.length() > 50 && res.headers["Content-Type"].find("image") == std::string::npos) {
            res.body = compress_gzip(res.body);
            res.headers["Content-Encoding"] = "gzip";
            res.headers["Content-Length"] = std::to_string(res.body.length());
        }
    }

    return res;
}

Response Router::forwardProxy(const Request& req, ProxyRoute& proxy) {
    if (proxy.backends.empty()) return Response(502, "Bad Gateway (no backends)\n");

    auto now = std::chrono::steady_clock::now();
    for (size_t i = 0; i < proxy.backends.size(); ++i) {
        int idx = (proxy.next_rr_index + i) % proxy.backends.size();
        auto& backend = proxy.backends[idx];

        if (!backend.is_healthy) {
            if (std::chrono::duration_cast<std::chrono::seconds>(now - backend.last_failed).count() > 10) {
                backend.is_healthy = true;
            } else {
                continue; // Still unhealthy
            }
        }

        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) continue; 

        struct timeval tv;
        tv.tv_sec = 2; tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        struct hostent *server = gethostbyname(backend.target_host.c_str());
        if (server == nullptr) {
            close(sock);
            backend.is_healthy = false;
            backend.last_failed = std::chrono::steady_clock::now();
            continue;
        }

        struct sockaddr_in serv_addr;
        std::memset(&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        std::memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);
        serv_addr.sin_port = htons(backend.target_port);

        if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
            close(sock);
            backend.is_healthy = false;
            backend.last_failed = std::chrono::steady_clock::now();
            continue; 
        }

        // Connection successful!
        proxy.next_rr_index = (idx + 1) % proxy.backends.size();

        // Serialize request
        std::ostringstream ss;
        ss << req.method << " " << req.path << " HTTP/1.1\r\n";
        for (const auto& h : req.headers) {
            if (h.first == "Host") {
                ss << "Host: " << backend.target_host << ":" << backend.target_port << "\r\n";
            } else if (h.first != "Connection") {
                ss << h.first << ": " << h.second << "\r\n";
            }
        }
        ss << "Connection: close\r\n\r\n";
        ss << req.body;

        std::string req_str = ss.str();
        write(sock, req_str.c_str(), req_str.length());

        // Read response
        std::string raw_resp;
        char buffer[4096];
        bool read_error = false;
        while (true) {
            int n = read(sock, buffer, sizeof(buffer));
            if (n < 0) {
                read_error = true;
                break;
            }
            if (n == 0) break;
            raw_resp.append(buffer, n);
        }
        close(sock);

        if (read_error && raw_resp.empty()) {
            backend.is_healthy = false;
            backend.last_failed = std::chrono::steady_clock::now();
            continue;
        }

        if (raw_resp.empty()) {
            backend.is_healthy = false;
            backend.last_failed = std::chrono::steady_clock::now();
            continue; 
        }

        // Parse minimal response (status and headers)
        Response res;
        size_t header_end = raw_resp.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            res.status_code = 502;
            res.body = "Bad Gateway (invalid upstream response)\n";
            return res;
        }

        std::string headers_part = raw_resp.substr(0, header_end);
        res.body = raw_resp.substr(header_end + 4);

        std::istringstream h_ss(headers_part);
        std::string line;
        if (std::getline(h_ss, line)) {
            if (line.back() == '\r') line.pop_back();
            size_t space1 = line.find(' ');
            if (space1 != std::string::npos) {
                size_t space2 = line.find(' ', space1 + 1);
                if (space2 != std::string::npos) {
                    res.status_code = std::stoi(line.substr(space1 + 1, space2 - space1 - 1));
                } else {
                    res.status_code = std::stoi(line.substr(space1 + 1));
                }
            }
        }

        while (std::getline(h_ss, line)) {
            if (line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string key = line.substr(0, colon);
                std::string val = line.substr(colon + 1);
                size_t first_non_space = val.find_first_not_of(" \t");
                if (first_non_space != std::string::npos) val = val.substr(first_non_space);
                if (key != "Transfer-Encoding" && key != "Connection") { 
                    res.headers[key] = val;
                }
            }
        }

        return res;
    }

    return Response(502, "Bad Gateway (all backends failed)\n");
}

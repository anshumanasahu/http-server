#include "Response.h"
#include <sstream>

Response::Response() : status_code(200) {}

Response::Response(int code, const std::string& body_content) 
    : status_code(code), body(body_content) {
    headers["Content-Length"] = std::to_string(body.length());
    if (headers.find("Content-Type") == headers.end()) {
        headers["Content-Type"] = "text/plain";
    }
}

std::string Response::getStatusMessage(int code) const {
    switch (code) {
        case 200: return "OK";
        case 404: return "Not Found";
        case 500: return "Internal Server Error";
        default: return "Unknown";
    }
}

std::string Response::serialize() const {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status_code << " " << getStatusMessage(status_code) << "\r\n";
    
    for (const auto& pair : headers) {
        oss << pair.first << ": " << pair.second << "\r\n";
    }
    
    oss << "\r\n";
    oss << body;
    
    return oss.str();
}

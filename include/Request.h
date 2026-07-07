#pragma once
#include <string>
#include <unordered_map>

class Request {
public:
    std::string method;
    std::string path;
    std::string version;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    
    bool isKeepAlive() const {
        auto it = headers.find("Connection");
        if (it != headers.end()) {
            return it->second != "close";
        }
        // HTTP/1.1 defaults to keep-alive
        return version == "HTTP/1.1";
    }
};

#pragma once
#include <string>
#include <unordered_map>

class Response {
public:
    int status_code;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    bool is_sse = false;

    Response();
    Response(int code, const std::string& body_content = "");
    
    std::string serialize() const;
    
private:
    std::string getStatusMessage(int code) const;
};

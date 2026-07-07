#pragma once
#include <string>
#include <chrono>
#include <unordered_map>
#include <functional>
#include "Request.h"
#include "Response.h"

using RouteHandler = std::function<Response(const Request&)>;

class Router {
public:
    void addRoute(const std::string& method, const std::string& path, RouteHandler handler);
    void addProxyRoute(const std::string& prefix, const std::vector<std::pair<std::string, int>>& backends);
    Response route(const Request& request);
    void setStaticDirectory(const std::string& dir);

private:
    struct Backend {
        std::string target_host;
        int target_port;
        bool is_healthy = true;
        std::chrono::steady_clock::time_point last_failed;
    };

    struct ProxyRoute {
        std::string prefix;
        std::vector<Backend> backends;
        size_t next_rr_index = 0;
    };

    // Key format: "METHOD PATH" e.g., "GET /api/status"
    std::unordered_map<std::string, RouteHandler> routes;
    std::vector<ProxyRoute> proxy_routes;
    std::string static_dir;

    std::string getMimeType(const std::string& filepath) const;
    Response forwardProxy(const Request& req, ProxyRoute& proxy);
};

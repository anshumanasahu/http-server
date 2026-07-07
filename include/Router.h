#pragma once
#include <string>
#include <chrono>
#include <unordered_map>
#include <functional>
#include <atomic>
#include <memory>
#include "Request.h"
#include "Response.h"
#include "LRUCache.h"

using RouteHandler = std::function<Response(const Request&)>;

class Router {
public:
    void addRoute(const std::string& method, const std::string& path, RouteHandler handler);
    void addProxyRoute(const std::string& prefix, const std::vector<std::pair<std::string, int>>& backends);
    Response route(const Request& request);
    void setStaticDirectory(const std::string& dir);

    struct Backend {
        std::string target_host;
        int target_port;
        bool is_healthy = true;
        std::chrono::steady_clock::time_point last_failed;
        std::atomic<uint64_t> req_count{0};
        std::atomic<uint64_t> last_latency_ms{0};
    };

    struct ProxyRoute {
        std::string prefix;
        std::vector<std::shared_ptr<Backend>> backends;
        size_t next_rr_index = 0;
    };

    std::vector<ProxyRoute> proxy_routes;

private:
    // Key format: "METHOD PATH" e.g., "GET /api/status"
    std::unordered_map<std::string, RouteHandler> routes;
    std::string static_dir;
    LRUCache cache{100};

    std::string getMimeType(const std::string& filepath) const;
    Response forwardProxy(const Request& req, ProxyRoute& proxy);
};

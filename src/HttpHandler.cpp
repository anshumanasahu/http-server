#include "HttpHandler.h"
#include "Connection.h"
#include "HttpParser.h"
#include "Metrics.h"
#include "WorkerPool.h"
#include <sys/epoll.h>
#include <iostream>

HttpHandler::HttpHandler(Router& router) : router_(router) {}

void HttpHandler::onConnect(std::shared_ptr<Connection> conn, int epoll_fd) {
    // No special initialization needed for HTTP
}

void HttpHandler::onDataReceived(std::shared_ptr<Connection> conn, WorkerPool& pool, int epoll_fd) {
    bool should_close = false;
    while (!conn->is_sse) {
        if (conn->parse_state == ParseState::REQUEST_LINE && conn->headers_bytes_seen == 0) {
            conn->timings.parse_start = std::chrono::steady_clock::now();
        }
        
        ParseResult res = HttpParser::parse_incremental(*conn);
        
        if (res == ParseResult::ERROR) {
            should_close = true;
            break;
        }
        if (res == ParseResult::INCOMPLETE) {
            break;
        }
        if (res == ParseResult::COMPLETE) {
            conn->timings.parse_end = std::chrono::steady_clock::now();
            conn->timings.route_start = std::chrono::steady_clock::now();
            
            conn->keep_alive = conn->current_request.isKeepAlive();
            if (conn->keep_alive) Metrics::getInstance().keep_alive_hits++;
            
            Request req = conn->current_request;
            std::shared_ptr<Connection> safe_conn = conn;
            
            if (!conn->is_sse) {
                conn->reset_for_next_request();
            }
            
            Router& router_ref = router_;
            pool.enqueue([&router_ref, safe_conn, req, epoll_fd]() {
                Response response = router_ref.route(req);
                response.headers["Connection"] = safe_conn->keep_alive ? "keep-alive" : "close";
                safe_conn->is_sse = response.is_sse;
                std::string serialized = response.serialize();
                
                safe_conn->timings.route_end = std::chrono::steady_clock::now();
                safe_conn->timings.write_start = safe_conn->timings.route_end;
                
                {
                    std::lock_guard<std::mutex> lock(safe_conn->write_mutex);
                    safe_conn->write_queue.push(serialized);
                }
                
                auto t_accept = std::max<long long>(0, safe_conn->timings.t_accept_us);
                auto t_parse = std::max<long long>(0, std::chrono::duration_cast<std::chrono::microseconds>(safe_conn->timings.parse_end - safe_conn->timings.parse_start).count());
                auto t_route = std::max<long long>(0, std::chrono::duration_cast<std::chrono::microseconds>(safe_conn->timings.route_end - safe_conn->timings.route_start).count());
                auto total_duration = t_accept + t_parse + t_route;
                
                Metrics::getInstance().total_requests++;
                Metrics::getInstance().http_requests++;
                Metrics::getInstance().bytes_transferred += serialized.length();
                
                std::string log_json = "{\"method\": \"" + req.method + 
                          "\", \"path\": \"" + req.path + 
                          "\", \"status\": " + std::to_string(response.status_code) + 
                          ", \"latency_us\": " + std::to_string(total_duration) + 
                          ", \"t_accept\": " + std::to_string(t_accept) +
                          ", \"t_parse\": " + std::to_string(t_parse) +
                          ", \"t_route\": " + std::to_string(t_route) + "}";
                std::cout << log_json << std::endl;
                Metrics::getInstance().addLog(log_json);
                          
                struct epoll_event ev;
                ev.data.fd = safe_conn->fd;
                ev.events = EPOLLIN | EPOLLOUT;
                epoll_ctl(epoll_fd, EPOLL_CTL_MOD, safe_conn->fd, &ev);
            });
        }
    }
    if (should_close) {
        conn->marked_for_close = true;
    }
}

void HttpHandler::onDisconnect(std::shared_ptr<Connection> conn) {
    // No special cleanup needed for HTTP
}

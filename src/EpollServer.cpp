#include "EpollServer.h"
#include "Connection.h"
#include "HttpParser.h"
#include "RateLimiter.h"
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <cstring>
#include <unordered_map>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include "Metrics.h"
#include <signal.h>

extern volatile sig_atomic_t server_running;

#define MAX_EVENTS 1024

EpollServer::EpollServer(int port, int tls_port, Router& router, SSL_CTX* ctx) 
    : port_(port), tls_port_(tls_port), router_(router), ctx_(ctx), thread_pool_(4) {}

int EpollServer::make_socket_non_blocking(int sfd) {
    int flags = fcntl(sfd, F_GETFL, 0);
    if (flags == -1) return -1;
    flags |= O_NONBLOCK;
    if (fcntl(sfd, F_SETFL, flags) == -1) return -1;
    return 0;
}

void EpollServer::run() {
    int server_fd, tls_server_fd;
    struct sockaddr_in address, tls_address;
    int opt = 1;

    // --- Plaintext Server ---
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) return;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) return;
    if (make_socket_non_blocking(server_fd) == -1) return;

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port_);
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) return;
    if (listen(server_fd, 10000) < 0) return;

    // --- TLS Server ---
    if ((tls_server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) return;
    if (setsockopt(tls_server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) return;
    if (make_socket_non_blocking(tls_server_fd) == -1) return;

    tls_address.sin_family = AF_INET;
    tls_address.sin_addr.s_addr = INADDR_ANY;
    tls_address.sin_port = htons(tls_port_);
    if (bind(tls_server_fd, (struct sockaddr *)&tls_address, sizeof(tls_address)) < 0) return;
    if (listen(tls_server_fd, 10000) < 0) return;

    // --- Epoll Setup ---
    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) return;

    struct epoll_event event;
    event.data.fd = server_fd;
    event.events = EPOLLIN;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event);

    event.data.fd = tls_server_fd;
    event.events = EPOLLIN;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, tls_server_fd, &event);

    struct epoll_event events[MAX_EVENTS];
    std::unordered_map<int, std::shared_ptr<Connection>> connections;

    std::cout << "Epoll Server listening on port " << port_ << " and TLS " << tls_port_ << "..." << std::endl;

    while (server_running) {
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, 1000);
        
        auto now = std::chrono::steady_clock::now();
        
        static auto last_sse_push = now;
        if (now - last_sse_push > std::chrono::seconds(1)) {
            std::string tcp_states = Metrics::getInstance().read_tcp_states();
            std::string logs_array = Metrics::getInstance().flushLogs();
            
            std::string proxy_json = "[";
            for (const auto& pr : router_.proxy_routes) {
                for (const auto& b : pr.backends) {
                    proxy_json += "{\"host\":\"" + b->target_host + ":" + std::to_string(b->target_port) + 
                                  "\",\"healthy\":" + (b->is_healthy ? "true" : "false") + 
                                  ",\"req_count\":" + std::to_string(b->req_count.load()) + 
                                  ",\"latency_ms\":" + std::to_string(b->last_latency_ms.load()) + "},";
                }
            }
            if (proxy_json.length() > 1) proxy_json.pop_back();
            proxy_json += "]";
            
            std::string sse_data = "data: {\"active\": " + std::to_string(Metrics::getInstance().active_connections.load()) + 
                                   ", \"requests\": " + std::to_string(Metrics::getInstance().total_requests.load()) + 
                                   ", \"blocked\": " + std::to_string(Metrics::getInstance().blocked_requests.load()) + 
                                   ", \"queue\": " + std::to_string(thread_pool_.queue_size()) + 
                                   ", \"keep_alive_hits\": " + std::to_string(Metrics::getInstance().keep_alive_hits.load()) + 
                                   ", \"new_connections\": " + std::to_string(Metrics::getInstance().new_connections.load()) + 
                                   ", \"tcp_states\": " + tcp_states +
                                   ", \"proxy_nodes\": " + proxy_json +
                                   ", \"logs\": " + logs_array + "}\n\n";
            for (auto& pair : connections) {
                if (pair.second->is_sse) {
                    std::lock_guard<std::mutex> lock(pair.second->write_mutex);
                    pair.second->write_queue.push(sse_data);
                    struct epoll_event ev;
                    ev.data.fd = pair.first;
                    ev.events = EPOLLIN | EPOLLOUT;
                    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, pair.first, &ev);
                }
            }
            last_sse_push = now;
        }

        for (auto it = connections.begin(); it != connections.end(); ) {
            if (!it->second->is_sse && std::chrono::duration_cast<std::chrono::seconds>(now - it->second->last_activity).count() > 10) {
                std::cout << "[Epoll] Connection timeout on fd " << it->first << std::endl;
                Metrics::getInstance().active_connections--;
                it = connections.erase(it);
            } else {
                ++it;
            }
        }
        
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            
            if (fd == server_fd || fd == tls_server_fd) {
                while (true) {
                    struct sockaddr_in in_addr;
                    socklen_t in_len = sizeof(in_addr);
                    auto acc_start = std::chrono::steady_clock::now();
                    int infd = accept(fd, (struct sockaddr *)&in_addr, &in_len);
                    auto acc_end = std::chrono::steady_clock::now();
                    uint64_t acc_us = std::chrono::duration_cast<std::chrono::microseconds>(acc_end - acc_start).count();
                    
                    if (infd == -1) {
                        if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
                            break;
                        } else {
                            std::cerr << "accept error" << std::endl;
                            break;
                        }
                    }

                    uint32_t ip = in_addr.sin_addr.s_addr;
                    if (!RateLimiter::getInstance().allow(ip)) {
                        std::string reject = "HTTP/1.1 429 Too Many Requests\r\nConnection: close\r\n\r\n";
                        write(infd, reject.c_str(), reject.length());
                        close(infd);
                        continue;
                    }

                    make_socket_non_blocking(infd);
                    
                    SSL* ssl = nullptr;
                    if (fd == tls_server_fd) {
                        ssl = SSL_new(ctx_);
                        SSL_set_fd(ssl, infd);
                        SSL_set_accept_state(ssl); 
                    }

                    event.data.fd = infd;
                    event.events = EPOLLIN; 
                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, infd, &event) == -1) {
                        std::cerr << "epoll_ctl error for infd" << std::endl;
                        if (ssl) SSL_free(ssl);
                        close(infd);
                    } else {
                        connections[infd] = std::make_shared<Connection>(infd, acc_us, ssl);
                        Metrics::getInstance().active_connections++;
                        Metrics::getInstance().new_connections++;
                    }
                }
            } else {
                if (connections.find(fd) == connections.end()) continue;
                std::shared_ptr<Connection> conn = connections[fd];
                
                if ((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP)) {
                    Metrics::getInstance().active_connections--;
                    connections.erase(fd);
                    continue;
                }

                if (events[i].events & EPOLLIN) {
                    bool connection_closed = false;
                    while (true) {
                        char buf[4096];
                        ssize_t count = conn->conn_read(buf, sizeof(buf));
                        if (count <= 0) {
                            if (conn->tls_session) {
                                int err = SSL_get_error(conn->tls_session, count);
                                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                                    break;
                                }
                            } else {
                                if (count == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                                    break;
                                }
                            }
                            connection_closed = true;
                            break;
                        }
                        conn->read_buf.insert(conn->read_buf.end(), buf, buf + count);
                        conn->update_activity();
                    }

                    if (connection_closed) {
                        Metrics::getInstance().active_connections--;
                        connections.erase(fd);
                        continue;
                    }

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
                            
                            thread_pool_.enqueue([this, safe_conn, req, epoll_fd]() {
                                Response response = router_.route(req);
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
                        Metrics::getInstance().active_connections--;
                        connections.erase(fd);
                        continue;
                    }
                }

                if (events[i].events & EPOLLOUT) {
                    bool write_error = false;
                    while (true) {
                        std::string data;
                        {
                            std::lock_guard<std::mutex> lock(conn->write_mutex);
                            if (conn->write_queue.empty()) break;
                            data = conn->write_queue.front();
                        }
                        
                        ssize_t count = conn->conn_write(data.c_str(), data.length());
                        
                        if (count <= 0) {
                            if (conn->tls_session) {
                                int err = SSL_get_error(conn->tls_session, count);
                                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                                    break;
                                }
                            } else {
                                if (count == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                                    break;
                                }
                            }
                            write_error = true;
                            break;
                        }
                        
                        {
                            std::lock_guard<std::mutex> lock(conn->write_mutex);
                            if (count == (ssize_t)data.length()) {
                                conn->write_queue.pop();
                            } else {
                                conn->write_queue.front() = data.substr(count);
                                break;
                            }
                        }
                        conn->update_activity();
                    }

                    if (write_error) {
                        Metrics::getInstance().active_connections--;
                        connections.erase(fd);
                        continue;
                    }

                    bool empty_queue;
                    {
                        std::lock_guard<std::mutex> lock(conn->write_mutex);
                        empty_queue = conn->write_queue.empty();
                    }
                    
                    if (empty_queue) {
                        if (!conn->keep_alive) {
                            Metrics::getInstance().active_connections--;
                            connections.erase(fd);
                        } else {
                            conn->timings.t_accept_us = 0; // reset for next request on keep-alive
                            event.data.fd = fd;
                            event.events = EPOLLIN;
                            epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &event);
                        }
                    }
                }
            }
        }
    }

    std::cout << "Shutting down Epoll Server. Cleaning up connections..." << std::endl;
    connections.clear();
    Metrics::getInstance().active_connections = 0;
    
    close(server_fd);
    close(tls_server_fd);
    close(epoll_fd);
}

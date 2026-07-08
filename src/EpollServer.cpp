#include "EpollServer.h"
#include "Connection.h"
#include "HttpParser.h"
#include "RateLimiter.h"
#include "HttpHandler.h"
#include "SmtpHandler.h"
#include "FtpHandler.h"
#include "ImapHandler.h"
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

EpollServer::EpollServer(std::vector<ListenerConfig> listeners, Router& router, SSL_CTX* ctx) 
    : listeners_(listeners), router_(router), ctx_(ctx), thread_pool_(4) {}

std::unordered_map<int, std::function<std::shared_ptr<ProtocolHandler>()>> EpollServer::dynamic_listeners_;
std::mutex EpollServer::dynamic_listeners_mutex_;

void EpollServer::registerDynamicListener(int fd, std::function<std::shared_ptr<ProtocolHandler>()> factory) {
    std::lock_guard<std::mutex> lock(dynamic_listeners_mutex_);
    dynamic_listeners_[fd] = factory;
}

bool EpollServer::isDynamicListener(int fd, std::function<std::shared_ptr<ProtocolHandler>()>& factory_out) {
    std::lock_guard<std::mutex> lock(dynamic_listeners_mutex_);
    auto it = dynamic_listeners_.find(fd);
    if (it != dynamic_listeners_.end()) {
        factory_out = it->second;
        return true;
    }
    return false;
}

void EpollServer::unregisterDynamicListener(int fd) {
    std::lock_guard<std::mutex> lock(dynamic_listeners_mutex_);
    dynamic_listeners_.erase(fd);
}

int EpollServer::make_socket_non_blocking(int sfd) {
    int flags = fcntl(sfd, F_GETFL, 0);
    if (flags == -1) return -1;
    flags |= O_NONBLOCK;
    if (fcntl(sfd, F_SETFL, flags) == -1) return -1;
    return 0;
}

void EpollServer::run() {
    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) return;

    for (auto& listener : listeners_) {
        struct epoll_event event;
        event.data.fd = listener.fd;
        event.events = EPOLLIN;
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listener.fd, &event);
        std::cout << "Epoll Server listening on port " << listener.port 
                  << " (Protocol: " << (int)listener.protocol << ")" << std::endl;
    }

    struct epoll_event events[MAX_EVENTS];
    std::unordered_map<int, std::shared_ptr<Connection>> connections;

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
                                   ", \"http_requests\": " + std::to_string(Metrics::getInstance().http_requests.load()) + 
                                   ", \"ftp_commands\": " + std::to_string(Metrics::getInstance().ftp_commands.load()) + 
                                   ", \"smtp_emails\": " + std::to_string(Metrics::getInstance().smtp_emails.load()) + 
                                   ", \"imap_commands\": " + std::to_string(Metrics::getInstance().imap_commands.load()) + 
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
                if (it->second->handler) it->second->handler->onDisconnect(it->second);
                it = connections.erase(it);
            } else {
                ++it;
            }
        }
        
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            
            bool is_listener = false;
            ProtocolType listener_protocol;
            bool listener_is_tls = false;
            std::function<std::shared_ptr<ProtocolHandler>()> dynamic_factory;
            
            for (auto& l : listeners_) {
                if (fd == l.fd) {
                    is_listener = true;
                    listener_protocol = l.protocol;
                    listener_is_tls = l.is_tls;
                    break;
                }
            }
            
            if (!is_listener) {
                if (isDynamicListener(fd, dynamic_factory)) {
                    is_listener = true;
                    listener_is_tls = false;
                    listener_protocol = ProtocolType::FTP_DATA; // placeholder to trigger dynamic factory branch
                }
            }
            
            if (is_listener) {
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
                        if (listener_protocol == ProtocolType::HTTP) {
                            std::string reject = "HTTP/1.1 429 Too Many Requests\r\nConnection: close\r\n\r\n";
                            write(infd, reject.c_str(), reject.length());
                        }
                        close(infd);
                        continue;
                    }

                    make_socket_non_blocking(infd);
                    
                    SSL* ssl = nullptr;
                    if (listener_is_tls) {
                        ssl = SSL_new(ctx_);
                        SSL_set_fd(ssl, infd);
                        SSL_set_accept_state(ssl); 
                    }

                    struct epoll_event event;
                    event.data.fd = infd;
                    event.events = EPOLLIN; 
                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, infd, &event) == -1) {
                        std::cerr << "epoll_ctl error for infd" << std::endl;
                        if (ssl) SSL_free(ssl);
                        close(infd);
                    } else {
                        auto conn = std::make_shared<Connection>(infd, acc_us, ssl);
                        
                        if (dynamic_factory) {
                            conn->handler = dynamic_factory();
                        } else if (listener_protocol == ProtocolType::HTTP) {
                            conn->handler = std::make_shared<HttpHandler>(router_);
                        } else if (listener_protocol == ProtocolType::SMTP) {
                            conn->handler = std::make_shared<SmtpHandler>();
                        } else if (listener_protocol == ProtocolType::FTP) {
                            conn->handler = std::make_shared<FtpHandler>();
                        } else if (listener_protocol == ProtocolType::IMAP) {
                            conn->handler = std::make_shared<ImapHandler>();
                        } else {
                            // Unsupported protocol
                            close(infd);
                            continue;
                        }
                        
                        conn->handler->onConnect(conn, epoll_fd);
                        connections[infd] = conn;
                        Metrics::getInstance().active_connections++;
                        Metrics::getInstance().new_connections++;
                    }
                }
            } else {
                if (connections.find(fd) == connections.end()) continue;
                std::shared_ptr<Connection> conn = connections[fd];
                
                if ((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP)) {
                    Metrics::getInstance().active_connections--;
                    if (conn->handler) conn->handler->onDisconnect(conn);
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
                        if (conn->handler) conn->handler->onDisconnect(conn);
                        connections.erase(fd);
                        continue;
                    }

                    if (conn->handler) {
                        conn->handler->onDataReceived(conn, thread_pool_, epoll_fd);
                        if (conn->marked_for_close) {
                            Metrics::getInstance().active_connections--;
                            conn->handler->onDisconnect(conn);
                            connections.erase(fd);
                            continue;
                        }
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
                        if (conn->handler) conn->handler->onDisconnect(conn);
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
                            if (conn->handler) conn->handler->onDisconnect(conn);
                            connections.erase(fd);
                        } else {
                            conn->timings.t_accept_us = 0; // reset for next request on keep-alive
                            struct epoll_event event;
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
    
    for (auto& l : listeners_) {
        close(l.fd);
    }
    close(epoll_fd);
}

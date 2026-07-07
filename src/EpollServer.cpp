#include "EpollServer.h"
#include "Connection.h"
#include "HttpParser.h"
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
    : port_(port), tls_port_(tls_port), router_(router), ctx_(ctx) {}

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
    std::unordered_map<int, Connection*> connections;

    std::cout << "Epoll Server listening on port " << port_ << " and TLS " << tls_port_ << "..." << std::endl;

    while (server_running) {
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, 1000);
        
        auto now = std::chrono::steady_clock::now();
        
        static auto last_sse_push = now;
        if (now - last_sse_push > std::chrono::seconds(1)) {
            std::string sse_data = "data: {\"active\": " + std::to_string(Metrics::getInstance().active_connections.load()) + 
                                   ", \"requests\": " + std::to_string(Metrics::getInstance().total_requests.load()) + "}\n\n";
            for (auto& pair : connections) {
                if (pair.second->is_sse) {
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
                delete it->second;
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
                    int infd = accept(fd, (struct sockaddr *)&in_addr, &in_len);
                    if (infd == -1) {
                        if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
                            break;
                        } else {
                            std::cerr << "accept error" << std::endl;
                            break;
                        }
                    }

                    make_socket_non_blocking(infd);
                    
                    SSL* ssl = nullptr;
                    if (fd == tls_server_fd) {
                        ssl = SSL_new(ctx_);
                        SSL_set_fd(ssl, infd);
                        SSL_set_accept_state(ssl); // Handle handshake transparently during SSL_read/write
                    }

                    event.data.fd = infd;
                    event.events = EPOLLIN; 
                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, infd, &event) == -1) {
                        std::cerr << "epoll_ctl error for infd" << std::endl;
                        if (ssl) SSL_free(ssl);
                        close(infd);
                    } else {
                        connections[infd] = new Connection(infd, ssl);
                        Metrics::getInstance().active_connections++;
                    }
                }
            } else {
                Connection* conn = connections[fd];
                
                if ((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP)) {
                    Metrics::getInstance().active_connections--;
                    delete conn;
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
                        delete conn;
                        connections.erase(fd);
                        continue;
                    }

                    bool should_close = false;
                    while (!conn->is_sse) {
                        ParseResult res = HttpParser::parse_incremental(*conn);
                        if (res == ParseResult::ERROR) {
                            should_close = true;
                            break;
                        }
                        if (res == ParseResult::INCOMPLETE) {
                            break;
                        }
                        if (res == ParseResult::COMPLETE) {
                            auto start_time = std::chrono::high_resolution_clock::now();
                            
                            conn->keep_alive = conn->current_request.isKeepAlive();
                            Response response = router_.route(conn->current_request);
                            response.headers["Connection"] = conn->keep_alive ? "keep-alive" : "close";
                            
                            conn->is_sse = response.is_sse;
                            
                            std::string serialized = response.serialize();
                            conn->write_queue.push(serialized);
                            
                            auto end_time = std::chrono::high_resolution_clock::now();
                            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
                            
                            Metrics::getInstance().total_requests++;
                            Metrics::getInstance().bytes_transferred += serialized.length();
                            
                            std::cout << "{\"method\": \"" << conn->current_request.method 
                                      << "\", \"path\": \"" << conn->current_request.path 
                                      << "\", \"status\": " << response.status_code 
                                      << ", \"latency_us\": " << duration 
                                      << "}" << std::endl;
                            
                            if (!conn->is_sse) {
                                conn->reset_for_next_request();
                            }
                            
                            event.data.fd = fd;
                            event.events = EPOLLIN | EPOLLOUT;
                            epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &event);
                        }
                    }
                    if (should_close) {
                        Metrics::getInstance().active_connections--;
                        delete conn;
                        connections.erase(fd);
                        continue;
                    }
                }

                if (events[i].events & EPOLLOUT) {
                    if (connections.find(fd) == connections.end()) continue; 
                    
                    bool write_error = false;
                    while (!conn->write_queue.empty()) {
                        std::string& data = conn->write_queue.front();
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
                        
                        if (count == (ssize_t)data.length()) {
                            conn->write_queue.pop();
                        } else {
                            data = data.substr(count);
                            break;
                        }
                        conn->update_activity();
                    }

                    if (write_error) {
                        Metrics::getInstance().active_connections--;
                        delete conn;
                        connections.erase(fd);
                        continue;
                    }

                    if (conn->write_queue.empty()) {
                        if (!conn->keep_alive) {
                            Metrics::getInstance().active_connections--;
                            delete conn;
                            connections.erase(fd);
                        } else {
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
    for (auto& pair : connections) {
        delete pair.second;
    }
    connections.clear();
    Metrics::getInstance().active_connections = 0;
    
    close(server_fd);
    close(tls_server_fd);
    close(epoll_fd);
}

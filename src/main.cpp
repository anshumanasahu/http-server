#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <string>
#include <thread>
#include <sstream>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include "Connection.h"
#include "HttpParser.h"
#include "Router.h"
#include "ThreadPool.h"
#include "EpollServer.h"
#include "Metrics.h"
#include <signal.h>
#include <chrono>

volatile sig_atomic_t server_running = 1;

void signal_handler(int signum) {
    std::cout << "\nCaught signal " << signum << ". Initiating graceful shutdown..." << std::endl;
    server_running = 0;
    
    std::thread shutdown_timer([]() {
        auto start = std::chrono::steady_clock::now();
        while (Metrics::getInstance().active_connections > 0) {
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(5)) {
                std::cerr << "Shutdown deadline reached! Force exiting with " 
                          << Metrics::getInstance().active_connections << " active connections." << std::endl;
                exit(1);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
    shutdown_timer.detach();
}

void init_openssl() {
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();
}

void cleanup_openssl() {
    EVP_cleanup();
}

SSL_CTX* create_context() {
    const SSL_METHOD *method = SSLv23_server_method();
    SSL_CTX *ctx = SSL_CTX_new(method);
    if (!ctx) {
        std::cerr << "Unable to create SSL context" << std::endl;
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }
    return ctx;
}

void configure_context(SSL_CTX *ctx) {
    if (SSL_CTX_use_certificate_file(ctx, "certs/cert.pem", SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, "certs/key.pem", SSL_FILETYPE_PEM) <= 0 ) {
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }
}

void handle_connection(int client_socket, Router& router, SSL_CTX* ctx, bool is_tls) {
    SSL* ssl = nullptr;
    if (is_tls) {
        ssl = SSL_new(ctx);
        SSL_set_fd(ssl, client_socket);
        if (SSL_accept(ssl) <= 0) {
            ERR_print_errors_fp(stderr);
            SSL_free(ssl);
            close(client_socket);
            return;
        }
    }
    struct timeval timeout;
    timeout.tv_sec = 10;
    timeout.tv_usec = 0;
    setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(client_socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    Metrics::getInstance().active_connections++;

    Connection conn(client_socket, ssl);
    while (server_running && conn.keep_alive) {
        auto start_time = std::chrono::high_resolution_clock::now();
        // Try parsing whatever is already in the buffer
        ParseResult res = HttpParser::parse_incremental(conn);
        
        if (res == ParseResult::ERROR) {
            break; // Malformed request
        }
        
        if (res == ParseResult::COMPLETE) {
            std::cout << "Parsed Request - Method: [" << conn.current_request.method << "] Path: [" << conn.current_request.path << "]" << std::endl;
            
            conn.keep_alive = conn.current_request.isKeepAlive();
            
            // Route and generate response
            Response response = router.route(conn.current_request);
            response.headers["Connection"] = conn.keep_alive ? "keep-alive" : "close";
            
            std::string response_str = response.serialize();
            
            // Send response
            conn.conn_write(response_str.c_str(), response_str.length());
            
            auto end_time = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
            
            Metrics::getInstance().total_requests++;
            Metrics::getInstance().bytes_transferred += response_str.length();
            
            // JSON Access Log
            std::cout << "{\"method\": \"" << conn.current_request.method 
                      << "\", \"path\": \"" << conn.current_request.path 
                      << "\", \"status\": " << response.status_code 
                      << ", \"latency_us\": " << duration 
                      << "}" << std::endl;
            
            conn.reset_for_next_request();
            continue; // Parse next request if pipelined
        }
        
        // INCOMPLETE: read more data
        char buffer[4096] = {0};
        int valread = conn.conn_read(buffer, 4096);
        
        if (valread <= 0) {
            break; // Client disconnected or read error
        }
        
        conn.read_buf.insert(conn.read_buf.end(), buffer, buffer + valread);
    }

    // Close the connection
    Metrics::getInstance().active_connections--;
    // socket closed by Connection RAII
}

int main(int argc, char* argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    init_openssl();
    SSL_CTX *ctx = create_context();
    configure_context(ctx);
    std::string concurrency_mode = "thread-pool";
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.find("--concurrency=") == 0) {
            concurrency_mode = arg.substr(14);
        }
    }

    int server_fd, new_socket;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);
    const int PORT = 8080;
    const int TLS_PORT = 8443;

    Router router;
    router.setStaticDirectory("./public");
    router.addRoute("GET", "/api/status", [](const Request& req) {
        return Response(200, "{\"status\": \"ok\", \"service\": \"http-server\"}\n");
    });
    router.addRoute("GET", "/api/metrics", [](const Request& req) {
        std::ostringstream ss;
        ss << "{\n"
           << "  \"total_requests\": " << Metrics::getInstance().total_requests << ",\n"
           << "  \"active_connections\": " << Metrics::getInstance().active_connections << ",\n"
           << "  \"bytes_transferred\": " << Metrics::getInstance().bytes_transferred << "\n"
           << "}\n";
        Response res(200, ss.str());
        res.headers["Content-Type"] = "application/json";
        return res;
    });
    router.addRoute("POST", "/api/echo", [](const Request& req) {
        Response res(200, req.body);
        res.headers["Content-Type"] = "application/json";
        return res;
    });
    router.addProxyRoute("/proxy", {
        {"host.docker.internal", 9090},
        {"host.docker.internal", 9091}
    });

    if (concurrency_mode == "epoll") {
        std::cout << "Starting server in epoll mode on port " << PORT << " and TLS " << TLS_PORT << std::endl;
        EpollServer epoll_server(PORT, TLS_PORT, router, ctx);
        epoll_server.run();
        cleanup_openssl();
        return 0;
    }

    // Thread pool setup
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        std::cerr << "Socket creation failed" << std::endl;
        return 1;
    }

    // 2. Attach socket to the port 8080
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        std::cerr << "setsockopt failed" << std::endl;
        return 1;
    }
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    // 3. Bind the socket to the network address and port
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        std::cerr << "Bind failed" << std::endl;
        return 1;
    }

    // 4. Listen for incoming connections
    if (listen(server_fd, 10) < 0) {
        std::cerr << "Listen failed" << std::endl;
        return 1;
    }

    // TLS Server setup
    int tls_server_fd;
    if ((tls_server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        std::cerr << "TLS Socket creation failed" << std::endl;
        return 1;
    }
    if (setsockopt(tls_server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        std::cerr << "TLS setsockopt failed" << std::endl;
        return 1;
    }
    struct sockaddr_in tls_address;
    tls_address.sin_family = AF_INET;
    tls_address.sin_addr.s_addr = INADDR_ANY;
    tls_address.sin_port = htons(TLS_PORT);
    if (bind(tls_server_fd, (struct sockaddr *)&tls_address, sizeof(tls_address)) < 0) {
        std::cerr << "TLS Bind failed" << std::endl;
        return 1;
    }
    if (listen(tls_server_fd, 10) < 0) {
        std::cerr << "TLS Listen failed" << std::endl;
        return 1;
    }

    size_t num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;
    size_t max_queue_size = 100;
    
    ThreadPool http_pool(num_threads, max_queue_size, [&router, ctx](int client_socket) {
        handle_connection(client_socket, router, ctx, false);
    });
    
    ThreadPool https_pool(num_threads, max_queue_size, [&router, ctx](int client_socket) {
        handle_connection(client_socket, router, ctx, true);
    });

    std::cout << "Server listening on port " << PORT << " and TLS port " << TLS_PORT << " with " << num_threads << " worker threads each..." << std::endl;

    std::thread tls_accept_thread([&]() {
        while (server_running) {
            int new_tls_socket;
            int tls_addrlen = sizeof(tls_address);
            if ((new_tls_socket = accept(tls_server_fd, (struct sockaddr *)&tls_address, (socklen_t*)&tls_addrlen)) < 0) {
                continue;
            }
            if (!https_pool.enqueue(new_tls_socket)) {
                std::string overload_response = "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n\r\n";
                write(new_tls_socket, overload_response.c_str(), overload_response.length());
                close(new_tls_socket);
            }
        }
    });

    while (server_running) {
        if ((new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
            if (!server_running) break;
            std::cerr << "Accept failed" << std::endl;
            continue;
        }

        if (!http_pool.enqueue(new_socket)) {
            std::cerr << "Thread pool queue is full, rejecting connection." << std::endl;
            std::string overload_response = "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n\r\n";
            send(new_socket, overload_response.c_str(), overload_response.length(), 0);
            close(new_socket);
        }
    }

    std::cout << "Waiting for connections to close..." << std::endl;
    close(server_fd);
    close(tls_server_fd);

    tls_accept_thread.join();
    cleanup_openssl();
    std::cout << "Shutdown complete." << std::endl;
    return 0;
}

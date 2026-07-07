#pragma once

#include <string>
#include <vector>
#include <queue>
#include <chrono>
#include <unistd.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include "Request.h"

enum class ParseState {
    REQUEST_LINE,
    HEADERS,
    BODY_CONTENT_LENGTH,
    BODY_CHUNKED,
    COMPLETE,
    ERROR
};

enum class ParseResult {
    INCOMPLETE,
    COMPLETE,
    ERROR
};

struct Connection {
    int fd;
    std::vector<char> read_buf;
    std::queue<std::string> write_queue;
    
    ParseState parse_state;
    Request current_request;
    bool keep_alive;
    
    std::chrono::steady_clock::time_point last_activity;
    size_t headers_bytes_seen;
    size_t header_count;
    
    // Chunked parsing state
    int chunk_bytes_left; 
    bool reading_chunk_size;
    
    // TLS state
    SSL* tls_session;

    Connection(int socket_fd, SSL* ssl = nullptr) 
        : fd(socket_fd), 
          parse_state(ParseState::REQUEST_LINE), 
          keep_alive(true),
          headers_bytes_seen(0),
          header_count(0),
          chunk_bytes_left(0),
          reading_chunk_size(true),
          tls_session(ssl) {
        update_activity();
    }
    
    ~Connection() {
        if (tls_session) {
            SSL_shutdown(tls_session);
            SSL_free(tls_session);
        }
        if (fd >= 0) {
            close(fd);
        }
    }
          
    void update_activity() {
        last_activity = std::chrono::steady_clock::now();
    }
    
    ssize_t conn_read(char* buf, size_t count) {
        if (tls_session) {
            return SSL_read(tls_session, buf, count);
        }
        return read(fd, buf, count);
    }
    
    ssize_t conn_write(const char* buf, size_t count) {
        if (tls_session) {
            return SSL_write(tls_session, buf, count);
        }
        return write(fd, buf, count);
    }

    void reset_for_next_request() {
        parse_state = ParseState::REQUEST_LINE;
        current_request = Request();
        headers_bytes_seen = 0;
        header_count = 0;
        chunk_bytes_left = 0;
        reading_chunk_size = true;
        update_activity();
    }
};

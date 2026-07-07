# Project Context & Progress Tracker

This document tracks the ongoing development of the C++ HTTP/1.1 Server.

## 🚀 Progress So Far

- **Environment Setup:** Configured Docker and CMake.
- **Socket Foundations:** Implemented raw TCP sockets.
- **Modular Architecture & Routing:** Separated `Request`, `Response`, `HttpParser`, and `Router`.
- **Full HTTP/1.1 Parsing & Chunked Transfer:** Implemented incremental parsing and chunked encoding.
- **Concurrency (Thread Pool & Epoll):** Completed dual-engine concurrency.
- **Slowloris Protection (Timeouts):** Implemented read/write timeouts and max header sizes.
- **TLS / HTTPS:** Integrated OpenSSL for secure connections.

## 🎯 Upcoming Milestones (from PRD)

The following tasks represent the immediate next steps to bring the server up to standard:

- [x] **Full HTTP/1.1 Parsing:** Extract and parse headers properly. Implement body parsing based on `Content-Length`.
- [x] **Chunked Transfer Encoding:** Add support for chunked bodies and `Connection: keep-alive`.
- [x] **Concurrency (Thread Pool):** Move away from blocking single-thread architecture by implementing a fixed-size worker pool pulling from a connection queue.
- [x] **Concurrency (epoll Event Loop):** Implement a single-threaded multiplexing event loop and benchmark it against the thread pool.
- [x] **Slowloris Protection (Timeouts):** Add read/write timeouts and maximum header sizes to prevent slow clients from exhausting connections.
- [x] **TLS / HTTPS:** Wrap sockets with OpenSSL to accept secure connections.
- [x] **Static File Serving:** Serve static files from a directory while explicitly preventing directory traversal attacks (`../`).
- [x] **Reverse Proxy Mode:** Add logic to forward incoming requests to external backend servers.
- [x] **Compression:** Add gzip/deflate support for clients that advertise it via `Accept-Encoding`.
- [x] **Observability & Graceful Shutdown:** Add JSON access logs, a `/metrics` endpoint, and proper `SIGTERM` cleanup handling.

# C++ HTTP/1.1 Server

A fully operational, high-performance HTTP/1.1 server built entirely from scratch using raw C++ sockets and `sys/epoll.h`.

## Features
- **Dual Concurrency Engines:** Run the server using either a `ThreadPool` or a non-blocking `epoll` event loop.
- **TLS / HTTPS:** Built-in OpenSSL support for secure connections.
- **Reverse Proxy:** Forward incoming requests directly to a backend API.
- **Static File Serving:** Serve local HTML/JS/CSS files with path traversal security.
- **Chunked Transfer Encoding & Gzip:** Supports streaming large bodies and compressing responses via ZLIB.
- **Observability:** Exposes live metrics at `/api/metrics` and logs JSON access logs.
- **Slowloris Defense:** Implements connection timeouts and maximum header constraints.

## How to Run
The server is bundled using Docker to ensure cross-platform compatibility (especially for `epoll` which is Linux-exclusive).

1. Build and start the container in detached mode:
   ```bash
   docker-compose up -d --build
   ```

2. Watch the live access logs:
   ```bash
   docker logs -f httpserver-server-1
   ```

3. To shut down the server gracefully:
   ```bash
   docker stop httpserver-server-1
   ```

## Available Endpoints & Testing

### 1. Basic Routing (Plaintext HTTP)
```bash
curl -i http://localhost:8080/api/status
```

### 2. TLS / HTTPS Encryption
The server automatically maps port `8443` for HTTPS traffic (using self-signed certs).
```bash
curl -k -i https://localhost:8443/api/status
```

### 3. Gzip Compression
Request a large static file and ask for compression.
```bash
curl -k -i -H "Accept-Encoding: gzip" --compressed https://localhost:8443/large.txt
```

### 4. Metrics Dashboard
View the live JSON dashboard tracking total requests and active connections.
```bash
curl -k -i https://localhost:8443/api/metrics
```

### 5. Static File Serving
View the default `index.html` page.
```bash
curl -k -i https://localhost:8443/
```

### 6. Reverse Proxy Mode
Forward an HTTP request through the `/proxy` route. (By default, this forwards to `host.docker.internal:9090`).
```bash
curl -k -i https://localhost:8443/proxy
```

## Configuration
To switch between `thread-pool` and `epoll` mode, modify `docker-compose.yml`:
```yaml
command: bash -c "... && ./build/http-server --concurrency=epoll"
```

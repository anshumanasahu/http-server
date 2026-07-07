# Core Architecture: High-Performance C++ HTTP Server

This document explains the inner workings of this HTTP server. Rather than a standard "how to run" guide (see `README.md`), this file breaks down the exact system-level design decisions that allow this server to handle thousands of concurrent connections efficiently.

---

## 1. The Concurrency Model: Epoll Event Loop

Traditional web servers (like older versions of Apache) often use a **Thread-per-Connection** model. If 10,000 users connect, the server spawns 10,000 threads. This leads to massive RAM overhead and CPU context-switching, eventually crashing the server under heavy load.

This server is built on the **Epoll Event Loop** (`src/EpollServer.cpp`), a Linux-specific system call designed for immense scalability:
1. All client sockets are set to **non-blocking mode**.
2. The server registers all active sockets with the kernel's `epoll` instance.
3. A single, dedicated thread runs an infinite loop (`epoll_wait`). It sleeps until the kernel wakes it up, notifying it *exactly* which sockets have bytes ready to read or write.
4. The server instantly processes those specific sockets and goes right back to waiting.

By using `epoll`, this server can maintain tens of thousands of idle connections (like long-polling or Slowloris attacks) while using almost zero CPU and minimal memory, because it never spawns threads for idle users.

---

## 2. The HTTP Parser: Incremental State Machine

Because sockets are non-blocking, a client might send an HTTP request in tiny, fragmented chunks (e.g., sending `GET /` and then pausing for 2 seconds before sending `HTTP/1.1`). 

If the server waited for the whole string, it would block the event loop and freeze the entire server. Instead, `src/HttpParser.cpp` uses an **Incremental State Machine**. 
Every connection tracks its own state:
- `REQUEST_LINE`
- `HEADERS`
- `BODY`

When bytes arrive, the parser reads whatever is available, updates its state, and returns `INCOMPLETE` if it needs more data. The `epoll` loop moves on to the next user, coming back to finish parsing only when the next chunk of bytes arrives.

---

## 3. The Router & Static SPA Fallback

The `Router` (`src/Router.cpp`) is responsible for resolving the parsed `Request` into a `Response`.
It features a **Single Page Application (SPA) Fallback Mechanism**:

1. **API Endpoints:** It checks if the request matches a registered route (like `GET /api/status`).
2. **Static Assets:** If no route matches, it attempts to securely resolve the path (preventing directory traversal attacks) and serve a file from the `/public` directory (e.g., serving `app.js` or `style.css`).
3. **The Fallback:** If the client requests a path like `/dashboard` (which doesn't exist on the filesystem), the Router intercepts the 404. Since it is not an `/api/` route, the Router intentionally falls back to serving `index.html`. This allows modern frontend frameworks (like React or vanilla JS with client-side routing) to take over the URL logic without the C++ backend breaking.

---

## 4. V2 Additions: Live Metrics & SSE

To visualize the server's immense speed, the server features a real-time dashboard powered by **Server-Sent Events (SSE)**.
Unlike traditional HTTP requests that close immediately after the response is sent, SSE intentionally keeps the socket open.

- **Timeout Exemption:** The `epoll` loop features a `Timeout Guard` that aggressively kills idle connections after 10 seconds. SSE connections are specifically flagged (`is_sse = true`) to bypass this guard.
- **The Heartbeat:** Every 1 second, the `epoll` loop queries the `Metrics` singleton, serializes the active connection count and requests-per-second into a JSON string, and pushes it directly into the `write_queue` of every open SSE socket.

---

## 5. The Chaos Sandbox: Self-Imposed DoS

The project includes built-in endpoints designed to attack itself, proving its resilience in real-time.

### The Traffic Flood (`/api/chaos/flood`)
When triggered, the server spawns 50 detached C++ threads that act as malicious clients. They rapidly open local TCP sockets and bombard the `epoll` loop with thousands of valid HTTP requests. Because `epoll` has virtually zero context-switching overhead, the server processes these floods effortlessly, maintaining microsecond-level latency.

### The Slowloris Attack (`/api/chaos/slowloris`)
Slowloris is designed to exhaust server sockets by sending 1 byte every few seconds, preventing the request from ever finishing. 
When triggered, 100 detached threads open connections and trickle garbage data. 
**The Defense:** The `Timeout Guard` constantly sweeps all connections. Because the Slowloris threads fail to send a complete HTTP request within the strict 10-second window, the C++ server aggressively severs the sockets, freeing up the resources and neutralizing the attack automatically.

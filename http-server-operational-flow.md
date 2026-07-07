# HTTP Server Operational Flow

**Companion to:** `http-server-prd.md`, `http-server-trd.md`
**Purpose:** Trace exactly what happens, step by step, from a TCP connection landing on the listener to a response leaving the wire — across both concurrency models, both serving modes, and the operational layers (timeouts, TLS, compression, logging, shutdown) wrapped around them.

---

## 1. Connection lifecycle (outer shell)

One server binary, runtime-configured into a concurrency mode (thread pool or epoll) and a serving mode (static/routed or reverse proxy). Every connection — regardless of mode — passes through the same outer shell before it ever reaches the HTTP parser.

```
 Client opens TCP connection
        │
        ▼
 accept() loop
        │  accepts on listening socket (plaintext or TLS port)
        ▼
 TLS wrap (if TLS port)
        │  SSL_accept / rustls handshake before any bytes reach the parser
        ▼
 Timeout guard
        │  starts read-timeout clock, tracks headers_bytes_seen
        ▼
 Concurrency model
        │  thread pool queue  OR  epoll registration (EPOLLIN)
        ▼
 HTTP/1.1 parser
        │  incremental state machine, fed as bytes arrive
        ▼
 Handler (static/routed OR reverse proxy)
        │
        ▼
 Response serialized to write_queue
        │
        ▼
 Access log line written
        │
        ▼
 Keep-alive? ── yes ──▶ reset parser, loop back to "Timeout guard"
        │
        no
        ▼
 Connection closed
```

The `Connection` struct (fd, optional TLS session, read buffer, parse state, write queue, keep-alive flag, last-activity timestamp, headers-bytes-seen counter) is what's threaded through every stage below — it's the same struct regardless of which concurrency model is driving it, which is what makes the thread-pool-vs-epoll benchmark apples-to-apples.

---

## 2. Concurrency model internals

The two models diverge here, but both hand off into the identical parser/handler core in §3–4.

### 2.1 Thread pool

```
 accept() loop (dedicated thread)
        │  pushes accepted connection onto bounded ConcurrentQueue
        ▼
 Queue full? ── yes ──▶ reject immediately (503-equivalent close)
        │
        no
        ▼
 Idle worker wakes on condvar
        │  pops connection, owns it for its full lifetime while active
        ▼
 Worker loop: parse → handle → respond → check keep-alive → repeat
        │  (blocking I/O; connection stays with this worker across
        │   keep-alive requests rather than returning to the queue)
        ▼
 Connection closes → worker returns to idle, blocks on condvar again
```

### 2.2 epoll event loop

```
 epoll_create1() once at startup
        ▼
 loop { epoll_wait() }
        │
        ├── event.fd == listener_fd
        │        accept new connection(s), set non-blocking,
        │        epoll_ctl ADD (EPOLLIN)
        │
        ├── EPOLLIN on a connection fd
        │        read available bytes → feed parser (§3)
        │        request complete? → handle → enqueue response
        │        response not fully flushed? → epoll_ctl MOD to watch EPOLLOUT
        │
        ├── EPOLLOUT on a connection fd
        │        flush write_queue
        │        fully flushed? → epoll_ctl MOD back to EPOLLIN-only
        │                         (or close if not keep-alive)
        │
        └── EPOLLHUP / EPOLLERR
                 close connection, cleanup
```

Single thread, all sockets non-blocking, level-triggered epoll (re-fires as long as data is available/writable — the deliberately simpler choice for v1 over edge-triggered). No handler in this path may block, which is why static file reads and handler logic have to stay fast or non-blocking.

Mode selection is a single runtime flag (`--concurrency=thread-pool|epoll`) that swaps which of these two loops feeds the shared `Connection`/parser/handler code — that shared core is the entire point of the benchmark.

---

## 3. HTTP/1.1 parser (incremental state machine)

Fed byte-by-byte or chunk-by-chunk as `read()` returns data — never assumes a complete request arrives in one call. Identical under both concurrency models.

```
 REQUEST_LINE
        │  scan for \r\n → split method/path/version
        │  reject unsupported methods/versions → ERROR
        ▼
 HEADERS
        │  scan line-by-line for \r\n, "Name: Value"
        │  blank line ends headers
        │  headers_bytes_seen tracked against max-header-size
        │  DURING parsing, not after (§5 enforces the cutoff)
        ▼
 Body framing decided from headers:
        │
        ├── Content-Length: N present ──▶ BODY_CONTENT_LENGTH
        │                                  accumulate exactly N bytes
        │
        ├── Transfer-Encoding: chunked ──▶ BODY_CHUNKED
        │                                  repeat: <hex-size>\r\n<data>\r\n
        │                                  until 0\r\n\r\n terminator
        │                                  (trailers parsed and discarded)
        │
        └── neither, method has no body (GET) ──▶ COMPLETE immediately
        ▼
 COMPLETE
        │
        ▼
 Connection: keep-alive (default) and no Connection: close?
        │
        ├── yes ──▶ reset parser state for next request on same connection
        │            (any pipelined bytes already in read_buf feed directly
        │             into the next parse cycle)
        │
        └── no  ──▶ connection closes after response is sent
```

`ERROR` state (malformed request line, missing required framing) short-circuits straight to an error response and connection close — never a crash.

---

## 4. Handler dispatch: static/routed vs. reverse proxy

Once the parser produces `COMPLETE`, the request is dispatched to exactly one of two handler paths, chosen by server serving-mode config (not per-request):

```
 Parsed request
        │
        ├── Serving mode = static/routed ─────────────┐
        │                                              ▼
        │                                    Path-traversal check
        │                                    (reject ../ escapes)
        │                                              │
        │                                    Route table lookup
        │                                    (dynamic endpoint match?)
        │                                              │
        │                                    ├── match ──▶ dynamic handler
        │                                    └── no match ──▶ serve file
        │                                              │
        │                                              ▼
        │                                    Compression check (§5)
        │
        └── Serving mode = reverse proxy ──────────────┐
                                                         ▼
                                              Backend selection (§6)
                                                         │
                                                         ▼
                                              Forward request, stream
                                              backend response back
                                                         │
                                                         ▼
                                              Compression check (§5)
                                              (only if not already encoded)
```

---

## 5. Slowloris defense / timeout guard

Runs continuously alongside every stage above, not as a discrete pipeline step — but worth tracing on its own since it's the direct answer to "how do you stop a slow client from starving other connections."

```
 Connection accepted, last_activity = now
        │
        ▼
 Waiting for complete request line/headers?
        │
        ├── now - last_activity > READ_TIMEOUT (10s) ──▶ close connection
        │
        ▼
 headers_bytes_seen exceeds max header size (8KB)
 or max header count (100) DURING parsing
        │
        ├── yes ──▶ reject with 431-equivalent, close immediately
        │            (checked incrementally — never buffer-then-reject)
        ▼
 Body being read (Content-Length case), throughput-scaled timeout (30s)
        │
        ├── exceeded ──▶ close connection
        ▼
 Response being written, client not draining write buffer
        │
        ├── now - last_write_progress > WRITE_TIMEOUT (30s) ──▶ close
        ▼
 Request completes normally within all windows ──▶ proceed
```

Enforcement mechanism differs by concurrency model (thread pool: per-connection timer or watchdog sweep; epoll: `epoll_wait` timeout plus a min-heap of deadlines checked each loop iteration) but the policy — and the defaults above — are identical either way.

---

## 6. Reverse proxy backend selection

```
 Request arrives, serving mode = proxy
        ▼
 Select backend per configured strategy
        │
        ├── round-robin ──▶ next_rr_index++ % len(backends),
        │                    skipping any marked unhealthy
        │
        └── least-connections (stretch) ──▶ pick min active_connections
                                              among healthy backends
        ▼
 Open (or reuse pooled) connection to selected backend
        ▼
 Forward request line/headers/body, adjusting Host header
        ▼
 Backend reachable?
        │
        ├── no (connect failure or repeated request failure)
        │        │
        │        ▼
        │   Mark backend unhealthy, retry against another backend
        │        (periodic or passive re-check brings it back into
        │         rotation later)
        │
        └── yes
                 ▼
        Stream backend response back to client through the
        same write_queue mechanism as any local response
```

---

## 7. Compression

Applied uniformly after a handler (static file or proxy) produces a body, before final serialization to `write_queue`:

```
 Response body produced (static file or proxied)
        ▼
 Already Content-Encoding'd? (e.g. backend pre-compressed it)
        │
        ├── yes ──▶ skip compression, pass through as-is
        ▼
 Client sent Accept-Encoding: gzip?
        │
        ├── no ──▶ skip compression
        ▼
 Content-Type in compressible list (text/*, application/json)
 AND body size exceeds minimum threshold?
        │
        ├── no ──▶ skip compression
        ▼
 Compress via zlib/miniz
        ▼
 Set Content-Encoding: gzip, update Content-Length
        ▼
 Serialize to write_queue
```

---

## 8. Observability

Runs on the same worker/loop iteration that completes a response — no separate logging thread required at this scale.

```
 Response fully written to client
        ▼
 Access log line emitted (JSON):
        { timestamp, method, path, status, latency_ms,
          client_ip, bytes_sent }
        ▼
 Metrics counters updated:
        active_connections, requests_total,
        thread_pool_queue_depth (or epoll_watched_fds),
        latency histogram (feeds p99 reported at /metrics)
```

`/metrics` is a pull-based endpoint read independently of this flow — it just reflects whatever the counters above currently hold, which is why queue depth or fd count should visibly track load during a benchmark.

---

## 9. Graceful shutdown

Triggered externally (`SIGTERM`), overrides the normal accept loop but leaves in-flight connections alone until the deadline.

```
 SIGTERM received
        ▼
 Set global shutting_down flag
        ▼
 Stop accept()-ing new connections immediately
 (close listening socket, or thread pool/epoll simply stops
  registering the listener fd)
        ▼
 In-flight connections continue to completion:
        │
        ├── thread pool: workers finish current request/response
        │                 cycle, don't pull new work from queue
        │
        └── epoll: loop keeps servicing already-open fds,
                    listener fd no longer registered
        ▼
 Shutdown deadline timer running in parallel
        │
        ├── all connections close before deadline ──▶ exit cleanly
        │
        └── deadline fires first ──▶ force-close remaining
                                      connections, log a warning
                                      for each, then exit
```

---

## 10. How the flows compose end-to-end

A single keep-alive HTTPS request to a static file, with compression, touches nearly every flow above in this order:

1. **§1** — client connects to the TLS port; `accept()` fires; TLS handshake wraps the raw fd before any HTTP bytes are read.
2. **§1 / §5** — timeout guard starts its read-timeout clock for this connection.
3. **§2** — the connection is handed to whichever concurrency model is active (queued to a worker, or registered with epoll).
4. **§3** — parser accumulates bytes incrementally; request line and headers parsed; no body for a `GET`, so `COMPLETE` fires immediately.
5. **§5** — if header bytes had exceeded the 8KB/100-header limits at any point during step 4, the connection would have been rejected right there instead of reaching this step.
6. **§4** — serving mode is static/routed; path-traversal check passes; no route match, so the file is served from disk.
7. **§7** — client sent `Accept-Encoding: gzip`, `Content-Type: text/html` is compressible, body exceeds the size threshold → body is gzip-compressed, `Content-Encoding`/`Content-Length` updated.
8. **§1** — response serialized to `write_queue`; partial writes re-enqueue the remainder until fully flushed.
9. **§8** — access log line and metrics counters update.
10. **§3** — `Connection: keep-alive` present → parser resets, connection loops back to step 3 for the next request instead of closing.
11. If a `SIGTERM` arrives mid-session, **§9** takes over: this connection is allowed to finish its current request/response cycle, but no new connections are accepted, and the process won't exit until this one closes or the shutdown deadline fires — whichever comes first.

---

## 11. Open questions this flow surfaces (worth resolving before Milestone 8)

- Does the read-timeout clock reset on every byte received (rolling window), or only at state transitions (request line complete, headers complete)? The TRD implies rolling via `last_activity`, but it's worth pinning down explicitly since it changes how forgiving the Slowloris defense is.
- For the epoll model's timeout sweep (min-heap of deadlines), how often does the sweep run relative to `epoll_wait`'s own timeout — is staleness checked every iteration or only when `epoll_wait` itself times out with no events?
- In reverse proxy mode, does a backend health re-check happen on a fixed interval (active) or only opportunistically on the next request routed its way (passive) — the TRD leaves this as an either/or without a stated default.

---

## 12. V2: Visual Dashboard Flow Mechanics

Once V2 (the Live Command Center) is introduced, two new significant operational flows occur:

### 12.1 Server-Sent Events (SSE) Metric Streaming
```text
 React Frontend (Browser)
        │
        ▼
 GET /api/metrics/stream
        │
        ▼
 C++ Router matches route ──▶ Upgrades connection state to SSE
        │
        ▼
 Timeout Guard (§5) ──▶ Exempts this fd from standard WRITE_TIMEOUT
        │
        ▼
 Background Metrics Thread (fires every 1s)
        │
        ▼
 Serializes live JSON stats ──▶ Pushes into Connection's write_queue
        │
        ▼
 epoll loop flushes write_queue ──▶ Browser receives real-time update
```

### 12.2 SPA Static Routing Flow
```text
 User refreshes browser at https://server/dashboard/proxy-manager
        │
        ▼
 C++ Router parses request path: /dashboard/proxy-manager
        │
        ├── Is it an /api/* route? ──▶ No.
        ├── Does file exist in /public/dashboard/proxy-manager? ──▶ No.
        │
        ▼
 SPA Fallback Triggered
        │
        ▼
 Rewrites internal target to /public/index.html
        │
        ▼
 Serves index.html with 200 OK ──▶ React Router takes over on client-side
```

# Technical Requirement Document
## HTTP/1.1 Web Server from Scratch (Socket-Level)

**Author:** Anshuman
**Status:** Draft v1
**Companion doc:** `http-server-prd.md`
**Last updated:** July 2026

This document specifies *how* the system in the PRD gets built — connection state machines, parser design, concurrency internals, and infrastructure. Section numbers below map to the PRD's layer numbers (L1–L12) where applicable.

---

## 1. System Overview

A single server binary, runtime-configurable into one of two concurrency modes (thread pool or epoll event loop) and one of two serving modes (static/routed file server or reverse proxy). TLS is a transport-layer wrapper applied before the HTTP parser regardless of which concurrency or serving mode is active.

```
                    ┌────────────────────────────┐
  accept() loop ──▶ │  Connection (raw or TLS)     │
                    └─────────────┬──────────────┘
                                  │
                    ┌─────────────▼──────────────┐
                    │  Timeout Guard (L8)          │
                    │  read/write deadlines,        │
                    │  max header size               │
                    └─────────────┬──────────────┘
                                  │
        ┌─────────────────────────┼─────────────────────────┐
        ▼                                                     ▼
┌───────────────┐                                   ┌────────────────┐
│ Thread Pool     │                                   │ epoll Event     │
│ (L3)            │                                   │ Loop (L4)        │
└───────┬────────┘                                   └────────┬────────┘
        └─────────────────────────┬─────────────────────────┘
                                  ▼
                    ┌─────────────────────────────┐
                    │  HTTP/1.1 Parser (L2)         │
                    │  (incremental state machine)   │
                    └─────────────┬──────────────┘
                                  │
                ┌──────────────────┼──────────────────┐
                ▼                                      ▼
      ┌──────────────────┐                  ┌────────────────────┐
      │ Static/Routed      │                  │ Reverse Proxy (L9)   │
      │ Serving (L5)        │                  │ round-robin backends │
      │ + Compression (L10) │                  └────────────────────┘
      └──────────────────┘
                                  │
                    ┌─────────────▼──────────────┐
                    │  Access Log + Metrics (L11)   │
                    └─────────────────────────────┘
```

---

## 2. Connection Buffering & Partial I/O (PRD L1)

Each connection owns a growable read buffer and a write buffer/queue — never assume a single `read()`/`write()` call is complete.

```cpp
struct Connection {
  int fd;
  std::unique_ptr<SSL, decltype(&SSL_free)> tls_session; // present iff HTTPS listener (L7)
  std::vector<char> read_buf;                            // accumulates until a full request is parseable
  HttpParseState parse_state;                            // see §3
  std::queue<std::vector<char>> write_queue;             // pending response chunks (partial writes re-enqueue remainder)
  bool keep_alive;
  std::chrono::steady_clock::time_point last_activity;   // for timeout guard, §5
  size_t headers_bytes_seen;                             // for max-header-size enforcement, §5
  
  ~Connection() { if (fd >= 0) close(fd); }              // RAII socket cleanup
};
```
**Read loop:** `read()` into `read_buf` at the current offset; feed newly-read bytes into the incremental parser (§3); if the parser reports "incomplete," return to the event loop/thread pool and wait for more data — never block waiting for a full request in one call.
**Write loop:** attempt to flush `write_queue`; on a partial write (`write()` returns fewer bytes than requested, or `EAGAIN` in non-blocking/epoll mode), keep the unwritten remainder queued and retry on the next writable event (epoll) or next loop iteration (thread pool, blocking sockets — see §4.2 for the mode-specific handling).

---

## 3. HTTP/1.1 Parser (PRD L2)

Implemented as an **incremental state machine** (not "wait for full buffer then parse") so it works identically under both concurrency models and can report "need more data" cleanly:

```
enum ParseState { REQUEST_LINE, HEADERS, BODY_CONTENT_LENGTH, BODY_CHUNKED, COMPLETE, ERROR }
```

- **REQUEST_LINE:** scan for `\r\n`; on found, split into method/path/version; reject unsupported methods/versions. Transition to `HEADERS`.
- **HEADERS:** scan line-by-line for `\r\n`, each line `Name: Value`; blank line (`\r\n` alone) ends headers. Track `headers_bytes_seen` against the max-header-size limit (§5) as bytes are consumed, not after the fact. Determine body-framing from headers:
  - `Content-Length: N` present → `BODY_CONTENT_LENGTH`, need `N` more bytes.
  - `Transfer-Encoding: chunked` present → `BODY_CHUNKED`.
  - Neither, and method has no body (`GET`) → `COMPLETE` immediately.
- **BODY_CONTENT_LENGTH:** accumulate exactly `N` bytes; on complete, → `COMPLETE`.
- **BODY_CHUNKED:** repeatedly parse `<hex-size>\r\n<chunk-data>\r\n`; a `0\r\n\r\n` terminator (optionally followed by trailers, which for v1 can be parsed and discarded) ends the body → `COMPLETE`.
- **Keep-alive:** on `COMPLETE`, if `Connection: keep-alive` (HTTP/1.1 default) and no `Connection: close`, reset parser state for the next request on the same connection instead of closing it; leftover bytes already read past the current request's boundary (pipelined data) remain in `read_buf` and feed the next parse cycle directly.

Response formatting is the mirror: status line + headers (always including `Content-Length` or `Transfer-Encoding: chunked`, and `Content-Type`) + body, serialized into `write_queue`.

---

## 4. Concurrency Models

### 4.1 Thread Pool (PRD L3)
```cpp
struct ThreadPool {
  std::vector<std::thread> workers;
  ConcurrentQueue<Connection*> queue;     // bounded, configurable capacity
  std::mutex queue_mutex;                 // guard queue
  std::condition_variable condvar;        // wake workers
};
```
- `accept()` loop runs on a dedicated thread (or the main thread); each accepted connection is pushed onto `queue`.
- Worker threads block on `condvar` when idle; on wake, pop a connection and run its full request/response cycle using **blocking** I/O (simplifies worker logic — one connection fully owned by one thread for its lifetime while active).
- Bounded queue: if full when a new connection arrives, either reject immediately (503-equivalent close) or block the accept loop briefly — pick reject-immediately for v1, since it's simpler to reason about under load and gives a clean signal in the benchmark (Layer 6) about where the model falls over.
- Keep-alive connections stay assigned to the same worker across requests on that connection (the worker loops: parse → handle → respond → check keep-alive → repeat) rather than returning to the shared queue between requests, to avoid unnecessary queue churn.

### 4.2 epoll Event Loop (PRD L4)
```
epoll_fd = epoll_create1(0)
loop {
  events = epoll_wait(epoll_fd, ...)
  for each event:
    if event.fd == listener_fd: accept new connection(s), set non-blocking, epoll_ctl ADD (EPOLLIN)
    else: 
      if EPOLLIN: read available bytes, feed parser (§3), if request complete → handle → enqueue response, epoll_ctl MOD to watch EPOLLOUT if response not fully flushed
      if EPOLLOUT: flush write_queue; if fully flushed, epoll_ctl MOD back to EPOLLIN-only (or close if not keep-alive)
      if EPOLLHUP/EPOLLERR: close connection, cleanup
}
```
- **Level-triggered** epoll for v1 (simpler correctness properties — re-fires as long as data is available/writable, forgiving of a handler that doesn't drain everything in one pass) rather than edge-triggered; documented as a deliberate choice in the PRD's risk table, revisit only as a stretch goal.
- All sockets non-blocking; a single thread drives the loop (true single-threaded event loop, matching the PRD's positioning) — no request handler may block (e.g., static file reads should use non-blocking or `O_NONBLOCK`-aware I/O, or be kept fast enough not to matter at this project's scope; this is a documented simplification, not a production-grade async-I/O implementation).

### 4.3 Mode Selection
Runtime config flag (`--concurrency=thread-pool|epoll`) selects which of §4.1/§4.2 drives the same `Connection`/parser/handler code from §2–3 — this shared core is what makes the Layer 6 benchmark an apples-to-apples comparison rather than two different codebases.

---

## 5. Timeout Guard / Slowloris Defense (PRD L8)

Enforced identically regardless of concurrency mode:
- **Read timeout:** if `now - last_activity > READ_TIMEOUT` while still waiting for a complete request line/headers, close the connection. (Thread pool: a per-connection timer or periodic sweep by a watchdog thread; epoll: `epoll_wait`'s timeout parameter plus a periodic scan of connections for staleness, or a min-heap of deadlines checked each loop iteration.)
- **Write timeout:** analogous, if a client stops reading (write buffer not draining) beyond `WRITE_TIMEOUT`.
- **Max header size:** enforced incrementally during parsing (§3's `HEADERS` state), not after accumulating an unbounded buffer — reject with `431 Request Header Fields Too Large`-equivalent and close as soon as the threshold is crossed, not after fully buffering an oversized header set (buffering-then-rejecting defeats the purpose).
- **Max header count:** simple counter alongside the byte-size check.
- Defaults (tunable): read timeout 10s for headers, 30s for a `Content-Length` body given expected throughput; write timeout 30s; max header size 8KB; max header count 100.

---

## 6. TLS Integration (PRD L7)

- TLS listener is a **separate accepted-socket wrapping step**: after `accept()`, if the listening port is the TLS port, wrap the raw fd in a TLS session (OpenSSL `SSL_new`/`SSL_accept`, or Rust `rustls::ServerConnection`) before any bytes are handed to the parser (§3).
- `Connection.tls_session` (§2) is optional; all read/write calls in §2/§4 go through a thin abstraction (`conn_read()`/`conn_write()`) that dispatches to either raw syscall or TLS session read/write — this is what keeps the parser and concurrency models (§3, §4) unaware of whether TLS is involved.
- Certificate/key loaded once at startup from configured file paths; self-signed cert acceptable for development/demo (PRD explicitly scopes out cipher-suite hardening, OCSP, etc.).

---

## 7. Reverse Proxy Mode (PRD L9)

```
struct Backend { address, is_healthy: bool, active_connections: int }
struct ProxyPool { backends: Backend[], strategy: "round-robin" | "least-connections", next_rr_index: int }
```
- On a request arriving in proxy mode: select a backend per `strategy` (round-robin: `next_rr_index++ % len(backends)`, skipping unhealthy ones; least-connections: pick min `active_connections` among healthy backends).
- Open (or reuse, if pooling connections to backends — a reasonable stretch optimization) a connection to the selected backend, forward the request line/headers/body largely as-is (adjusting `Host` header as needed), stream the backend's response back to the original client through the same `write_queue` mechanism (§2).
- Health tracking: mark a backend unhealthy on connect failure or repeated request failures; periodically retry unhealthy backends (simple active health check on an interval, or passive — retry on next scheduled turn) to bring them back into rotation.

---

## 8. Compression (PRD L10)

- After the handler produces a response body (static file or proxied), before final serialization to `write_queue`: check request `Accept-Encoding` for `gzip`; if present, and response `Content-Type` is in a configured compressible list (e.g., `text/*`, `application/json`), and body size exceeds a minimum threshold (compressing tiny bodies isn't worth the CPU), compress via `zlib`/`miniz` and set `Content-Encoding: gzip` + updated `Content-Length`.
- Skip compression if the response is already `Content-Encoding`'d (e.g., proxied from a backend that already compressed it) — check before re-compressing.

---

## 9. Observability & Graceful Shutdown (PRD L11, L12)

**Access logs:** one structured (JSON) line per completed request: `{ timestamp, method, path, status, latency_ms, client_ip, bytes_sent }`, written by the same worker/loop iteration that completes the response (no separate logging thread needed at this scale, but a bounded async log queue is a reasonable stretch if logging I/O visibly affects benchmark numbers).

**Metrics endpoint** (`/metrics` on a separate internal port, or same port under a reserved path): `active_connections`, `requests_total`, `thread_pool_queue_depth` (thread-pool mode) or `epoll_watched_fds` (epoll mode), latency histogram.

**Graceful shutdown:**
1. On `SIGTERM`: set a global `shutting_down` flag; stop calling `accept()` (or close the listening socket) immediately.
2. In-flight connections continue processing to completion under both concurrency models (thread pool: workers finish their current connection's request/response cycle and don't pull new work from the queue; epoll: loop continues servicing already-open fds but the listener fd is no longer registered).
3. A shutdown deadline timer runs in parallel; any connection still open when the deadline fires is force-closed with a logged warning.
4. Process exits once all connections are closed or the deadline is reached, whichever comes first.

---

## 10. Non-Functional Requirements

| Category | Requirement |
|---|---|
| **Concurrency scale** | epoll mode must sustain a meaningfully higher connection count than thread-per-connection baseline at equal or better p99 latency — must be graphed with real `wrk`/`k6` numbers (PRD headline metric) |
| **Slowloris resilience** | A simulated slow-header attack (many connections, one byte of header every few seconds) must not exhaust thread pool or epoll fd capacity — timeouts must cap the damage within a bounded window |
| **TLS overhead** | Measured and reported explicitly (handshake latency, throughput delta vs. plaintext) — not asserted |
| **Reverse proxy correctness** | A killed backend is detected and routed around with no client-visible failure beyond the single in-flight retried request |
| **Graceful shutdown** | In-flight requests complete successfully during a `SIGTERM`-triggered shutdown; new connections are refused immediately; process exits within the configured deadline |

---

## 11. Infrastructure & Deployment

### 11.1 Configuration
Flat config file (JSON/TOML) or CLI flags covering: listen ports (plaintext/TLS), concurrency mode, thread pool size, timeouts (§5), TLS cert/key paths, serving mode (static+routes vs. reverse proxy) and its mode-specific settings (document root / backend list + strategy), compression settings, graceful shutdown deadline.

### 11.2 Benchmark environment
- `wrk` or `k6` driving load from a separate machine/process than the server under test, to avoid client-side resource contention skewing results.
- Identical hardware, identical load-generator config, across all three concurrency-model comparisons (thread-per-connection baseline, thread pool, epoll) — methodology consistency is what makes the headline C10K graph credible.

---

## 12. Testing Strategy

| Layer | Test approach |
|---|---|
| Parser (§3) | Unit tests per state transition; fragmented-input tests (feed the parser 1 byte at a time, assert identical result to feeding it all at once); chunked-encoding edge cases (zero-length chunks, trailers) |
| Partial I/O (§2) | Slow-send test client that writes a request in deliberately tiny, delayed increments; assert correct parsing and no data loss |
| Thread pool (§4.1) | Load test at increasing connection counts; assert bounded-queue reject behavior triggers at the configured threshold, not silently drops or hangs |
| epoll loop (§4.2) | Load test at connection counts that would exhaust thread-per-connection; assert successful handling with bounded memory/fd usage |
| Slowloris defense (§5) | Dedicated chaos test: many connections trickling headers one byte at a time; assert timeouts reclaim connections and total resource usage stays bounded throughout |
| TLS (§6) | Handshake test via real client (`curl`/browser) against a self-signed cert; assert identical request/response behavior to the plaintext listener underneath |
| Reverse proxy (§7) | Kill a backend mid-test; assert requests route to remaining healthy backends with no client-visible errors beyond the single affected request |
| Compression (§8) | Assert compressed response, when decompressed, is byte-identical to the uncompressed version; assert `Accept-Encoding` absence correctly skips compression |
| Graceful shutdown (§9) | Send `SIGTERM` mid-load-test; assert in-flight requests complete, new connections are refused, and process exits within the deadline |

---

## 13. Open Technical Decisions (resolve before Milestone 1)

- Level-triggered vs. edge-triggered epoll (§4.2 recommends level-triggered for v1) — revisit only if you want the extra performance/complexity credibility as a stretch goal, per the PRD's risk table.
- Round-robin vs. least-connections as the reverse proxy's default strategy (§7) — round-robin is simpler to reason about and sufficient for the demo; least-connections is the documented stretch goal.
- Log I/O model (§9) — synchronous per-request logging is simplest and fine unless benchmarks show it affecting p99 latency, in which case an async bounded log queue is the fallback.

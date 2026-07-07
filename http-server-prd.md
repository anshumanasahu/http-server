# Project Requirement Document
## HTTP/1.1 Web Server from Scratch (Socket-Level)

**Author:** Anshuman
**Status:** Draft v1
**Track:** Resume project — backend / systems depth
**Last updated:** July 2026

---

## 1. Positioning

A multithreaded HTTP/1.1 server built directly on raw TCP sockets — no framework — implementing connection handling, HTTP parsing, TLS, a thread pool and an epoll-based event loop, reverse-proxying, and production-adjacent concerns (timeouts, graceful shutdown, observability), benchmarked head-to-head against a naive thread-per-connection model.

The core "C10K problem" comparison (thread-per-connection vs. thread pool vs. epoll event loop) is still the headline result. The additions here are what keep the project from reading as a toy that stops the moment `curl` returns `200 OK` — they're the difference between "parses HTTP" and "the things you'd actually have to build before putting this anywhere near real traffic."

---

## 2. Goals

- Implement HTTP/1.1 parsing and response formatting correctly by hand, including the genuinely fiddly parts (`Content-Length`, chunked transfer encoding, keep-alive).
- Build and benchmark three distinct concurrency models on the same request-handling core, proving the C10K story with real numbers.
- Serve over both plaintext and TLS.
- Defend against slow/malicious clients (Slowloris-style attacks) with real timeouts, not just theoretical awareness.
- Support a reverse-proxy mode, turning the project from "a web server" into "a small piece of request-routing infrastructure."
- Add the operational layer (compression, graceful shutdown, structured logs, metrics) that every real server needs and that most from-scratch projects skip.

## 3. Non-Goals (explicitly out of scope for v1)

- HTTP/2 or HTTP/3 (QUIC) — HTTP/1.1 only.
- WebSocket support.
- A full CGI/FastCGI-style dynamic backend execution model — routing to a couple of hardcoded dynamic endpoints is sufficient (Layer 5); this isn't an application server.
- Auto-scaling or multi-process (SO_REUSEPORT-style) worker models — single process, multiple threads/event loop.
- A configuration language/DSL — a flat config file (JSON/TOML) or command-line flags are sufficient.

---

## 4. Architecture Overview

```
                     ┌─────────────────────────────┐
   Client ─────────▶│  Listener (raw socket)        │
                     │  plaintext or TLS (L7)        │
                     └──────────────┬────────────────┘
                                    │
                     ┌──────────────▼────────────────┐
                     │  Slow-client defense (L8)       │
                     │  timeouts, max header size       │
                     └──────────────┬────────────────┘
                                    │
                     ┌──────────────▼────────────────┐
                     │  Concurrency Model              │
                     │  Thread Pool (L3) or             │
                     │  epoll Event Loop (L4)           │
                     └──────────────┬────────────────┘
                                    │
                     ┌──────────────▼────────────────┐
                     │  HTTP/1.1 Parser (L2)            │
                     └──────────────┬────────────────┘
                                    │
              ┌─────────────────────┼─────────────────────┐
              │                                            │
   ┌──────────▼───────────┐                    ┌───────────▼──────────┐
   │  Static Files /       │                    │  Reverse Proxy Mode   │
   │  Routing (L5)         │                    │  (L9)                  │
   │  + Compression (L10)  │                    │  round-robin backends │
   └────────────────────────┘                    └───────────────────────┘
                                    │
                     ┌──────────────▼────────────────┐
                     │  Access Logs + Metrics (L11)     │
                     │  Graceful Shutdown (L12)         │
                     └────────────────────────────────┘
```

---

## 5. Detailed Layer Requirements

### Layer 1 — Socket Fundamentals
- Raw socket setup: `socket()`, `bind()`, `listen()`, `accept()` — no HTTP library, no framework.
- Get a single blocking connection working end-to-end before adding any concurrency.
- Buffering/accumulation strategy for partial reads/writes — a `read()` call is not guaranteed to return a complete HTTP request or write the full response in one call.

**Acceptance criteria:** a single hardcoded request/response round-trip works correctly against a real browser or `curl`, including under artificially fragmented TCP writes (test via a slow-send client).

### Layer 2 — HTTP/1.1 Protocol Parsing
- Hand-written parser: request line (method, path, version), headers, body.
- `GET` and `POST` at minimum.
- `Content-Length`-delimited bodies and chunked transfer encoding.
- `Connection: keep-alive` — multiple requests over one TCP connection without reconnecting.
- Correct response formatting: status line, headers (`Content-Type`, `Content-Length`), body.

**Acceptance criteria:** parser correctly handles a chunked-encoded request body, a keep-alive session with 3+ sequential requests on one connection, and malformed input (missing `Content-Length`, garbage request line) without crashing.

### Layer 3 — Concurrency Model #1: Thread Pool
- Fixed-size worker pool pulling accepted connections off a shared queue.
- Mutex/condition-variable (or lock-free) synchronization between the accept loop and workers.
- Configurable pool size; explicit, documented behavior when all threads are busy (bounded queue with reject, vs. unbounded queue — pick one and justify it).

**Acceptance criteria:** pool size is configurable at startup and the reject/queue behavior is observable under load (visible in the benchmark, not just in code).

### Layer 4 — Concurrency Model #2: Event Loop (epoll)
- Single-threaded event loop using `epoll` (Linux) multiplexing many connections without one thread per connection.
- Runtime-selectable mode (config flag) so thread-per-connection, thread pool, and epoll can be directly compared on the same codebase.

**Acceptance criteria:** epoll mode handles a connection count that meaningfully exceeds what the thread-per-connection model can sustain, at lower memory/context-switch overhead — this is the headline benchmark result.

### Layer 5 — Static File Serving & Routing
- Serve files from a configured directory; explicit path-traversal protection (reject `../` escapes) — call this out as a concrete, demonstrable security detail.
- Basic routing table for a couple of dynamic endpoints (e.g., an echo endpoint, a JSON status endpoint) so it's clear the server isn't just a file server.

**Acceptance criteria:** a crafted request attempting path traversal (`GET /../../etc/passwd`) is rejected, not served.

### Layer 6 — Benchmark & Demo
- Load test with `wrk` or `k6` at increasing concurrency (100 → 1,000 → 10,000 connections).
- Head-to-head comparison: thread-per-connection (naive baseline) vs. thread pool vs. epoll event loop. Graph throughput and p99 latency for each.
- Document where the naive model falls over (thread exhaustion, context-switch overhead) versus where the event loop keeps scaling — this is the C10K story and should be the centerpiece of the write-up.

### Layer 7 — TLS/HTTPS Support *(new)*
- Wrap the raw socket layer with TLS via OpenSSL (C) or rustls (Rust) — server presents a certificate, negotiates a cipher suite, and all subsequent read/write goes through the TLS layer instead of the raw socket directly.
- Support both plaintext and TLS listeners simultaneously (different ports), so the concurrency and parsing benchmarks can optionally be re-run over TLS to show the overhead.

**Acceptance criteria:** a browser or `curl --cacert` connects successfully over HTTPS using a locally-generated self-signed cert; the same request/response logic (Layer 2) runs unmodified underneath.

### Layer 8 — Slowloris Protection / Connection Timeouts *(new)*
- Read timeout: if a client doesn't send a complete request line/headers within a bounded window, the connection is closed.
- Write timeout: if a client stops reading a response mid-send, the connection is closed rather than held open indefinitely.
- Max header size and max header count, to bound memory use per connection regardless of timeout behavior.
- This is the direct, concrete answer to "how do you prevent a slow/idle client from starving other connections" — build it, don't just be ready to talk about it abstractly.

**Acceptance criteria:** a simulated Slowloris client (opens many connections, sends headers one byte at a time with long delays) is unable to exhaust the thread pool or event loop — connections are dropped by timeout before they can accumulate unboundedly. Include this as its own chaos-style demo alongside the main benchmark.

### Layer 9 — Reverse Proxy / Load-Balancing Mode *(new)*
- A server mode where incoming requests are forwarded to a pool of backend servers rather than served locally (static files/routing).
- Load-balancing strategy: round-robin at minimum; least-connections as a stretch goal.
- Correctly proxy request/response bodies and relevant headers; handle a backend being unreachable (mark it down, skip it in rotation, retry against another backend).

**Acceptance criteria:** with 2–3 dummy backend servers running, the proxy distributes requests according to the configured strategy, and correctly routes around a backend that's killed mid-test.

### Layer 10 — Gzip/Deflate Compression *(new)*
- Inspect `Accept-Encoding` on the request; if the client supports gzip (or deflate) and the response is compressible (based on `Content-Type` and/or a minimum size threshold), compress the body and set `Content-Encoding` accordingly.
- Applies to both static file serving (Layer 5) and proxied responses (Layer 9), if feasible.

**Acceptance criteria:** a compressible response (e.g., a large text/JSON file) is measurably smaller in bytes-on-wire when the client advertises gzip support, and byte-identical to the uncompressed version once decompressed.

### Layer 11 — Observability: Access Logs & Metrics *(new)*
- Structured (JSON or common-log-format) access logs: method, path, status code, response time, client IP, per request.
- `/metrics` endpoint (or equivalent) exposing: active connection count, requests/sec, thread pool queue depth (or epoll fd count), average/p99 latency.

**Acceptance criteria:** metrics endpoint values visibly track load during a benchmark run (e.g., queue depth rises under thread-pool saturation) — used live during the Layer 6 demo, not just present in code.

### Layer 12 — Graceful Shutdown *(new)*
- On `SIGTERM` (or equivalent), the server stops `accept()`-ing new connections immediately, allows in-flight requests to complete up to a configured deadline, then exits.
- Requests that don't complete within the deadline are cut off with a clear log entry, not a silent hang.

**Acceptance criteria:** sending `SIGTERM` mid-load-test results in in-flight requests completing successfully while new connection attempts are refused, and the process exits cleanly within the configured deadline.

---

## 6. Tech Stack

**C++ (C++17 or later).** C++ is an excellent choice for this project. It provides the "from scratch" systems-level authenticity of C (direct access to POSIX sockets, `epoll`, and manual memory management where needed) while offering modern standard library abstractions (`std::vector`, `std::string_view`, `std::thread`, `std::unique_ptr`) to avoid tedious buffer management bugs. 

**Critical Constraint:** To preserve the interview signal, you must rely exclusively on raw POSIX/Linux syscalls (`sys/socket.h`, `sys/epoll.h`) for networking. The use of high-level abstractions like `Boost.Asio` or third-party HTTP libraries is strictly prohibited. Avoid a managed-runtime language (Go, Java, Node) — the entire point of this project is demonstrating what such runtimes normally hide from you.

Suggested libraries: OpenSSL for TLS (Layer 7), utilizing RAII wrappers (e.g., `std::unique_ptr<SSL, decltype(&SSL_free)>`) for memory safety; `zlib`/`miniz` for gzip (Layer 10); no HTTP, no async runtime, no networking framework — everything else hand-rolled.

---

## 7. Build Sequence / Milestones

| # | Milestone | Depends on |
|---|-----------|------------|
| 1 | Single blocking connection, hardcoded response | — |
| 2 | HTTP/1.1 parser: request line, headers, `Content-Length` body | 1 |
| 3 | Chunked transfer encoding + keep-alive | 2 |
| 4 | Static file serving + routing + path-traversal protection | 2 |
| 5 | Thread pool concurrency model | 2 |
| 6 | epoll event loop concurrency model (runtime-selectable vs. thread pool) | 2 |
| 7 | Baseline benchmark: thread-per-connection vs. thread pool vs. epoll | 5, 6 |
| 8 | Connection timeouts + max header size (Slowloris defense) | 2, 5, 6 |
| 9 | TLS/HTTPS listener | 1 |
| 10 | Gzip/deflate compression | 4 |
| 11 | Reverse proxy / load-balancing mode | 2, 5 or 6 |
| 12 | Access logs + metrics endpoint | 5 or 6 |
| 13 | Graceful shutdown | 5 or 6 |
| 14 | Slowloris chaos demo + final benchmark suite + write-up | 8, 12 |

Milestones 1–7 are the original spec and the load-bearing core — the C10K comparison is the headline result and needs to be airtight before anything else is layered on. 8–13 are the additions, roughly ordered by interview leverage (Slowloris defense directly answers a question you already flagged; TLS and reverse-proxy add the most "this is real infrastructure" credibility; compression, logging, and graceful shutdown round it out).

---

## 8. Success Metrics

- epoll event loop sustains meaningfully higher concurrent connections than thread-per-connection at equivalent or better p99 latency (this is the headline number — graph it).
- Thread-per-connection model visibly degrades (rising latency, thread exhaustion, or crashes) at a connection count the epoll model handles cleanly.
- Slowloris simulation cannot exhaust server resources — timeouts cap the damage.
- TLS handshake overhead is measured and reported, not just "it works."
- Reverse proxy correctly routes around a killed backend with no client-visible errors beyond the single retried request.

---

## 9. Interview Talking Points

- Why is `Content-Length` necessary — what happens without it, and how does chunked encoding solve the same problem differently?
- Thread pool vs. event loop — what's the actual mechanism that lets epoll handle many more connections with one thread?
- What's a partial read, and why can't you assume one `read()` call returns a complete HTTP request?
- How do you prevent a slow/idle client from starving other connections?
- Where does TLS sit relative to your HTTP parsing layer, and why does that layering matter?
- What's the difference between round-robin and least-connections load balancing, and when would you choose one over the other?
- Why does graceful shutdown need a deadline, not just "wait for everything to finish"?

---

## 10. Risks & Mitigations

| Risk | Mitigation |
|------|------------|
| epoll edge-triggered vs. level-triggered semantics are a common source of subtle bugs (missed events, busy-looping) | Start with level-triggered (simpler, more forgiving) and only move to edge-triggered if you want the extra credibility — document the choice either way |
| TLS integration can eat disproportionate time versus its interview payoff | Timebox it — a working self-signed-cert HTTPS listener is the goal, not a fully hardened TLS configuration (cipher suite tuning, OCSP stapling, etc. are explicitly out of scope) |
| Benchmark results are only credible if methodology is sound | Run all three concurrency models on identical hardware, identical `wrk`/`k6` config, and report methodology alongside numbers — an impressive-looking graph with sloppy methodology is a liability in an interview, not an asset |
| Scope: 14 milestones is a lot for a from-scratch systems project | Protect 1–8 (core + Slowloris defense, since it's a named interview question) over 9–13; cut order if needed: graceful shutdown → metrics/logging → compression → reverse proxy → TLS (TLS last to cut, since it's the highest standalone credibility signal) |

---

## 11. Future Extensions (explicitly out of scope, good "what's next")

- HTTP/2 (multiplexed streams, HPACK header compression) — a natural "if I had more time" answer given how different its concurrency model is from HTTP/1.1.
- WebSocket upgrade support.
- SO_REUSEPORT-based multi-process worker model for true multi-core scaling beyond a single event loop.
- Least-connections and latency-aware load balancing strategies for the reverse proxy.
- Rate limiting at the server layer — natural crossover with a standalone rate limiter project, if you build both.

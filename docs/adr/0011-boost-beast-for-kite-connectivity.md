# ADR-0011: Boost.Beast (Asio + OpenSSL) for Kite WebSocket and REST clients

- **Status:** Accepted
- **Date:** 2026-10-22
- **Deciders:** Sivakannan, Claude

## Context

`mda-feedd` needs:
- up to **3 TLS WebSocket connections** to `wss://ws.kite.trade` (binary ticks, JSON text
  messages, heartbeats, reconnects);
- **HTTPS REST** calls to `https://api.kite.trade` at startup and occasionally (validate
  the session token, download the ~10 MB instrument CSV, later historical backfill).

Load is small (a few thousand messages/s, 3 sockets); the internet path to Zerodha
dominates latency. The choice matters for correctness, portability (macOS + Linux) and
learning value more than raw speed.

## Options considered

### Option A — Boost.Beast on Boost.Asio, OpenSSL for TLS
- Pros: complete WebSocket **client** and HTTP/1.1 in one library; TLS via Asio's SSL
  stream; epoll (Linux) / kqueue (macOS) underneath; the de-facto C++ standard; Asio
  is what interviews and trading codebases expect you to know.
- Cons: verbose, steep learning curve (completion handlers, strands, lifetimes); large
  dependency (header-only parts of Boost, plus OpenSSL).

### Option B — uWebSockets
- Pros: very fast; simple API.
- Cons: server-oriented; client support limited — not suitable for this side.

### Option C — Drogon
- Pros: full framework (HTTP client/server, WebSocket client).
- Cons: hides the event loop and I/O model we want to learn; heavy framework for a client.

### Option D — libcurl (REST) + a small WebSocket library (IXWebSocket, websocketpp)
- Pros: easy REST.
- Cons: two networking stacks and two event models; websocketpp is unmaintained.

### Option E — Hand-written client on epoll/kqueue + OpenSSL
- Pros: maximum learning; full control.
- Cons: TLS on non-blocking sockets, WebSocket framing, HTTP parsing — weeks of work
  before the first tick. Planned later as a **learning exercise benchmarked against
  Beast**, not as the first implementation.

## Decision

**Option A.** Boost.Beast + Boost.Asio + OpenSSL (via vcpkg: `boost-beast`, `boost-asio`,
`openssl`), plus **Boost.JSON** for the small amount of JSON (Kite text messages, REST
responses, session file) — same library family, no extra ecosystem.

- One `asio::io_context` in `mda-feedd` runs all connections (see
  [Kite connectivity](../design/kite-connectivity.md) for the threading model).
- C++20 coroutines (`asio::awaitable`) for connection logic: linear, readable code for
  connect → subscribe → read loop → reconnect. (ADR-0002 keeps coroutines off the hot
  path; here the hot work — decode and publish — is plain synchronous code called from
  the read loop.)

## Rationale

Beast is the only option that gives a production-grade WebSocket *client*, HTTP and TLS
on one event loop, portable across our two OSes, and it teaches the Asio model that
dominates C++ networking. The hand-written epoll/kqueue client remains on the roadmap
as a measured comparison.

## Consequences

- Positive: one networking stack for WS and REST; portable; well documented.
- Negative: Boost + OpenSSL increase first-build time (vcpkg binary cache mitigates);
  Asio lifetime rules require care (covered by ASan/TSan runs).
- **TLS trust store:** vcpkg's OpenSSL does not read the macOS keychain; the CA bundle
  path is configurable (default: Homebrew's `openssl@3/cert.pem` on macOS, system store
  on Linux). Certificate verification is never disabled.
- Revisit: after Phase 1, benchmark against a hand-written client.

## Learning notes

- Asio's proactor model, completion tokens, coroutines vs callbacks, strands.
- TLS handshake, SNI, certificate verification; WebSocket upgrade, framing, ping/pong.
- Interview: "Walk me through what happens between `connect()` and the first market data
  message on a TLS WebSocket."

# ADR-0013: mda-gateway HTTP server on Boost.Beast, Kite-style API conventions

- **Status:** Accepted
- **Date:** 2026-10-22
- **Deciders:** Sivakannan, Claude

## Context

`mda-gateway` serves the browser (React dashboard) and command-line clients: REST now
(users, then watchlists), WebSocket push later (live data). We already depend on
Boost.Beast/Asio for the Kite clients (ADR-0011). The API style was agreed early:
mirror Kite Connect's conventions so there is one style to learn.

## Options considered

- **Boost.Beast server** — same library and event model as the Kite clients; HTTP and
  WebSocket server; low-level (we write routing and JSON helpers ourselves).
- **Drogon** — full framework: routing, ORM, sessions; hides the I/O model; a second
  networking stack beside Asio.
- **uWebSockets** — excellent WebSocket fan-out; HTTP/REST support is basic; another
  event loop.
- **Crow / Pistache** — simple REST; less maintained or less capable for WebSocket push.

## Decision

**Boost.Beast** for `mda-gateway`, with C++20 coroutines on one `io_context` thread, plus a
small worker pool for CPU-heavy work (password hashing) so the event loop never blocks.

API conventions (Kite-style), specified in [`docs/api/rest.md`](../api/rest.md):

- Base path `/api/v1`; JSON request and response bodies (`application/json`).
- Success: `{"status": "success", "data": …}`; error:
  `{"status": "error", "error_type": "…", "message": "…"}` with a matching HTTP status.
- Error types: `InputException` (400), `AuthException` (401), `PermissionException`
  (403), `NotFoundException` (404), `ConflictException` (409), `RateLimitException` (429),
  `GeneralException` (500).
- Listens on **127.0.0.1 by default**; exposing it beyond the machine requires TLS
  (a later decision).

## Rationale

One networking stack for the whole backend, an I/O model we understand and can measure,
and a familiar API shape for the frontend. uWebSockets stays a candidate for high-volume
WebSocket fan-out if measurements ever demand it.

## Consequences

- Positive: consistent codebase; no extra framework; full control over performance.
- Negative: routing, request parsing helpers and JSON envelopes are ours to write and test.
- The OpenAPI document (`docs/api/openapi.yaml`) follows once the REST surface stabilises.

## Learning notes

- Writing an HTTP server on Asio; keep-alive, request limits, timeouts.
- Keeping CPU-bound work off the event loop (thread pool hand-off with `co_await`).
- Interview: "Your event loop must stay responsive while doing a 50 ms CPU task. How?"

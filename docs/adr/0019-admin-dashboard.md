# ADR-0019: Admin dashboard: roles, auth event log and per-user usage

- **Status:** Accepted
- **Date:** 2026-10-22
- **Deciders:** Sivakannan, Claude

## Context

The operator (and, later, anyone running md_analytics for a few friends) wants to see:
- who the users are;
- who is online now and since when;
- when each user last signed in and out, and from which device / IP;
- how much data each user consumed today.

Part of this exists today:
- `users` lists the users.
- `sessions` has `created_at`, `last_seen_at` and the user agent.

The rest does not:
- **sign-out deletes the session row**, so logout times are lost;
- nothing records failed sign-ins;
- the gateway doesn't measure what it sends.

## Decision

### Who is an admin
- `users.role` is `user` or `admin`.
- Admins are made **only from the command line** (`mda-useradm grant-admin <username>`),
  run by whoever has access to the machine and the DB. There is no web route to change
  roles, so a stolen browser session cannot escalate itself.
- Every `/api/v1/admin/*` route checks the role **on the server** (403 otherwise). The UI
  hiding the page is a convenience, not the protection.

### Auth event log (`auth_events`)
- An append-only row per event: `sign_in`, `sign_in_failed`, `sign_out`,
  `password_changed`, `session_expired`, `session_revoked` (by an admin) and
  `account_deleted`. Each row has the time, IP and user agent.
- It has no foreign key to `users`, and it copies the username, so the history of a
  deleted account remains readable.
- Rows older than **90 days** are purged at gateway start and daily (they hold IPs).
- "Last signed in / out" are derived from this log. A session's start is still the
  session row.

### Client IP
- The HTTP server stamps each request with the peer address in an internal header,
  removing any copy sent by the client.
- `X-Forwarded-For` (first hop) is trusted **only from a loopback peer**, i.e. our own
  dev proxy or a reverse proxy on the same machine. Otherwise anyone could fake their IP.

### Usage metering (`usage_daily`)
- The gateway counts per user, per **IST trading day**: WebSocket bytes and messages, and
  REST response bytes and requests.
- Counting happens in memory under one mutex: two additions per write, nothing on the bus
  hot path.
- Totals are **added** to `usage_daily` every 60 s and at shutdown, so a crash loses at
  most a minute.
- REST bytes are attributed through a small token→user cache. The auth service fills it
  each time it authenticates a token, so the HTTP server needs no database lookups.
- Bytes are payload bytes as written by the server (HTTP header + body; WebSocket
  message payload). TLS and TCP overhead are not counted. This is good enough to compare
  users and to spot a runaway client.

### Online now
- The stream service registers each open WebSocket (user, since, subscribed instruments).
  The admin API reads that registry. It lives in memory: "online" means "has a live
  stream to this gateway".

### API and UI
- `GET /api/v1/admin/users`: one row per user with role, created, last sign-in / sign-out,
  last seen, active sessions, online connections and today's usage.
- `GET /api/v1/admin/users/{id}`: sessions (device, IP, since, last seen), the last 50
  auth events, 30 days of usage and live connections.
- `DELETE /api/v1/admin/sessions/{session_id}`: force sign-out. The session id is the
  hex SHA-256 of the token, which we already store. The token itself is never exposed.
  An open stream closes at its next re-check (≤ 60 s).
- Contact details are **masked** in admin responses (`si***@gmail.com`, `+91•••••43210`).
- The web app shows an **Admin** page, only to admins: a user table refreshed every 5 s
  and a per-user detail view.

## Consequences

- There is no history before this ships. Sign-in times and usage start when it is deployed.
- `signin()` now takes the client IP. Sessions store it (migration 5).
- Usage numbers are approximate in the ways listed above, and that is said in the UI.
- Multi-gateway deployments would need the registry and counters shared (not planned).

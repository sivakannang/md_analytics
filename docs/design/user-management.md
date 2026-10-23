# User management (app login)

> Status: **Accepted** · Decisions: [ADR-0012](../adr/0012-sqlite-app-database.md)
> (SQLite), [ADR-0013](../adr/0013-gateway-http-server-and-api-conventions.md) (gateway),
> [ADR-0014](../adr/0014-authentication.md) (auth) · API: [`docs/api/rest.md`](../api/rest.md)

Sign up, sign in, sign out, current user, change password. Independent of the live feed.
Watchlists come next and reuse everything here.

## 1. Modules

| Module | Contents |
|---|---|
| `storage` | SQLite RAII wrapper (`Database`, `Statement`), transactions, migrations |
| `auth` | Argon2id password hashing (OpenSSL), session tokens, `UserStore` (SQL), `AuthService` (rules) |
| `gateway` | Beast HTTP server, router, JSON envelope helpers, cookie handling, handlers |
| `apps/mda-gateway` | Process: flags, open DB + migrate, start server |

Dependency direction: `gateway → auth → storage`. Nothing depends on `kite`.

## 2. Schema (`data/app.db`, migration 1)

```sql
CREATE TABLE users (
    id             INTEGER PRIMARY KEY,
    username       TEXT NOT NULL UNIQUE COLLATE NOCASE,
    password_hash  TEXT NOT NULL,              -- PHC string: $argon2id$v=19$m=…$salt$hash
    created_at     INTEGER NOT NULL,           -- Unix seconds, UTC
    password_changed_at INTEGER NOT NULL
);

CREATE TABLE sessions (
    token_hash     BLOB PRIMARY KEY,           -- SHA-256 of the 32-byte random token
    user_id        INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    created_at     INTEGER NOT NULL,
    last_seen_at   INTEGER NOT NULL,           -- sliding 30-day expiry
    user_agent     TEXT
);
CREATE INDEX sessions_by_user ON sessions(user_id);
```

Migration 2 (contact details for notifications):

```sql
ALTER TABLE users ADD COLUMN email TEXT COLLATE NOCASE;   -- lower-case, unique
ALTER TABLE users ADD COLUMN phone TEXT;                  -- E.164 (+91…), not unique
ALTER TABLE users ADD COLUMN email_verified_at INTEGER;   -- NULL = not verified
ALTER TABLE users ADD COLUMN phone_verified_at INTEGER;
CREATE UNIQUE INDEX users_by_email ON users(email) WHERE email IS NOT NULL;
```

`PRAGMA user_version` = applied migration number.

**Personal data:** email and phone are personal information. They are never written to logs,
only returned to the signed-in user themself, and will be used only for notifications the
user opts into.

## 3. Request flow

```
HTTP request ─► router (method + path) ─► [🔒 session lookup: cookie → SHA-256 → sessions row,
                                            expiry check, touch last_seen_at] ─► handler
handler ─► validate JSON input ─► AuthService ─► UserStore (SQLite)
                                       └─► password hash/verify on worker pool (co_await)
        ─► JSON envelope response (+ Set-Cookie on sign-in / sign-out)
```

Limits: request body ≤ 64 KB; header ≤ 8 KB; 15 s read timeout; keep-alive supported.

## 4. Behaviour details

| Case | Behaviour |
|---|---|
| Signup, username taken (any case) | 409 `ConflictException` |
| Sign-in, unknown user or wrong password | 401, identical message; dummy Argon2 verify for unknown users |
| 5+ failures for a username | Increasing delay per attempt (1, 2, 4 … 60 s); 429 while blocked |
| Session older than 30 days of inactivity | Deleted on lookup; 401 |
| Change password | Verify current → hash new → update → delete all *other* sessions — one transaction |
| Sign-out | Delete session; `Set-Cookie: mda_session=; Max-Age=0` |
| Startup | Delete expired sessions |

## 5. Implementation units

| Unit | Scope | Tests |
|---|---|---|
| U1 ✅ | `storage`: SQLite wrapper, transactions, migrations | In-memory and temp-file DBs |
| U2 ✅ | `auth`: Argon2id + PHC, tokens, `UserStore`, `AuthService` (no HTTP) | Rules, timing-safe paths, revocation |
| U3 ✅ | `gateway` + `mda-gateway`: HTTP server, routes, cookies, envelopes | In-process HTTP tests + curl walkthrough |

## 6. Open items

- Persist throttling state (only needed if exposed beyond localhost).
- Email / password reset: not needed for two users; an admin CLI (`mda-user reset`) later.
- HTTPS and `Secure` cookies before any non-local exposure.

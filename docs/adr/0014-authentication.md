# ADR-0014: App authentication — Argon2id passwords, server-side sessions, HttpOnly cookies

- **Status:** Accepted
- **Date:** 2026-10-22
- **Deciders:** Sivakannan, Claude

## Context

Users (the author, then a friend) sign up, sign in, change their password and sign out
in the browser dashboard. This is the **app login**, unrelated to the daily Kite broker
login (`mda-login`). Passwords must be stored so that a stolen database does not reveal
them, and sessions must be revocable (e.g. after a password change).

## Options considered

### Password storage
- **Plain / fast hash (SHA-256)** — unacceptable: GPUs test billions of guesses per second.
- **bcrypt / scrypt / PBKDF2** — acceptable; older designs.
- **Argon2id** — winner of the Password Hashing Competition; memory-hard (resists GPU/ASIC
  cracking); OWASP's first recommendation. Available in our OpenSSL 3.6 (no new dependency).

### Sessions
- **JWT (stateless signed tokens)** — no server lookup, but **can't be revoked** before
  expiry without extra machinery; easy to misuse.
- **Server-side sessions** — random token, stored (hashed) in the DB; revocable instantly;
  one cheap lookup per request.

### Transport of the session token
- **`Authorization` header** stored by browser JavaScript — readable by any injected script (XSS).
- **HttpOnly cookie** — JavaScript cannot read it; with `SameSite=Strict` the browser won't
  send it on cross-site requests (CSRF protection).

## Decision

- **Argon2id** with OWASP's minimum parameters: memory 19 MiB, 2 iterations, 1 lane,
  16-byte random salt, 32-byte output; stored in the standard PHC string format
  (`$argon2id$v=19$m=19456,t=2,p=1$<salt>$<hash>`) so parameters can be raised later and
  old hashes still verify. Hashing runs on a worker thread (≈ tens of ms).
- **Server-side sessions**: 32 random bytes (OS CSPRNG), sent to the client once; only
  their **SHA-256 is stored**, so a database leak doesn't leak usable sessions. Expiry
  30 days of inactivity; sign-out deletes the session; **a password change revokes every
  other session** of that user.
- **Cookie** `mda_session`: `HttpOnly`, `SameSite=Strict`, `Path=/`; `Secure` once served
  over HTTPS. The token is never put in a response body.
- **Passwords:** 8–128 characters, no composition rules (NIST/OWASP guidance); usernames
  `[a-z0-9_.-]`, 3–32 characters, unique, case-insensitive.
- **Same error for wrong username or wrong password** ("invalid username or password"),
  and the password is verified against a dummy hash when the user doesn't exist, so
  timing doesn't reveal which usernames exist.
- **Brute-force throttling:** per-username failure counter with increasing delay
  (in-memory, v1).
- **Signup** is open while the gateway listens on 127.0.0.1 only; a flag disables it
  (`--no-signup`) before any wider exposure.

## Rationale

Industry-standard, well-understood choices that are safe by default for a small app and
teach the reasons behind them. Revocable sessions matter more than statelessness at our
scale.

## Consequences

- Positive: database leaks expose neither passwords nor live sessions; sessions revocable.
- Negative: one session lookup per authenticated request (microseconds in SQLite).
- Throttling state resets on restart (acceptable for v1; persist later if exposed online).

## Learning notes

- Why password hashes must be slow and memory-hard; salts; PHC format; parameter upgrades.
- Session tokens vs JWT; XSS vs CSRF and how HttpOnly + SameSite address them.
- Interview: "Design login for a trading dashboard. Where are tokens stored, and why?"

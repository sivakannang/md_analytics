# ADR-0016: Notification service — separate process, Telegram and email first

- **Status:** Accepted
- **Date:** 2026-10-22
- **Deciders:** Sivakannan, Claude

## Context

Notifications are part of the product: market **signals** (e.g. OI buildup changes on a
watchlist), **alerts** the user configures, **account messages** (verification codes,
"password changed") and **system alerts** (Kite token expired — run `mda-login`; feed
disconnected). Users provide email and an Indian mobile number at sign-up (unverified).

Delivery talks to slow, unreliable external services; it must never delay market data,
must not lose messages across restarts, and must not spam.

## Options considered

### Where delivery runs
- **Inside `mda-engine` or `mda-gateway`** — fewer processes, but an SMTP timeout or a
  provider outage would stall analytics or the API.
- **Separate `mda-notifier` process** fed by the bus and the app DB — isolated failures,
  independent restarts, matches the multi-process architecture (ADR-0006).

### Channels (India)
- **Telegram bot** — free, instant, rich formatting, widely used for trading signals;
  long polling works from a laptop with no public URL; the bot can ask the user to share
  their phone number, which can verify it.
- **Email (SMTP)** — free/cheap; good for summaries and account messages; not for
  time-critical signals.
- **SMS** — requires TRAI **DLT** registration (entity, sender ID, every template),
  per-message cost, fixed templates; the standard route for phone OTP.
- **WhatsApp Business** — Meta approval, templates, per-conversation fees.
- **Browser push / in-app** — free; needs the dashboard or browser running.

## Decision

- A separate **`mda-notifier`** process. Producers (engine, gateway, feed) never send
  directly: they create notifications; the notifier delivers them.
- **Transactional outbox** in the app DB: a notification is written as a row
  (`pending`) in the same transaction as whatever caused it; the notifier delivers, retries
  with back-off, and records the outcome. Nothing is lost on a crash; nothing is sent twice
  for the same dedup key.
- **Channels in order:** **Telegram** and **email** first; in-app/browser push next; SMS
  and WhatsApp deferred (DLT / Meta overhead) and only if needed.
- **Phone verification via Telegram:** linking the bot can share the Telegram account's
  phone number; if it matches the registered number, the phone is marked verified — no SMS
  needed. **Email verification** by a one-time code/link sent by email.
- **Users opt in** per channel and per alert type; quiet hours and rate limits per user.
- Signals carry the disclaimer that they are analytics, not investment advice.

## Rationale

Process isolation keeps the data path immune to delivery problems; the outbox gives
at-least-once delivery with idempotence without a message broker. Telegram and email give
real-time and durable channels at zero cost and no regulatory registration.

## Consequences

- Positive: reliable, auditable delivery (every attempt logged); channels can be added
  without touching producers.
- Negative: one more process to supervise; bot token and SMTP credentials to protect
  (`secrets/`, ADR-0005).
- Phone numbers stay unverified for users who don't link Telegram until SMS exists.

## Learning notes

- Outbox pattern, at-least-once delivery, idempotency keys, retries with back-off and jitter.
- External API rate limits (Telegram: ~1 msg/s per chat, ~30 msg/s overall).
- Interview: "How do you guarantee an alert is sent exactly once when the sender can crash?"

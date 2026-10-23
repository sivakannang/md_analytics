# Notification service (mda-notifier)

> Status: **Accepted — building N1–N6 (2026-10-22)** · Decision: [ADR-0016](../adr/0016-notification-service.md)

## 1. Flow

```
producers                      app DB (SQLite)                      mda-notifier
─────────                      ───────────────                      ────────────
mda-engine  (signals)  ─┐
mda-gateway (account)  ─┼─►  notifications (outbox, pending)  ─►  pick due rows ─► channel adapter
mda-feedd   (system)   ─┘      + dedup key                        Telegram / Email
                                                                   └─► sent / retry later / failed
```

- Producers only **insert rows**; they never call Telegram or SMTP.
- Market signals arrive on the analytics bus; the notifier (or engine) matches them against
  each user's **alert rules** and inserts notifications.

## 2. Data model (app DB)

Channel-independent so new channels (WhatsApp, SMS, push) are a new adapter plus a name,
with no schema change: a **notification** is the event; the notifier fans it out into one
**delivery** per channel, and each delivery is retried on its own.

| Table | Purpose |
|---|---|
| `notifications` | The outbox (migration 3): user, type, payload (JSON), email snapshot, **dedup_key** (unique), status `pending` → `dispatched` (deliveries created) or `expired` (older than 24 h when first seen) |
| `notification_deliveries` | One per notification and channel: channel (`email`, `telegram`, …), address snapshot, status `pending`/`sent`/`failed`, attempts, next_attempt_at, sent_at, last error. Address purged after `sent`/`failed` |
| `user_channels` | Per user and channel: address (Telegram `chat_id`; email comes from `users`), `enabled`, linked/verified time |
| `channel_link_codes` | One-time codes: Telegram link (10 min) and email verification (6 digits, 15 min, hashed) |
| `alert_rules` | Later (N7): OI buildup / threshold alerts per instrument |

**Routing** (which channels a type goes to): `account.*` → email (verified or not: it is
how we reach a new user; `account.email_code` only to the address being verified);
`system.*` → admins' Telegram and email. Signals later → the user's enabled channels.

The unique `dedup_key` (e.g. `oi-buildup:<instrument>:<bar time>:<user>`) makes a repeated
signal a no-op instead of a second message.

## 3. Channels

### Telegram (first)

| Aspect | Design |
|---|---|
| Bot | One bot created via @BotFather; token in `secrets/notifier.json` |
| Receiving | **Long polling** (`getUpdates`): works from a laptop behind NAT, no public URL or webhook |
| Linking | Dashboard → "Connect Telegram" → one-time link `https://t.me/<bot>?start=<code>` → user taps Start → bot receives `/start <code>` → stores `chat_id` for that user |
| Phone verification | After linking, the bot asks to **share contact** (Telegram's contact button); if the shared number equals the registered one → `phone_verified_at` set |
| Sending | `sendMessage` (HTML formatting); limits respected: ≤ 1 msg/s per chat, ≤ 30 msg/s overall |
| Transport | HTTPS JSON via our Beast/Asio client (same stack as the Kite REST client) |

### Email (first)

| Aspect | Design |
|---|---|
| Transport | SMTP submission over TLS (port 465, or 587 with STARTTLS) with username/password |
| Provider | Configurable. Start: a Gmail account with an **app password** (personal use); later a provider with our own domain (SPF/DKIM) for deliverability |
| Verification | Six-digit code emailed at request; entered on the Account page → `email_verified_at` |
| Use | Account messages, daily summaries; urgent signals prefer Telegram |
| Implementation | Own SMTP client on Asio + OpenSSL (implicit TLS on 465; AUTH PLAIN; one message per connection), no new dependency |

## 4. Delivery rules

| Rule | Default |
|---|---|
| Retries | Exponential back-off 30 s → 1 h, max 8 attempts, then `failed` (visible to the user) |
| Rate limit | Per user: 20 signals/hour per channel; excess folded into a summary |
| Quiet hours | Per user (e.g. 22:00–07:00 IST); system/account messages exempt |
| Opt-in | Nothing is sent on a channel until it is linked/verified and enabled |
| Content | Signals end with "Analytics only, not investment advice" |
| Privacy | Contact details and message bodies never logged; only ids and status |

## 5. First notification types

| Type | Source | Available |
|---|---|---|
| Kite session rejected (`system.session_expired`) | `mda-feedd` (WebSocket or auto-backfill) | N6: once per IST day, 07:00–16:00 |
| Morning login reminder (`system.login_reminder`) | `mda-notifier`, from the session file's date | N6: Mon–Fri 08:45–15:30 if `mda-login` hasn't run today |
| Feed down / back (`system.feed_down`, `feed_restored`) | `mda-feedd` WebSocket state | N6: not connected for 60 s, Mon–Fri 09:00–15:45; "back" once after a down alert |
| Verification codes, password changed | `mda-gateway` | With N1/N2 |
| OI buildup change on a watchlist instrument | `mda-engine` | After engine + watchlists |
| Price / OI threshold crossed | `mda-engine` + alert rules | After engine |

## 6. Implementation units

| Unit | Scope |
|---|---|
| N1 | Migration 6 (deliveries, user channels, link codes); `mda::notify` library: channel interface, routing, fan-out, retry/back-off, expiry, templates; tests on an in-memory DB with a fake channel |
| N2 | Email channel: SMTP client (Asio, TLS 465, AUTH PLAIN, MIME text), tested against a local fake SMTP server |
| N3 | Telegram channel: Bot API over HTTPS (sendMessage, getUpdates long polling), `/start <code>` linking, contact share → phone verification; rate limits |
| N4 | `mda-notifier` process: config, `secrets/notifier.json`, the delivery loop + Telegram poller; runbook |
| N5 | Gateway + web: Account → Notifications (connect Telegram, verify email, per-channel switches); REST docs |
| N6 | System alerts: feed disconnected/stale and Kite session expired (mda-feedd), morning login reminder (notifier) |
| N7 | Later: alert rules (OI buildup, price/OI thresholds), in-app/browser push, WhatsApp |

## 7. Open questions

- Decided 2026-10-22: Gmail with an app password; own SMTP client; this round delivers
  account messages and system alerts; pending rows older than 24 h expire.
- **SMS:** skip unless needed (DLT registration).

N6 rules live in `notify/system_alerts.hpp` as plain functions; every alert carries a dedup
key (per incident or per day), so restarts never send twice.

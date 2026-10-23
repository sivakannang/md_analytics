# ADR-0027: Remote access over Tailscale

- **Status:** Accepted (TS1–TS2 done 2026-10-23)
- **Date:** 2026-10-23
- **Deciders:** owner, Claude

## Context

The app runs on the owner's Mac and is reachable only on it: mda-gateway listens on
`127.0.0.1:8090`, and the website is Vite's **dev server** on `127.0.0.1:5173`, which proxies
`/api` (REST and the live stream) to the gateway. The owner wants to use it away from home,
and a friend wants to use it too. Nobody else should.

Facts that shape the choice:

- **Two users.** No public audience; the app shouldn't even be discoverable.
- **Kite Connect data** is licensed for the account holder's own use; publishing it would
  breach exchange redistribution rules. Private access for one friend is the most we do.
- **Home internet:** Indian ISPs commonly put homes behind carrier-grade NAT, so an
  incoming port may be impossible to forward at all.
- **Already in place:** Argon2 passwords, rate-limited sign-in, an HttpOnly SameSite=Strict
  session cookie, a WebSocket Origin check, admin routes that check the role, and the client
  IP taken from `X-Forwarded-For` only when the peer is loopback (ADR-0019).
- **Not ready to face a network:** the Vite dev server (built for development; it has had
  file-read vulnerabilities), open signup (`gateway.allow_signup: true`), and a session
  cookie without `Secure` (no HTTPS today).

## Options considered

### Option A — Tailscale (private WireGuard network)
- Pros: the Mac is reachable only from devices in the owner's tailnet or that it is shared
  with; nothing is open to the internet. Works through CGNAT, no router changes. HTTPS with
  a real certificate for `<mac>.<tailnet>.ts.net` (`tailscale serve`). Free for this use.
  End-to-end encrypted; Tailscale's servers only coordinate keys.
- Cons: the friend installs the Tailscale app and accepts an invite. A dependency on
  Tailscale's coordination service (existing connections keep working if it is down).

### Option B — Cloudflare Tunnel + Cloudflare Access
- Pros: a normal HTTPS URL, nothing for the friend to install; an email allowlist (one-time
  code) in front, so strangers stop at Cloudflare.
- Cons: the login page is public (discoverable); Cloudflare terminates TLS and sees the
  traffic; needs a domain on Cloudflare; two logins (Access, then ours).

### Option C — Router port forwarding + dynamic DNS + our own TLS
- Pros: no third party.
- Cons: the gateway faces every scanner on the internet; certificates and renewals are ours;
  likely impossible behind CGNAT.

## Decision

**Option A, Tailscale.** The Mac joins the owner's tailnet; the gateway stays on loopback,
and `tailscale serve` publishes it as `https://<mac>.<tailnet>.ts.net` inside the tailnet
only (**never `tailscale funnel`**, which would make it public). The friend gets the Mac
**shared** to their own tailnet (Tailscale's node sharing): they can reach this one machine
and nothing else on the owner's network.

To serve one origin over HTTPS without the dev server:

1. **The gateway serves the built website** (`npm run build` → `web/dist`): static files with
   long-lived caching for hashed assets, `index.html` for any other non-`/api` path (the
   app's client-side routes), and no directory listings or paths outside the folder. One
   process, one port, one origin for pages, REST and the stream.
2. **`tailscale serve` → `http://127.0.0.1:8090`** terminates HTTPS. It connects from
   loopback, so the existing `X-Forwarded-For` rule gives the real client (tailnet) address.
3. **The session cookie is `Secure` when the request came over HTTPS**: decided per request
   from `X-Forwarded-Proto: https`, trusted only from a loopback peer (as `X-Forwarded-For`).
   Local development over plain `http://127.0.0.1` keeps working.
4. **The WebSocket Origin check** must accept the `ts.net` origin through the proxy (checked
   in TS2: if `tailscale serve` rewrites `Host`, the gateway compares against
   `X-Forwarded-Host` from a loopback peer instead).
5. **Signup closes** (`gateway.allow_signup: false`) once the friend has an account.
6. The Vite dev server stays for development on the Mac only (it is never served by
   Tailscale).

## Rationale

Two known users and data we may not publish: the strongest control is that nobody else can
reach the app at all, which is Tailscale's default. It needs no router or ISP cooperation, and
its HTTPS removes the cookie and mixed-content issues for free. Serving the built site from the
gateway removes the dev server from anything remote and makes production one process instead
of two. Option B's public login page and second login buy nothing for two users who can
install an app.

## Consequences

- Positive: remote access with no open port; real HTTPS; the production site no longer
  depends on Node at run time; the client IP in Admin shows the tailnet device.
- Negative / costs we accept: the friend installs Tailscale; a rebuild (`npm run build`) is
  needed for web changes to reach the served site (the dev server at :5173 still hot-reloads
  for the owner). The Mac must stay awake and online for anyone to use it (today's battery
  episode); launchd supervision and power alerts matter more now.
- Follow-ups: Tailscale's identity headers (`Tailscale-User-Login`) could replace our login
  for tailnet users later; revisit if the friend finds the double login annoying.

## Units

| Unit | Who | Scope |
|---|---|---|
| **TS1** | Claude | Gateway serves `web/dist` (static files, caching, SPA fallback, safe paths) + tests; `gateway.web_dir` config; `tools/stack.sh` builds the site before starting the gateway when it changed |
| **TS2** | Claude | `Secure` cookie from `X-Forwarded-Proto` (loopback only); WebSocket Origin behind the proxy; tests; `rest.md`, `websocket.md` |
| **TS3** | Owner, guided | Install Tailscale on the Mac and sign in; enable MagicDNS and HTTPS certificates in the admin console; `tailscale serve --bg 8090` (Claude can run this with the owner's OK); share the Mac with the friend; the friend installs Tailscale, accepts, signs up; then `allow_signup: false` |
| **TS4** | Claude | Runbook `docs/runbooks/remote-access.md` (start/stop, adding or removing a person, what to check when it doesn't connect); check from a phone on mobile data |

Claude never creates the Tailscale account or signs in for the owner; the owner does the
account steps.

## Learning notes

- WireGuard: a minimal VPN protocol (Noise handshakes, Curve25519, ChaCha20-Poly1305).
  Tailscale adds key distribution, NAT traversal (STUN, DERP relays) and ACLs on top.
- Reverse-proxy trust: which headers (`X-Forwarded-For`, `-Proto`, `-Host`) a server may
  believe, and why only from known peers.
- Cookie attributes: `Secure`, `HttpOnly`, `SameSite`, and why a secure context matters.

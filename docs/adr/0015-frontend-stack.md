# ADR-0015: Frontend — React + TypeScript + Vite, minimal dependencies

- **Status:** Accepted
- **Date:** 2026-10-22
- **Deciders:** Sivakannan, Claude

## Context

The dashboard (sign-in, watchlists, option chain, price/OI charts) runs in the browser
and talks to `mda-gateway` ([ADR-0013](0013-gateway-http-server-and-api-conventions.md)).
The vision listed React + TypeScript + Vite as the proposed stack. The first screens are
sign up / sign in / account; live-data views follow.

## Options considered

- **React + TypeScript + Vite** — the most widely used combination; strong typing for API
  contracts; fast dev server with a proxy; large ecosystem for financial charts.
- **Next.js** — server rendering and routing conventions we don't need (the backend is
  C++); adds a Node server to run.
- **Svelte / Solid** — leaner runtime; smaller ecosystem and less transferable.
- **Plain HTML/JS** — no build step; unmanageable once charts and live state arrive.

## Decision

- **React 19 + TypeScript 5.9 (strict) + Vite 8**, in `web/`; React Router for pages.
- **No UI component library or CSS framework** for now: plain CSS with design tokens
  (CSS variables). Fewer dependencies, full control; revisit if screens multiply.
- **Same-origin API access:** in development Vite proxies `/api` to `mda-gateway`
  (`MDA_GATEWAY_URL`, default `http://127.0.0.1:8090`), so the `SameSite=Strict`,
  `HttpOnly` session cookie works without CORS. In production the gateway (or a reverse
  proxy) serves the built files from the same origin.
- **One typed API client** (`src/api/`) that understands the Kite-style envelope and
  turns `{"status":"error"}` into typed errors; components never call `fetch` directly.
- Versions pinned in `package-lock.json`; `npm ci` for reproducible installs.

## Rationale

The mainstream stack with the best typing and tooling, kept deliberately small. TypeScript
5.9 over the newer native TypeScript 7 for ecosystem compatibility; revisit later.

## Consequences

- Positive: fast iteration (hot reload); types catch API mismatches at build time.
- Negative: a Node toolchain beside the C++ one (dev only — production is static files).
- CI gets a `web` job (install, typecheck, build) once the front end grows.

## Learning notes

- Same-origin vs CORS; why the dev proxy keeps cookie security intact.
- Typed API boundaries; error envelopes; auth state in a React context.

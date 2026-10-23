# ADR-0005: Keep secrets out of the public repository

- **Status:** Proposed
- **Date:** 2026-10-19
- **Deciders:** Sivakannan, Claude

## Context

The repository is public. The system handles:

| Secret | Lifetime | Impact if leaked |
|---|---|---|
| Kite `api_key` | Long-lived | Low alone, but identifies the app |
| Kite `api_secret` | Long-lived | **High** — with a `request_token` it mints access tokens |
| Kite `access_token` | Until ~6 AM next day | **High** — full API access to the account (market data now; orders in Phase 4) |
| Future: other brokers, DB passwords, user credentials | Varies | High |

Making a repo private after a leak does not undo it: clones, forks and secret
scanners may already have the data. Prevention is the only real defence.

## Options considered

- **Config files with secrets, just be careful** — relies on memory; one `git add -A` away from a leak.
- **Environment variables only** — safe from git, but awkward for multi-field broker sessions and tokens that change daily.
- **Git-ignored `secrets/` directory + env-var overrides + automated scanning** — files are convenient for tokens written by the login helper; scanning catches mistakes.
- **OS keychain / secret manager (macOS Keychain, Vault, AWS Secrets Manager)** — strongest, but extra complexity before we deploy anywhere.

## Decision

1. Secrets live only in the **git-ignored `secrets/`** directory (e.g.
   `secrets/kite.yaml` for key/secret, `secrets/kite_session.json` for the daily token),
   created with permissions **`0600`**.
2. Checked-in config (`config/*.yaml`) contains **no secrets** — only references such
   as `secrets_file: secrets/kite.yaml`. Environment variables (`MDA_KITE_API_KEY`, …)
   override files, for CI and servers.
3. A committed **`config/secrets.example.yaml`** documents the expected fields with fake values.
4. **gitleaks** runs in a git **pre-commit hook** (`scripts/git-hooks/`, enabled with
   `git config core.hooksPath scripts/git-hooks`) and in **CI** on every push.
5. Secrets are **never logged**: the logging layer redacts known secret fields; tokens
   are shown at most as a short prefix (`abcd…`).
6. **If a secret leaks:** regenerate the API secret in the Kite developer console,
   invalidate the session, then clean history — in that order.
7. Revisit OS keychain / a secret manager when we deploy to a server (Phase 3).

## Rationale

Layered defence at low cost: `.gitignore` prevents the common mistake, the pre-commit
hook catches the uncommon one locally, CI catches anything that bypassed the hook, and
redaction prevents leaks through logs and screenshots.

## Consequences

- Positive: safe to keep the repo public; the setup is documented and repeatable.
- Negative: `gitleaks` must be installed locally; hooks must be enabled once per clone.

## Learning notes

- Why rotating a leaked secret matters more than rewriting git history.
- Threat model for a trading system: what an attacker can do with each credential.
- Interview: "How do you handle credentials in a production trading system?"

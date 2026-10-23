# Runbook: daily Kite login

Kite access tokens expire every morning (~06:00 IST). Each trading day, before starting
`mda-feedd`, log in once with `mda-login` (about 10 seconds).

## One-time setup

1. `secrets/kite_session.json` (git-ignored, mode 600) must contain your app's API key
   and secret — see [`config/kite_session.example.json`](../../config/kite_session.example.json):

   ```json
   {"api_key": "…", "api_secret": "…", "access_token": "written daily by mda-login"}
   ```

2. Your Kite Connect app's **redirect URL** must be
   `http://127.0.0.1:8080/auth/zerodha/callback` (set in the Kite developer console).
3. Port 8080 must be free while `mda-login` runs (stop any other app using it).

## Every trading day

```bash
build/release/engine/apps/mda-login/mda-login
```

1. Your browser opens Kite's login page; log in (password + TOTP) as usual.
2. The browser shows "Logged in"; the terminal shows:

   ```
   logged in   user AB1234; access token wxyz… saved to secrets/kite_session.json
   ```

3. Optional check: `build/release/engine/apps/tools/mda-kite-check` validates the token
   and imports today's instruments.

`mda-login` waits up to 5 minutes (`--timeout`). `--no-browser` prints the URL instead of
opening it.

## Exit codes and problems

| Exit | Meaning | What to do |
|---|---|---|
| 0 | Logged in, token saved | — |
| 2 | Kite rejected the login or token exchange | Check api_key/api_secret belong to the same app; log in again |
| 1 | Anything else | Read the message |

| Message | Cause |
|---|---|
| `cannot listen on 127.0.0.1:8080 … address already in use` | Another app is on port 8080 |
| `session file lacks a required field` | `api_key` or `api_secret` missing |
| `Login not recognised` in the browser | A callback from an older `mda-login` run; just log in again from the new URL |
| `no successful login received within 300 s` | Timed out — run it again |

## Security notes

- The listener binds to **127.0.0.1 only**, and accepts the callback only with the random
  `state` it generated (sent via Kite's `redirect_params`).
- The session file is rewritten atomically with mode 600; secrets are never printed in
  full (first 4 characters only).

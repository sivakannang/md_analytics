#pragma once

// Kite session credentials (docs/design/kite-connectivity.md §2, ADR-0005).
//
// Source, first match wins:
//   1. environment: MDA_KITE_API_KEY and MDA_KITE_ACCESS_TOKEN
//   2. file (default secrets/kite_session.json):
//      {"api_key": "...", "api_secret": "...", "access_token": "..."}
//
// `api_secret` is only needed by mda-login (to create the daily token); `access_token`
// is written by mda-login each morning. Secrets never appear in logs: use redact().

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace mda::kite {

struct Session {
    std::string api_key;
    std::string access_token;
    std::string api_secret;  ///< optional; only mda-login needs it
    std::string user_id;     ///< optional; written by mda-login for information
};

enum class SessionError {
    NotFound,      ///< no environment variables and no file
    Unreadable,    ///< file exists but can't be read
    Malformed,     ///< not valid JSON / not an object
    MissingField,  ///< a required field is missing or empty
    WriteFailed,   ///< could not write the file
};

struct LoadedSession {
    Session session;
    std::string source;                 ///< "environment" or the file path
    bool insecure_permissions = false;  ///< file readable by group/others (should be 0600)
};

/// Loads credentials for API use: api_key and access_token are required.
[[nodiscard]] std::expected<LoadedSession, SessionError> load_session(
    const std::filesystem::path& file);

/// Loads the file only (no environment), requiring api_key and api_secret; access_token
/// may be missing or stale. Used by mda-login.
[[nodiscard]] std::expected<LoadedSession, SessionError> load_login_credentials(
    const std::filesystem::path& file);

/// Writes the session atomically (temp file + fsync + rename) with mode 0600, so a crash
/// never leaves a half-written file and the secret is never world-readable.
[[nodiscard]] std::expected<void, SessionError> save_session(const std::filesystem::path& file,
                                                             const Session& session);

[[nodiscard]] std::string_view to_string(SessionError error) noexcept;

/// First 4 characters followed by an ellipsis — safe to log.
[[nodiscard]] std::string redact(std::string_view secret);

}  // namespace mda::kite

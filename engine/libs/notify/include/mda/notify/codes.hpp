#pragma once

// One-time codes for linking a channel and verifying an address (docs/design/notifications.md
// §2-3), in channel_codes. Only a SHA-256 hash is stored. A Telegram link code is random
// and unique on its own; a 6-digit email code is hashed with its user id, so two users can
// hold the same digits. A code is consumed by its first successful use.

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include <mda/storage/database.hpp>

namespace mda::notify {

inline constexpr std::string_view kTelegramLink = "telegram_link";
inline constexpr std::string_view kEmailVerify = "email_verify";

/// A new Telegram link code (32 hex characters, valid for `ttl`); replaces the user's
/// earlier unused one.
[[nodiscard]] std::expected<std::string, storage::DbError> create_link_code(storage::Database& db, std::int64_t user_id,
                                                                            std::chrono::seconds ttl, std::int64_t now);

/// The user a Telegram link code belongs to, consuming it; nothing if unknown or expired.
[[nodiscard]] std::expected<std::optional<std::int64_t>, storage::DbError> consume_link_code(storage::Database& db,
                                                                                             std::string_view code,
                                                                                             std::int64_t now);

inline constexpr std::chrono::seconds kEmailCodeTtl{15 * 60};
inline constexpr std::chrono::seconds kEmailCodeGap{60};  ///< between two codes for one user
inline constexpr int kEmailCodeGuesses = 5;               ///< wrong guesses before a code dies

/// A new 6-digit email code for the user, replacing the earlier one; nothing if the
/// earlier one was issued less than kEmailCodeGap ago.
[[nodiscard]] std::expected<std::optional<std::string>, storage::DbError> create_email_code(storage::Database& db,
                                                                                            std::int64_t user_id,
                                                                                            std::int64_t now);

/// Whether `code` is the user's current email code (consumed if so). A wrong guess
/// counts; the kEmailCodeGuesses-th wrong guess deletes the code.
[[nodiscard]] std::expected<bool, storage::DbError> consume_email_code(storage::Database& db, std::int64_t user_id,
                                                                       std::string_view code, std::int64_t now);

}  // namespace mda::notify

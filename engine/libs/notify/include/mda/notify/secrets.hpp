#pragma once

// secrets/notifier.json (git-ignored, 600): the Telegram bot token and the SMTP login.
// Each section is optional; a missing one turns that channel off. Template:
// config/notifier.example.json. Errors name the field, never its value.

#include <expected>
#include <filesystem>
#include <optional>
#include <string>

#include <mda/notify/email_channel.hpp>
#include <mda/notify/telegram.hpp>

namespace mda::notify {

struct NotifierSecrets {
    std::optional<TelegramOptions> telegram;
    std::optional<SmtpOptions> smtp;
    bool insecure_permissions = false;  ///< readable by group or others
};

[[nodiscard]] std::expected<NotifierSecrets, std::string> load_notifier_secrets(const std::filesystem::path& path);

/// The same, from the file's text (tests).
[[nodiscard]] std::expected<NotifierSecrets, std::string> parse_notifier_secrets(std::string_view text);

}  // namespace mda::notify

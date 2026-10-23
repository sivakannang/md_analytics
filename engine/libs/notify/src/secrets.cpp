#include <fstream>
#include <sstream>

#include <sys/stat.h>

#include <boost/json.hpp>

#include <mda/notify/secrets.hpp>

namespace mda::notify {
namespace {

namespace json = boost::json;

/// A required non-empty string field, or an error naming it.
std::expected<std::string, std::string> required(const json::object& section, std::string_view section_name,
                                                 std::string_view key) {
    const auto* value = section.if_contains(key);
    if (value == nullptr || !value->is_string() || value->as_string().empty()) {
        return std::unexpected(std::string(section_name) + "." + std::string(key) + " is missing or not a string");
    }
    return std::string(value->as_string());
}

std::string optional_text(const json::object& section, std::string_view key, std::string fallback) {
    const auto* value = section.if_contains(key);
    return value != nullptr && value->is_string() && !value->as_string().empty() ? std::string(value->as_string())
                                                                                 : fallback;
}

}  // namespace

std::expected<NotifierSecrets, std::string> parse_notifier_secrets(std::string_view text) {
    boost::system::error_code ec;
    const json::value parsed = json::parse(text, ec);
    if (ec || !parsed.is_object()) {
        return std::unexpected("not a JSON object");
    }
    const auto& root = parsed.as_object();
    NotifierSecrets secrets;
    if (const auto* telegram = root.if_contains("telegram"); telegram != nullptr && telegram->is_object()) {
        auto token = required(telegram->as_object(), "telegram", "bot_token");
        if (!token) {
            return std::unexpected(token.error());
        }
        secrets.telegram = TelegramOptions{.token = std::move(*token)};
    }
    if (const auto* smtp = root.if_contains("smtp"); smtp != nullptr && smtp->is_object()) {
        const auto& section = smtp->as_object();
        SmtpOptions options;
        for (const auto& [key, field] : {std::pair{"host", &options.host}, std::pair{"username", &options.username},
                                         std::pair{"password", &options.password},
                                         std::pair{"from_address", &options.from_address}}) {
            auto value = required(section, "smtp", key);
            if (!value) {
                return std::unexpected(value.error());
            }
            *field = std::move(*value);
        }
        options.port = optional_text(section, "port", "465");
        options.from_name = optional_text(section, "from_name", "md_analytics");
        if (!plausible_email(options.from_address)) {
            return std::unexpected("smtp.from_address is not an email address");
        }
        secrets.smtp = std::move(options);
    }
    return secrets;
}

std::expected<NotifierSecrets, std::string> load_notifier_secrets(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        return std::unexpected("cannot read " + path.string());
    }
    std::ostringstream text;
    text << file.rdbuf();
    auto secrets = parse_notifier_secrets(text.str());
    if (!secrets) {
        return std::unexpected(path.string() + ": " + secrets.error());
    }
    struct stat info{};
    if (::stat(path.c_str(), &info) == 0) {
        secrets->insecure_permissions = (info.st_mode & (S_IRWXG | S_IRWXO)) != 0;
    }
    return secrets;
}

}  // namespace mda::notify

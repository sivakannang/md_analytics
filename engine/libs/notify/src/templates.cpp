#include <string>

#include <boost/json.hpp>

#include <mda/notify/templates.hpp>

namespace mda::notify {
namespace {

namespace json = boost::json;

std::string field(const json::object* payload, std::string_view key) {
    if (payload == nullptr) {
        return {};
    }
    const auto* value = payload->if_contains(key);
    if (value == nullptr) {
        return {};
    }
    if (value->is_string()) {
        return std::string(value->as_string());
    }
    return json::serialize(*value);
}

constexpr std::string_view kFooter = "\n\n— md_analytics";

}  // namespace

Message render(std::string_view type, std::string_view payload) {
    boost::system::error_code ec;
    const json::value parsed = json::parse(payload, ec);
    const json::object* object = !ec && parsed.is_object() ? &parsed.as_object() : nullptr;
    const std::string name = field(object, "username");
    const std::string hello = name.empty() ? "Hello," : "Hi " + name + ",";
    const auto message = [&](std::string subject, std::string body) {
        return Message{.subject = std::move(subject), .text = body + std::string(kFooter)};
    };

    if (type == "account.signup") {
        return message("Welcome to md_analytics", hello + "\n\nYour md_analytics account is ready. You can sign in "
                                                          "with your username at any time.");
    }
    if (type == "account.password_changed") {
        return message("Your password was changed",
                       hello + "\n\nThe password of your md_analytics account was just changed. If you didn't "
                               "do this, tell the admin at once.");
    }
    if (type == "account.deleted") {
        return message("Your account was deleted",
                       hello + "\n\nYour md_analytics account and its watchlists have been deleted.");
    }
    if (type == "account.email_code") {
        return message("Your verification code",
                       hello + "\n\nYour md_analytics verification code is " + field(object, "code") +
                           ". It expires in 15 minutes. If you didn't ask for it, ignore this message.");
    }
    if (type == "system.feed_down") {
        return message("Market feed " + field(object, "state"),
                       "mda-feedd reports the Kite feed as " + field(object, "state") + " since " +
                           field(object, "since") + " IST. " + field(object, "detail"));
    }
    if (type == "system.feed_restored") {
        return message("Market feed back", "mda-feedd is receiving market data again since " +
                                               field(object, "since") + " IST.");
    }
    if (type == "system.session_expired") {
        return message("Kite session expired",
                       "Kite rejected today's session (" + field(object, "process") +
                           "). Run mda-login, then restart mda-feedd.");
    }
    if (type == "system.login_reminder") {
        return message("Kite login needed", "There is no Kite session for today yet. Run mda-login before 09:15 IST.");
    }
    return message(std::string(type), std::string(payload));
}

}  // namespace mda::notify

#pragma once

// Telegram (docs/design/notifications.md §3): the Bot API over HTTPS on our Asio/Beast stack.
// - TelegramChannel sends deliveries (sendMessage), at most one message per chat per
//   second and ~25 per second overall (Telegram's limits).
// - The bot side long-polls getUpdates (no public URL needed) and handles linking
//   ("/start <code>" from the dashboard's link), phone verification (the user shares their
//   contact) and "/stop".
// The bot token is in every request path: it is never logged, and errors never contain it.

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <boost/asio/ssl/context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/json/value.hpp>

#include <mda/notify/channel.hpp>
#include <mda/storage/database.hpp>

namespace mda::notify {

struct TelegramOptions {
    std::string token;  ///< from @BotFather; secret
    std::string host = "api.telegram.org";
    std::string port = "443";
    std::chrono::seconds timeout{30};  ///< per request (long polls add poll_timeout)
    std::chrono::seconds poll_timeout{25};
    std::chrono::milliseconds per_chat_gap{1100};
    std::chrono::milliseconds global_gap{40};
};

struct TelegramError {
    int code = 0;             ///< Telegram's error_code (HTTP status); 0 = network or bad reply
    std::string description;  ///< Telegram's description or the network error
    [[nodiscard]] std::string what(std::string_view method) const;
};

class TelegramApi {
public:
    TelegramApi(TelegramOptions options, boost::asio::ssl::context& tls)
        : options_(std::move(options)), tls_(&tls) {}

    /// POST /bot<token>/<method> with a JSON body; the "result" of an ok reply.
    boost::asio::awaitable<std::expected<boost::json::value, TelegramError>> call(
        std::string_view method, const boost::json::object& body, std::chrono::seconds timeout);

    [[nodiscard]] const TelegramOptions& options() const { return options_; }

private:
    TelegramOptions options_;
    boost::asio::ssl::context* tls_;
};

struct OutgoingMessage {
    enum class Keyboard { None, ShareContact, Remove };
    std::string chat_id;
    std::string text;
    Keyboard keyboard = Keyboard::None;
};

/// The sendMessage body.
[[nodiscard]] boost::json::object to_json(const OutgoingMessage& message);

class TelegramChannel : public Channel {
public:
    explicit TelegramChannel(TelegramApi& api) : api_(&api) {}

    [[nodiscard]] std::string_view name() const override { return "telegram"; }
    boost::asio::awaitable<SendResult> send(const Delivery& delivery) override;

private:
    boost::asio::awaitable<void> pace(const std::string& chat_id);

    TelegramApi* api_;
    std::map<std::string, std::chrono::steady_clock::time_point> last_per_chat_;
    std::chrono::steady_clock::time_point last_any_{};
};

/// The parts of a Telegram update the bot uses.
struct Update {
    std::int64_t update_id = 0;
    std::string chat_id;
    bool private_chat = false;
    std::int64_t from_id = 0;
    std::string text;
    std::optional<std::string> contact_phone;  ///< a shared contact
    std::int64_t contact_user_id = 0;          ///< whose contact it is (0: not a Telegram user)
};

[[nodiscard]] std::vector<Update> parse_updates(const boost::json::value& result);

/// Answers the bot's private chats: links accounts, verifies phones, turns alerts off.
class TelegramLinker {
public:
    explicit TelegramLinker(storage::Database& db) : db_(&db) {}

    /// The reply to send, if any.
    [[nodiscard]] std::expected<std::optional<OutgoingMessage>, storage::DbError> handle(
        const Update& update, std::int64_t now);

private:
    storage::Database* db_;
};

/// One long poll: fetches updates after `offset`, handles them and sends the replies.
/// Returns the next offset (unchanged when the poll failed).
boost::asio::awaitable<std::int64_t> poll_once(TelegramApi& api, TelegramLinker& linker,
                                               std::int64_t offset, std::int64_t now);

}  // namespace mda::notify

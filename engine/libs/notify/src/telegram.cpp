#include <algorithm>

#include <boost/asio/redirect_error.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/json.hpp>
#include <openssl/ssl.h>

#include <mda/notify/codes.hpp>
#include <mda/notify/telegram.hpp>

namespace mda::notify {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace json = boost::json;

using storage::DbError;
using storage::StepResult;

constexpr std::size_t kBodyLimit = 1024 * 1024;

std::string last_digits(std::string_view phone, std::size_t count) {
    std::string digits;
    for (const char c : phone) {
        if (c >= '0' && c <= '9') {
            digits += c;
        }
    }
    return digits.size() >= count ? digits.substr(digits.size() - count) : std::string{};
}

const json::object* object_at(const json::object& parent, std::string_view key) {
    const auto* value = parent.if_contains(key);
    return value != nullptr && value->is_object() ? &value->as_object() : nullptr;
}

std::string text_at(const json::object& parent, std::string_view key) {
    const auto* value = parent.if_contains(key);
    return value != nullptr && value->is_string() ? std::string(value->as_string()) : std::string{};
}

std::int64_t int_at(const json::object& parent, std::string_view key) {
    const auto* value = parent.if_contains(key);
    return value != nullptr && value->is_int64() ? value->as_int64() : 0;
}

OutgoingMessage reply(const Update& update, std::string text,
                      OutgoingMessage::Keyboard keyboard = OutgoingMessage::Keyboard::None) {
    return {.chat_id = update.chat_id, .text = std::move(text), .keyboard = keyboard};
}

constexpr std::string_view kHelp =
    "This bot sends md_analytics alerts. To connect it, open md_analytics → Account → Notifications → "
    "Connect Telegram. Send /stop to turn the alerts off.";

}  // namespace

std::string TelegramError::what(std::string_view method) const {
    return "telegram " + std::string(method) + ": " + (code != 0 ? std::to_string(code) + " " : "") + description;
}

asio::awaitable<std::expected<json::value, TelegramError>> TelegramApi::call(std::string_view method,
                                                                              const json::object& body,
                                                                              std::chrono::seconds timeout) {
    const auto executor = co_await asio::this_coro::executor;
    boost::system::error_code ec;
    const auto token = asio::redirect_error(asio::use_awaitable, ec);
    const auto network = [&](std::string_view step) {
        return std::unexpected(TelegramError{.code = 0, .description = std::string(step) + ": " + ec.message()});
    };

    asio::ip::tcp::resolver resolver(executor);
    const auto endpoints = co_await resolver.async_resolve(options_.host, options_.port, token);
    if (ec) {
        co_return network("resolve");
    }
    beast::ssl_stream<beast::tcp_stream> stream(executor, *tls_);
    if (SSL_set_tlsext_host_name(stream.native_handle(), options_.host.c_str()) != 1) {
        co_return std::unexpected(TelegramError{.description = "could not set SNI"});
    }
    stream.set_verify_callback(asio::ssl::host_name_verification(options_.host));
    auto& tcp = beast::get_lowest_layer(stream);
    tcp.expires_after(options_.timeout);
    co_await tcp.async_connect(endpoints, token);
    if (ec) {
        co_return network("connect");
    }
    tcp.expires_after(options_.timeout);
    co_await stream.async_handshake(asio::ssl::stream_base::client, token);
    if (ec) {
        co_return network("handshake");
    }

    http::request<http::string_body> request{http::verb::post, "/bot" + options_.token + "/" + std::string(method), 11};
    request.set(http::field::host, options_.host);
    request.set(http::field::content_type, "application/json");
    request.body() = json::serialize(body);
    request.prepare_payload();
    tcp.expires_after(options_.timeout);
    co_await http::async_write(stream, request, token);
    if (ec) {
        co_return network("write");
    }
    beast::flat_buffer buffer;
    http::response_parser<http::string_body> parser;
    parser.body_limit(kBodyLimit);
    tcp.expires_after(timeout);
    co_await http::async_read(stream, buffer, parser, token);
    if (ec) {
        co_return network("read");
    }
    const auto response = parser.release();
    tcp.expires_after(std::chrono::seconds(2));
    co_await stream.async_shutdown(token);  // servers often just close; errors don't matter

    boost::system::error_code parse_error;
    json::value parsed = json::parse(response.body(), parse_error);
    if (parse_error || !parsed.is_object()) {
        co_return std::unexpected(
            TelegramError{.code = static_cast<int>(response.result_int()), .description = "reply is not JSON"});
    }
    auto& reply = parsed.as_object();
    const auto* ok = reply.if_contains("ok");
    if (ok != nullptr && ok->is_bool() && ok->as_bool()) {
        co_return reply["result"];
    }
    const std::int64_t code = int_at(reply, "error_code");
    co_return std::unexpected(TelegramError{.code = code != 0 ? static_cast<int>(code) : static_cast<int>(response.result_int()),
                                            .description = text_at(reply, "description")});
}

json::object to_json(const OutgoingMessage& message) {
    json::object body{{"chat_id", message.chat_id}, {"text", message.text}, {"disable_web_page_preview", true}};
    if (message.keyboard == OutgoingMessage::Keyboard::ShareContact) {
        body["reply_markup"] = json::object{
            {"keyboard", json::array{json::array{json::object{{"text", "Share phone number"}, {"request_contact", true}}}}},
            {"one_time_keyboard", true},
            {"resize_keyboard", true}};
    } else if (message.keyboard == OutgoingMessage::Keyboard::Remove) {
        body["reply_markup"] = json::object{{"remove_keyboard", true}};
    }
    return body;
}

asio::awaitable<void> TelegramChannel::pace(const std::string& chat_id) {
    const auto now = std::chrono::steady_clock::now();
    auto ready = last_any_ + api_->options().global_gap;
    if (const auto found = last_per_chat_.find(chat_id); found != last_per_chat_.end()) {
        ready = std::max(ready, found->second + api_->options().per_chat_gap);
    }
    if (ready > now) {
        asio::steady_timer timer(co_await asio::this_coro::executor, ready);
        boost::system::error_code ignored;
        co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
    }
    last_any_ = last_per_chat_[chat_id] = std::chrono::steady_clock::now();
}

asio::awaitable<SendResult> TelegramChannel::send(const Delivery& delivery) {
    co_await pace(delivery.address);
    const OutgoingMessage message{.chat_id = delivery.address,
                                  .text = delivery.message.subject + "\n\n" + delivery.message.text};
    const auto sent = co_await api_->call("sendMessage", to_json(message), api_->options().timeout);
    if (sent) {
        co_return SendResult{.outcome = SendOutcome::Sent};
    }
    // 400 (chat not found) and 403 (the user blocked the bot) won't get better; a bad token
    // (401), rate limits (429) and server or network trouble might.
    const int code = sent.error().code;
    const bool permanent = code == 400 || code == 403;
    co_return SendResult{.outcome = permanent ? SendOutcome::Fail : SendOutcome::Retry,
                         .error = sent.error().what("sendMessage")};
}

std::vector<Update> parse_updates(const json::value& result) {
    std::vector<Update> out;
    if (!result.is_array()) {
        return out;
    }
    for (const auto& item : result.as_array()) {
        if (!item.is_object()) {
            continue;
        }
        const auto& object = item.as_object();
        Update update{.update_id = int_at(object, "update_id")};
        if (const auto* message = object_at(object, "message")) {
            if (const auto* chat = object_at(*message, "chat")) {
                update.chat_id = std::to_string(int_at(*chat, "id"));
                update.private_chat = text_at(*chat, "type") == "private";
            }
            if (const auto* from = object_at(*message, "from")) {
                update.from_id = int_at(*from, "id");
            }
            update.text = text_at(*message, "text");
            if (const auto* contact = object_at(*message, "contact")) {
                update.contact_phone = text_at(*contact, "phone_number");
                update.contact_user_id = int_at(*contact, "user_id");
            }
        }
        out.push_back(std::move(update));
    }
    return out;
}

std::expected<std::optional<OutgoingMessage>, DbError> TelegramLinker::handle(const Update& update, std::int64_t now) {
    using Keyboard = OutgoingMessage::Keyboard;
    if (!update.private_chat || update.chat_id.empty()) {
        return std::optional<OutgoingMessage>{};  // groups and channels: not ours
    }
    const auto linked_user = [&]() -> std::expected<std::optional<std::int64_t>, DbError> {
        auto query = db_->prepare("SELECT user_id FROM user_channels WHERE channel = 'telegram' AND address = ?1");
        if (!query) {
            return std::unexpected(query.error());
        }
        query->bind(1, std::string_view(update.chat_id));
        const auto row = query->step();
        if (!row) {
            return std::unexpected(row.error());
        }
        return *row == StepResult::Row ? std::optional(query->column_int64(0)) : std::nullopt;
    };

    if (update.text == "/start" || update.text.starts_with("/start ")) {
        const std::string code = update.text.size() > 7 ? update.text.substr(7) : std::string{};
        if (code.empty()) {
            return std::optional(reply(update, std::string(kHelp)));
        }
        const auto user = consume_link_code(*db_, code, now);
        if (!user) {
            return std::unexpected(user.error());
        }
        if (!*user) {
            return std::optional(reply(update,
                                       "This link has expired or was already used. Open Account → Notifications in "
                                       "md_analytics and connect Telegram again."));
        }
        auto transaction = storage::Transaction::begin(*db_);
        if (!transaction) {
            return std::unexpected(transaction.error());
        }
        // A chat belongs to one account: linking it elsewhere unlinks it here.
        auto unlink = db_->prepare("DELETE FROM user_channels WHERE channel = 'telegram' AND address = ?1 AND user_id <> ?2");
        auto link = db_->prepare(
            "INSERT INTO user_channels (user_id, channel, address, enabled, linked_at) VALUES (?1, 'telegram', ?2, 1, ?3) "
            "ON CONFLICT (user_id, channel) DO UPDATE SET address = excluded.address, enabled = 1, "
            "linked_at = excluded.linked_at");
        if (!unlink || !link) {
            return std::unexpected(!unlink ? unlink.error() : link.error());
        }
        unlink->bind(1, std::string_view(update.chat_id)).bind(2, **user);
        link->bind(1, **user).bind(2, std::string_view(update.chat_id)).bind(3, now);
        if (auto done = unlink->run(); !done) {
            return std::unexpected(done.error());
        }
        if (auto done = link->run(); !done) {
            return std::unexpected(done.error());
        }
        if (auto committed = transaction->commit(); !committed) {
            return std::unexpected(committed.error());
        }
        return std::optional(reply(update,
                                   "Connected: md_analytics alerts will arrive here.\n\nTap \"Share phone number\" "
                                   "below to verify the phone number on your account.",
                                   Keyboard::ShareContact));
    }

    if (update.contact_phone) {
        const auto user = linked_user();
        if (!user) {
            return std::unexpected(user.error());
        }
        if (!*user) {
            return std::optional(reply(update, std::string(kHelp), Keyboard::Remove));
        }
        if (update.contact_user_id != update.from_id) {
            return std::optional(reply(update, "Please share your own contact, with the button below.",
                                       Keyboard::ShareContact));
        }
        auto phone = db_->prepare("SELECT phone FROM users WHERE id = ?1");
        if (!phone) {
            return std::unexpected(phone.error());
        }
        phone->bind(1, **user);
        const auto row = phone->step();
        if (!row) {
            return std::unexpected(row.error());
        }
        const std::string registered =
            *row == StepResult::Row && !phone->column_is_null(0) ? last_digits(phone->column_text(0), 10) : std::string{};
        if (registered.empty() || registered != last_digits(*update.contact_phone, 10)) {
            return std::optional(reply(update,
                                       "That number doesn't match the phone number on your md_analytics account, so "
                                       "it stays unverified.",
                                       Keyboard::Remove));
        }
        auto verify = db_->prepare("UPDATE users SET phone_verified_at = ?2 WHERE id = ?1");
        if (!verify) {
            return std::unexpected(verify.error());
        }
        verify->bind(1, **user).bind(2, now);
        if (auto done = verify->run(); !done) {
            return std::unexpected(done.error());
        }
        return std::optional(reply(update, "Phone number verified.", Keyboard::Remove));
    }

    if (update.text == "/stop") {
        auto off = db_->prepare("UPDATE user_channels SET enabled = 0 WHERE channel = 'telegram' AND address = ?1");
        if (!off) {
            return std::unexpected(off.error());
        }
        off->bind(1, std::string_view(update.chat_id));
        if (auto done = off->run(); !done) {
            return std::unexpected(done.error());
        }
        return std::optional(reply(update,
                                   "Telegram alerts are off. Turn them on again under Account → Notifications in "
                                   "md_analytics."));
    }
    return std::optional(reply(update, std::string(kHelp)));
}

asio::awaitable<std::int64_t> poll_once(TelegramApi& api, TelegramLinker& linker, std::int64_t offset, std::int64_t now) {
    const json::object request{{"offset", offset},
                               {"timeout", api.options().poll_timeout.count()},
                               {"allowed_updates", json::array{"message"}}};
    const auto result = co_await api.call("getUpdates", request, api.options().timeout + api.options().poll_timeout);
    if (!result) {
        co_return offset;
    }
    for (const Update& update : parse_updates(*result)) {
        offset = std::max(offset, update.update_id + 1);  // confirmed at the next poll
        const auto answer = linker.handle(update, now);
        if (answer && *answer) {
            (void)co_await api.call("sendMessage", to_json(**answer), api.options().timeout);
        }
    }
    co_return offset;
}

}  // namespace mda::notify

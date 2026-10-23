// Telegram: update parsing and the linker on an in-memory DB, then the channel and the
// long poll against a fake Bot API server (HTTPS on 127.0.0.1, test certificate).

#include <functional>
#include <future>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <mda/appdb/schema.hpp>
#include <mda/notify/codes.hpp>
#include <mda/notify/telegram.hpp>

#include "test_tls.hpp"

namespace mda::notify {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace json = boost::json;
using asio::ip::tcp;

constexpr std::int64_t kNow = 1'792'690'000;

Update message(std::string text, std::string chat = "555") {
    return {.update_id = 1,
            .chat_id = std::move(chat),
            .private_chat = true,
            .from_id = 42,
            .text = std::move(text)};
}

class LinkerTest : public ::testing::Test {
protected:
    LinkerTest() : db_(storage::Database::open_in_memory().value()) {
        EXPECT_TRUE(db_.migrate(appdb::migrations()).has_value());
        auto insert = db_.prepare(
            "INSERT INTO users (username, password_hash, created_at, password_changed_at, phone) "
            "VALUES ('asha', 'x', 0, 0, '98765 43210')");
        EXPECT_TRUE(insert->run().has_value());
    }

    std::string text(const std::string& sql) {
        auto query = db_.prepare(sql);
        if (query->step().value() != storage::StepResult::Row || query->column_is_null(0)) {
            return "NULL";
        }
        return query->column_text(0);
    }

    OutgoingMessage handle(const Update& update) {
        return linker_.handle(update, kNow).value().value();
    }

    storage::Database db_;
    TelegramLinker linker_{db_};
};

TEST(TelegramParseTest, ReadsTextAndContactMessages) {
    const auto updates = parse_updates(json::parse(R"([
        {"update_id": 10, "message": {"chat": {"id": 555, "type": "private"}, "from": {"id": 42},
                                      "text": "/start abc"}},
        {"update_id": 11, "message": {"chat": {"id": -100, "type": "group"}, "from": {"id": 42},
                                      "contact": {"phone_number": "+919876543210", "user_id": 42}}}
    ])"));
    ASSERT_EQ(updates.size(), 2u);
    EXPECT_EQ(updates[0].chat_id, "555");
    EXPECT_TRUE(updates[0].private_chat);
    EXPECT_EQ(updates[0].text, "/start abc");
    EXPECT_FALSE(updates[1].private_chat);
    EXPECT_EQ(updates[1].contact_phone, std::optional<std::string>("+919876543210"));
    EXPECT_EQ(updates[1].contact_user_id, 42);
}

TEST_F(LinkerTest, StartWithACodeLinksTheChat) {
    const auto code = create_link_code(db_, 1, std::chrono::minutes(10), kNow).value();
    const auto answer = handle(message("/start " + code));
    EXPECT_EQ(answer.chat_id, "555");
    EXPECT_EQ(answer.keyboard, OutgoingMessage::Keyboard::ShareContact);
    EXPECT_EQ(
        text("SELECT address || ' ' || enabled FROM user_channels WHERE channel = 'telegram'"),
        "555 1");

    // The code is spent: a second tap is told so.
    EXPECT_NE(handle(message("/start " + code)).text.find("expired"), std::string::npos);
    EXPECT_NE(handle(message("/start")).text.find("Connect Telegram"), std::string::npos);
}

TEST_F(LinkerTest, SharingTheMatchingNumberVerifiesThePhone) {
    const auto code = create_link_code(db_, 1, std::chrono::minutes(10), kNow).value();
    (void)handle(message("/start " + code));

    Update someone_else = message("");
    someone_else.contact_phone = "+919876543210";
    someone_else.contact_user_id = 7;  // a contact from the address book, not the sender
    EXPECT_EQ(handle(someone_else).keyboard, OutgoingMessage::Keyboard::ShareContact);
    EXPECT_EQ(text("SELECT phone_verified_at FROM users"), "NULL");

    Update wrong = message("");
    wrong.contact_phone = "+919999999999";
    wrong.contact_user_id = 42;
    EXPECT_NE(handle(wrong).text.find("doesn't match"), std::string::npos);
    EXPECT_EQ(text("SELECT phone_verified_at FROM users"), "NULL");

    Update own = message("");
    own.contact_phone = "919876543210";
    own.contact_user_id = 42;
    EXPECT_EQ(handle(own).text, "Phone number verified.");
    EXPECT_EQ(text("SELECT phone_verified_at FROM users"), std::to_string(kNow));
}

TEST_F(LinkerTest, StopTurnsAlertsOffAndGroupsAreIgnored) {
    const auto code = create_link_code(db_, 1, std::chrono::minutes(10), kNow).value();
    (void)handle(message("/start " + code));
    (void)handle(message("/stop"));
    EXPECT_EQ(text("SELECT enabled FROM user_channels"), "0");

    Update group = message("/stop", "-100");
    group.private_chat = false;
    EXPECT_FALSE(linker_.handle(group, kNow).value().has_value());
}

// ---------------------------------------------------------------- fake Bot API over HTTPS

struct Call {
    std::string target;
    json::object body;
};

using Handler = std::function<std::pair<http::status, std::string>(const std::string& method,
                                                                   const json::object&)>;

asio::awaitable<void> serve_api(tcp::acceptor& acceptor, asio::ssl::context& tls, Handler handler,
                                std::vector<Call>& calls) {
    while (true) {
        boost::system::error_code ec;
        const auto token = asio::redirect_error(asio::use_awaitable, ec);
        tcp::socket socket = co_await acceptor.async_accept(token);
        if (ec) {
            co_return;
        }
        beast::ssl_stream<tcp::socket> stream(std::move(socket), tls);
        co_await stream.async_handshake(asio::ssl::stream_base::server, token);
        if (ec) {
            continue;
        }
        beast::flat_buffer buffer;
        http::request<http::string_body> request;
        co_await http::async_read(stream, buffer, request, token);
        if (ec) {
            continue;
        }
        const std::string target(request.target());
        const json::object body = json::parse(request.body()).as_object();
        calls.push_back({target, body});
        const auto [status, reply] = handler(target.substr(target.rfind('/') + 1), body);
        http::response<http::string_body> response{status, 11};
        response.set(http::field::content_type, "application/json");
        response.body() = reply;
        response.prepare_payload();
        response.keep_alive(false);
        co_await http::async_write(stream, response, token);
        co_await stream.async_shutdown(token);
    }
}

class TelegramApiTest : public LinkerTest {
protected:
    TelegramApiTest() {
        test::serve_with(server_tls_, cert_);
        test::trust(client_tls_, cert_);
    }

    template <typename T>
    T run(Handler handler, std::function<asio::awaitable<T>(TelegramApi&)> work) {
        asio::io_context io;
        tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
        asio::co_spawn(io, serve_api(acceptor, server_tls_, std::move(handler), calls_),
                       asio::detached);
        TelegramApi api({.token = "123:secret",
                         .host = "localhost",
                         .port = std::to_string(acceptor.local_endpoint().port()),
                         .timeout = std::chrono::seconds(2),
                         .poll_timeout = std::chrono::seconds(0),
                         .per_chat_gap = std::chrono::milliseconds(300),
                         .global_gap = std::chrono::milliseconds(0)},
                        client_tls_);
        auto result = asio::co_spawn(
            io,
            [&]() -> asio::awaitable<T> {
                T value = co_await work(api);
                acceptor.close();
                co_return value;
            },
            asio::use_future);
        io.run_for(std::chrono::seconds(10));
        return result.get();
    }

    static std::pair<http::status, std::string> ok(std::string result = "true") {
        return {http::status::ok, R"({"ok":true,"result":)" + result + "}"};
    }

    test::Certificate cert_ = test::make_localhost_certificate();
    asio::ssl::context server_tls_{asio::ssl::context::tls_server};
    asio::ssl::context client_tls_{asio::ssl::context::tls_client};
    std::vector<Call> calls_;
};

Delivery delivery(std::string chat) {
    return {.id = 1,
            .channel = "telegram",
            .address = std::move(chat),
            .type = "system.login_reminder",
            .message = {.subject = "Kite login needed", .text = "Run mda-login."}};
}

TEST_F(TelegramApiTest, SendsAndPacesMessagesPerChat) {
    std::vector<SendResult> results;
    const auto elapsed = run<std::chrono::steady_clock::duration>(
        [](const std::string&, const json::object&) { return ok(); },
        [&](TelegramApi& api) -> asio::awaitable<std::chrono::steady_clock::duration> {
            TelegramChannel channel(api);
            const auto started = std::chrono::steady_clock::now();
            results.push_back(co_await channel.send(delivery("555")));
            results.push_back(co_await channel.send(delivery("555")));
            co_return std::chrono::steady_clock::now() - started;
        });
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].outcome, SendOutcome::Sent) << results[0].error;
    EXPECT_EQ(results[1].outcome, SendOutcome::Sent);
    EXPECT_GE(elapsed, std::chrono::milliseconds(300));  // one chat: spaced by the per-chat gap
    ASSERT_EQ(calls_.size(), 2u);
    EXPECT_EQ(calls_[0].target, "/bot123:secret/sendMessage");
    EXPECT_EQ(calls_[0].body.at("chat_id").as_string(), "555");
    EXPECT_EQ(calls_[0].body.at("text").as_string(), "Kite login needed\n\nRun mda-login.");
}

TEST_F(TelegramApiTest, BlockedBotsFailAndRateLimitsRetry) {
    const auto send_with = [&](http::status status, std::string reply) {
        return run<SendResult>(
            [=](const std::string&, const json::object&) { return std::pair{status, reply}; },
            [](TelegramApi& api) -> asio::awaitable<SendResult> {
                TelegramChannel channel(api);
                co_return co_await channel.send(delivery("555"));
            });
    };
    const auto blocked = send_with(
        http::status::forbidden,
        R"({"ok":false,"error_code":403,"description":"Forbidden: bot was blocked by the user"})");
    EXPECT_EQ(blocked.outcome, SendOutcome::Fail);
    EXPECT_EQ(blocked.error.find("secret"), std::string::npos);  // the token never shows
    const auto limited = send_with(
        http::status::too_many_requests,
        R"({"ok":false,"error_code":429,"description":"Too Many Requests: retry after 3"})");
    EXPECT_EQ(limited.outcome, SendOutcome::Retry);
    EXPECT_EQ(send_with(http::status::bad_gateway, "<html>").outcome, SendOutcome::Retry);
}

TEST_F(TelegramApiTest, APollLinksTheChatAndAnswers) {
    const auto code = create_link_code(db_, 1, std::chrono::minutes(10), kNow).value();
    const auto offset = run<std::int64_t>(
        [&](const std::string& method, const json::object&) {
            if (method == "getUpdates") {
                return ok(R"([{"update_id": 70, "message": {"chat": {"id": 555, "type": "private"},
                               "from": {"id": 42}, "text": "/start )" +
                          code + R"("}}])");
            }
            return ok(R"({"message_id": 1})");
        },
        [&](TelegramApi& api) -> asio::awaitable<std::int64_t> {
            co_return co_await poll_once(api, linker_, 0, kNow);
        });
    EXPECT_EQ(offset, 71);
    ASSERT_EQ(calls_.size(), 2u);
    EXPECT_EQ(calls_[0].body.at("offset").as_int64(), 0);
    EXPECT_EQ(calls_[1].target, "/bot123:secret/sendMessage");
    EXPECT_TRUE(calls_[1].body.at("reply_markup").as_object().contains("keyboard"));
    EXPECT_EQ(text("SELECT address FROM user_channels"), "555");
}

}  // namespace
}  // namespace mda::notify

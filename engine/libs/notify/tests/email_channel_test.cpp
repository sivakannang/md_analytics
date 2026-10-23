// EmailChannel against a fake SMTP server on 127.0.0.1 with implicit TLS. The server's
// certificate is a throwaway self-signed one for "localhost", made in memory per test.

#include <future>
#include <memory>
#include <string>
#include <vector>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/write.hpp>
#include <gtest/gtest.h>
#include <openssl/evp.h>

#include <mda/notify/email_channel.hpp>

#include "test_tls.hpp"

namespace mda::notify {
namespace {

namespace asio = boost::asio;
using asio::ip::tcp;

std::string unbase64(std::string_view text) {
    std::string out(text.size() * 3 / 4 + 3, '\0');
    const int length = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                       reinterpret_cast<const unsigned char*>(text.data()),
                                       static_cast<int>(text.size()));
    out.resize(static_cast<std::size_t>(std::max(length, 0)));
    while (!text.empty() && text.back() == '=') {  // DecodeBlock keeps padding bytes
        text.remove_suffix(1);
        out.pop_back();
    }
    return out;
}

/// Reply codes the fake server gives at each stage.
struct Script {
    int greeting = 220;
    int ehlo = 250;
    int auth = 235;
    int mail = 250;
    int rcpt = 250;
    int data = 354;
    int end = 250;
    bool silent = false;  ///< accept and handshake, then say nothing
};

struct Received {
    std::vector<std::string> commands;
    std::string message;
};

asio::awaitable<void> serve(tcp::acceptor& acceptor, asio::ssl::context& tls, Script script,
                            Received& got) {
    boost::system::error_code ec;
    const auto token = asio::redirect_error(asio::use_awaitable, ec);
    tcp::socket socket = co_await acceptor.async_accept(token);
    if (ec) {
        co_return;
    }
    asio::ssl::stream<tcp::socket> stream(std::move(socket), tls);
    co_await stream.async_handshake(asio::ssl::stream_base::server, token);
    if (ec) {
        co_return;
    }
    const auto say = [&](std::string line) -> asio::awaitable<void> {
        co_await asio::async_write(stream, asio::buffer(line), token);
    };
    if (script.silent) {
        asio::steady_timer wait(co_await asio::this_coro::executor, std::chrono::seconds(3));
        co_await wait.async_wait(token);
        co_return;
    }
    co_await say(std::to_string(script.greeting) + " localhost ESMTP fake\r\n");
    if (script.greeting != 220) {
        co_return;
    }
    std::string input;
    while (true) {
        const std::size_t length =
            co_await asio::async_read_until(stream, asio::dynamic_buffer(input), "\r\n", token);
        if (ec) {
            co_return;
        }
        const std::string line = input.substr(0, length - 2);
        input.erase(0, length);
        got.commands.push_back(line);
        if (line.starts_with("EHLO")) {
            co_await say(std::to_string(script.ehlo) + "-localhost\r\n" +
                         std::to_string(script.ehlo) + " AUTH PLAIN\r\n");
        } else if (line.starts_with("AUTH")) {
            co_await say(std::to_string(script.auth) + " auth\r\n");
        } else if (line.starts_with("MAIL")) {
            co_await say(std::to_string(script.mail) + " ok\r\n");
        } else if (line.starts_with("RCPT")) {
            co_await say(std::to_string(script.rcpt) + " recipient <someone@example.com>\r\n");
        } else if (line == "DATA") {
            co_await say(std::to_string(script.data) + " go ahead\r\n");
            if (script.data != 354) {
                continue;
            }
            const std::size_t end = co_await asio::async_read_until(
                stream, asio::dynamic_buffer(input), "\r\n.\r\n", token);
            if (ec) {
                co_return;
            }
            got.message = input.substr(0, end - 3);
            input.erase(0, end);
            co_await say(std::to_string(script.end) + " queued\r\n");
        } else if (line == "QUIT") {
            co_await say("221 bye\r\n");
            co_return;
        } else {
            co_await say("500 what\r\n");
        }
    }
}

class EmailChannelTest : public ::testing::Test {
protected:
    EmailChannelTest() {
        test::serve_with(server_tls_, cert_);
        test::trust(client_tls_, cert_);
    }

    SendResult run(Script script, std::string address = "asha@example.com", bool trusted = true) {
        asio::io_context io;
        tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
        asio::co_spawn(io, serve(acceptor, server_tls_, script, received_), asio::detached);
        SmtpOptions options{.host = "localhost",
                            .port = std::to_string(acceptor.local_endpoint().port()),
                            .username = "bot@example.com",
                            .password = "app-password",
                            .from_address = "bot@example.com",
                            .timeout = std::chrono::seconds(1)};
        asio::ssl::context untrusted(asio::ssl::context::tls_client);
        untrusted.set_verify_mode(asio::ssl::verify_peer);
        EmailChannel channel(options, trusted ? client_tls_ : untrusted);
        Delivery delivery{.id = 7,
                          .notification_id = 3,
                          .channel = "email",
                          .address = std::move(address),
                          .type = "account.signup",
                          .message = {.subject = "Welcome to md_analytics",
                                      .text = "Hi asha,\n\nReady.\n\n— md_analytics"}};
        auto result = asio::co_spawn(
            io,
            [&]() -> asio::awaitable<SendResult> {
                const auto started = std::chrono::steady_clock::now();
                const SendResult r = co_await channel.send(delivery);
                send_time_ = std::chrono::steady_clock::now() - started;
                co_return r;
            },
            asio::use_future);
        io.run_for(std::chrono::seconds(10));
        return result.get();
    }

    test::Certificate cert_ = test::make_localhost_certificate();
    asio::ssl::context server_tls_{asio::ssl::context::tls_server};
    asio::ssl::context client_tls_{asio::ssl::context::tls_client};
    Received received_;
    std::chrono::steady_clock::duration send_time_{};
};

TEST_F(EmailChannelTest, DeliversAMailOverTls) {
    const auto result = run({});
    EXPECT_EQ(result.outcome, SendOutcome::Sent) << result.error;
    ASSERT_GE(received_.commands.size(), 6u);
    EXPECT_EQ(received_.commands[0], "EHLO md-analytics");
    EXPECT_EQ(unbase64(received_.commands[1].substr(std::string("AUTH PLAIN ").size())),
              std::string("\0bot@example.com\0app-password", 29));
    EXPECT_EQ(received_.commands[2], "MAIL FROM:<bot@example.com>");
    EXPECT_EQ(received_.commands[3], "RCPT TO:<asha@example.com>");
    EXPECT_EQ(received_.commands[4], "DATA");
    EXPECT_EQ(received_.commands.back(), "QUIT");

    const std::string& m = received_.message;
    EXPECT_NE(m.find("To: <asha@example.com>\r\n"), std::string::npos);
    const auto subject = m.find("Subject: =?UTF-8?B?");
    ASSERT_NE(subject, std::string::npos);
    const auto subject_end = m.find("?=", subject);
    EXPECT_EQ(unbase64(m.substr(subject + 19, subject_end - subject - 19)),
              "Welcome to md_analytics");
    const auto body = m.find("\r\n\r\n");
    std::string encoded;
    for (const char c : m.substr(body + 4)) {
        if (c != '\r' && c != '\n') {
            encoded += c;
        }
    }
    EXPECT_EQ(unbase64(encoded), "Hi asha,\r\n\r\nReady.\r\n\r\n— md_analytics");
}

TEST_F(EmailChannelTest, AnUnknownRecipientFailsWithoutLeakingTheAddress) {
    const auto result = run({.rcpt = 550});
    EXPECT_EQ(result.outcome, SendOutcome::Fail);
    EXPECT_EQ(result.error, "smtp RCPT TO: reply 550");
}

TEST_F(EmailChannelTest, BusyServersAndRejectedLoginsAreRetried) {
    EXPECT_EQ(run({.greeting = 421}).outcome, SendOutcome::Retry);
    const auto login = run({.auth = 535});
    EXPECT_EQ(login.outcome, SendOutcome::Retry);
    EXPECT_EQ(login.error, "smtp AUTH: reply 535");
}

TEST_F(EmailChannelTest, AnUntrustedCertificateIsNeverSentTo) {
    const auto result = run({}, "asha@example.com", false);
    EXPECT_EQ(result.outcome, SendOutcome::Retry);
    EXPECT_TRUE(received_.commands.empty());
}

TEST_F(EmailChannelTest, ASilentServerTimesOut) {
    const auto result = run({.silent = true});
    EXPECT_EQ(result.outcome, SendOutcome::Retry);
    EXPECT_LT(send_time_, std::chrono::seconds(2));  // the 1 s timeout, not the server's 3 s
}

TEST_F(EmailChannelTest, ABadAddressFailsWithoutConnecting) {
    EXPECT_EQ(run({}, "asha@example.com\r\nBcc: x@y.z").outcome, SendOutcome::Fail);
    EXPECT_FALSE(plausible_email("no-at-sign"));
    EXPECT_FALSE(plausible_email("a@b@c"));
    EXPECT_TRUE(plausible_email("first.last+tag@example.co.in"));
}

TEST(FormatEmailTest, HeadersUseIstAndCrlf) {
    const SmtpOptions options{.from_address = "bot@example.com"};
    const auto text = format_email(options, "asha@example.com", {.subject = "Hi", .text = "a\nb"},
                                   1'792'690'000, "1.1@md-analytics");
    EXPECT_NE(text.find("From: md_analytics <bot@example.com>\r\n"), std::string::npos);
    EXPECT_NE(text.find("Date: Thu, 22 Oct 2026 22:56:40 +0530\r\n"), std::string::npos);
    EXPECT_NE(text.find("Content-Transfer-Encoding: base64\r\n\r\n"), std::string::npos);
}

}  // namespace
}  // namespace mda::notify

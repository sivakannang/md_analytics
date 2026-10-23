#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include <boost/asio/read_until.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <openssl/evp.h>
#include <openssl/ssl.h>

#include <mda/notify/email_channel.hpp>

namespace mda::notify {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;

constexpr std::int64_t kIst = 5 * 3600 + 30 * 60;

std::string base64(std::string_view data) {
    std::string out(4 * ((data.size() + 2) / 3), '\0');
    const int length = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                       reinterpret_cast<const unsigned char*>(data.data()),
                                       static_cast<int>(data.size()));
    out.resize(static_cast<std::size_t>(length));
    return out;
}

/// "Thu, 22 Oct 2026 23:30:00 +0530".
std::string rfc5322_date(std::int64_t now) {
    static constexpr const char* kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static constexpr const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const std::chrono::sys_seconds local{std::chrono::seconds{now + kIst}};
    const auto day = std::chrono::floor<std::chrono::days>(local);
    const std::chrono::year_month_day ymd{day};
    const std::chrono::weekday weekday{day};
    const std::chrono::hh_mm_ss hms{local - day};
    char text[48];
    std::snprintf(text, sizeof text, "%s, %02u %s %04d %02ld:%02ld:%02ld +0530", kDays[weekday.c_encoding()],
                  static_cast<unsigned>(ymd.day()), kMonths[static_cast<unsigned>(ymd.month()) - 1],
                  static_cast<int>(ymd.year()), static_cast<long>(hms.hours().count()),
                  static_cast<long>(hms.minutes().count()), static_cast<long>(hms.seconds().count()));
    return text;
}

struct Reply {
    int code = 0;
};

enum class Stage { Connect, Greeting, Ehlo, Auth, MailFrom, RcptTo, Data, Body };

const char* to_string(Stage stage) {
    switch (stage) {
        case Stage::Connect:
            return "connect";
        case Stage::Greeting:
            return "greeting";
        case Stage::Ehlo:
            return "EHLO";
        case Stage::Auth:
            return "AUTH";
        case Stage::MailFrom:
            return "MAIL FROM";
        case Stage::RcptTo:
            return "RCPT TO";
        case Stage::Data:
            return "DATA";
        case Stage::Body:
            return "message";
    }
    return "?";
}

SendResult unexpected_reply(Stage stage, int code) {
    // A rejected login is a configuration error: keep the mail and retry once it's fixed.
    const bool permanent = code >= 500 && stage != Stage::Auth;
    return {.outcome = permanent ? SendOutcome::Fail : SendOutcome::Retry,
            .error = std::string("smtp ") + to_string(stage) + ": reply " + std::to_string(code)};
}

}  // namespace

bool plausible_email(std::string_view address) {
    const auto at = address.find('@');
    if (at == std::string_view::npos || at == 0 || at + 1 >= address.size() ||
        address.find('@', at + 1) != std::string_view::npos || address.size() > 254) {
        return false;
    }
    for (const char c : address) {
        if (static_cast<unsigned char>(c) <= ' ' || c == '<' || c == '>' || c == ',' || c == '"' || c == 0x7f) {
            return false;
        }
    }
    return true;
}

std::string format_email(const SmtpOptions& options, std::string_view to, const Message& message, std::int64_t now,
                         std::string_view message_id) {
    std::string out;
    out += "From: " + options.from_name + " <" + options.from_address + ">\r\n";
    out += "To: <" + std::string(to) + ">\r\n";
    out += "Subject: =?UTF-8?B?" + base64(message.subject) + "?=\r\n";
    out += "Date: " + rfc5322_date(now) + "\r\n";
    out += "Message-ID: <" + std::string(message_id) + ">\r\n";
    out += "MIME-Version: 1.0\r\n";
    out += "Content-Type: text/plain; charset=UTF-8\r\n";
    out += "Content-Transfer-Encoding: base64\r\n\r\n";
    // CRLF line ends in the text, then base64 in 76-character lines.
    std::string text;
    for (const char c : message.text) {
        if (c == '\n') {
            text += "\r\n";
        } else if (c != '\r') {
            text += c;
        }
    }
    const std::string encoded = base64(text);
    for (std::size_t at = 0; at < encoded.size(); at += 76) {
        out += encoded.substr(at, 76) + "\r\n";
    }
    return out;
}

asio::awaitable<SendResult> EmailChannel::send(const Delivery& delivery) {
    if (!plausible_email(delivery.address)) {
        co_return SendResult{.outcome = SendOutcome::Fail, .error = "invalid email address"};
    }
    const auto executor = co_await asio::this_coro::executor;
    boost::system::error_code ec;
    const auto token = asio::redirect_error(asio::use_awaitable, ec);
    const auto network = [&](Stage stage) {
        return SendResult{.outcome = SendOutcome::Retry,
                          .error = std::string("smtp ") + to_string(stage) + ": " + ec.message()};
    };

    asio::ip::tcp::resolver resolver(executor);
    const auto endpoints = co_await resolver.async_resolve(options_.host, options_.port, token);
    if (ec) {
        co_return network(Stage::Connect);
    }
    beast::ssl_stream<beast::tcp_stream> stream(executor, *tls_);
    if (SSL_set_tlsext_host_name(stream.native_handle(), options_.host.c_str()) != 1) {
        co_return SendResult{.outcome = SendOutcome::Retry, .error = "smtp connect: could not set SNI"};
    }
    stream.set_verify_callback(asio::ssl::host_name_verification(options_.host));
    auto& tcp = beast::get_lowest_layer(stream);
    tcp.expires_after(options_.timeout);
    co_await tcp.async_connect(endpoints, token);
    if (ec) {
        co_return network(Stage::Connect);
    }
    tcp.expires_after(options_.timeout);
    co_await stream.async_handshake(asio::ssl::stream_base::client, token);
    if (ec) {
        co_return network(Stage::Connect);
    }

    std::string input;
    // One reply: lines "250-…" continue, "250 …" ends it.
    const auto read_reply = [&]() -> asio::awaitable<Reply> {
        while (true) {
            tcp.expires_after(options_.timeout);
            const std::size_t length = co_await asio::async_read_until(stream, asio::dynamic_buffer(input), "\r\n", token);
            if (ec) {
                co_return Reply{};
            }
            const std::string line = input.substr(0, length - 2);
            input.erase(0, length);
            if (line.size() < 3) {
                co_return Reply{};
            }
            const int code = std::atoi(line.substr(0, 3).c_str());
            if (line.size() == 3 || line[3] != '-') {
                co_return Reply{.code = code};
            }
        }
    };
    const auto command = [&](std::string line) -> asio::awaitable<Reply> {
        line += "\r\n";
        tcp.expires_after(options_.timeout);
        co_await asio::async_write(stream, asio::buffer(line), token);
        if (ec) {
            co_return Reply{};
        }
        co_return co_await read_reply();
    };

    struct Step {
        Stage stage;
        std::string line;  // empty: just read (the greeting)
        int expect;
    };
    const std::string auth = std::string("\0", 1) + options_.username + std::string("\0", 1) + options_.password;
    const std::vector<Step> steps{
        {Stage::Greeting, "", 220},
        {Stage::Ehlo, "EHLO md-analytics", 250},
        {Stage::Auth, "AUTH PLAIN " + base64(auth), 235},
        {Stage::MailFrom, "MAIL FROM:<" + options_.from_address + ">", 250},
        {Stage::RcptTo, "RCPT TO:<" + delivery.address + ">", 250},
        {Stage::Data, "DATA", 354},
    };
    for (const Step& step : steps) {
        const Reply reply = step.line.empty() ? co_await read_reply() : co_await command(step.line);
        if (ec) {
            co_return network(step.stage);
        }
        if (reply.code != step.expect && !(step.stage == Stage::RcptTo && reply.code == 251)) {
            co_return unexpected_reply(step.stage, reply.code);
        }
    }

    const std::string id = std::to_string(delivery.id) + "." + std::to_string(delivery.notification_id) + "@md-analytics";
    std::string body = format_email(options_, delivery.address, delivery.message,
                                    std::chrono::duration_cast<std::chrono::seconds>(
                                        std::chrono::system_clock::now().time_since_epoch())
                                        .count(),
                                    id);
    // The body is base64 and the headers are ours, so no line starts with "."; the end mark:
    body += ".\r\n";
    tcp.expires_after(options_.timeout);
    co_await asio::async_write(stream, asio::buffer(body), token);
    if (ec) {
        co_return network(Stage::Body);
    }
    const Reply accepted = co_await read_reply();
    if (ec) {
        co_return network(Stage::Body);
    }
    if (accepted.code != 250) {
        co_return unexpected_reply(Stage::Body, accepted.code);
    }
    (void)co_await command("QUIT");  // the mail is accepted; how the server says goodbye doesn't matter
    co_return SendResult{.outcome = SendOutcome::Sent};
}

}  // namespace mda::notify

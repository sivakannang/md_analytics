#pragma once

// Email over SMTP (docs/design/notifications.md §3): our own client on Asio + OpenSSL.
// Implicit TLS (port 465), certificate checked against the host name, AUTH PLAIN, one
// message per connection (a few mails a day). The body is plain UTF-8 text, base64-encoded.
//
// Outcomes: network trouble, 4xx replies and a rejected login (the password can be fixed
// before the retries run out) → Retry; a 5xx reply otherwise, e.g. an unknown recipient →
// Fail. Errors carry the stage and reply code only: server text may echo the address.

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include <boost/asio/ssl/context.hpp>

#include <mda/notify/channel.hpp>

namespace mda::notify {

struct SmtpOptions {
    std::string host;          ///< e.g. smtp.gmail.com
    std::string port = "465";  ///< implicit TLS
    std::string username;
    std::string password;  ///< never logged
    std::string from_address;
    std::string from_name = "md_analytics";
    std::chrono::seconds timeout{30};  ///< per SMTP step
};

/// The RFC 5322 message: headers (subject as a UTF-8 encoded word) and a base64 body, with
/// CRLF line ends. `now` is Unix seconds (the Date header is in IST).
[[nodiscard]] std::string format_email(const SmtpOptions& options, std::string_view to,
                                       const Message& message, std::int64_t now,
                                       std::string_view message_id);

/// An address we are willing to put in an envelope: one @, no spaces, brackets or line breaks.
[[nodiscard]] bool plausible_email(std::string_view address);

class EmailChannel : public Channel {
public:
    EmailChannel(SmtpOptions options, boost::asio::ssl::context& tls)
        : options_(std::move(options)), tls_(&tls) {}

    [[nodiscard]] std::string_view name() const override { return "email"; }
    boost::asio::awaitable<SendResult> send(const Delivery& delivery) override;

private:
    SmtpOptions options_;
    boost::asio::ssl::context* tls_;
};

}  // namespace mda::notify

#pragma once

// A delivery channel (docs/design/notifications.md §3): email, Telegram, later WhatsApp,
// SMS or push. The notifier hands it one rendered message for one recipient; the channel
// says whether it was sent, should be retried, or can never be sent.

#include <cstdint>
#include <string>
#include <string_view>

#include <boost/asio/awaitable.hpp>

namespace mda::notify {

/// A message rendered from a notification's type and payload (templates.hpp).
struct Message {
    std::string subject;  ///< email subject; Telegram shows it as the first line
    std::string text;     ///< plain text body
};

/// One message to one recipient on one channel.
struct Delivery {
    std::int64_t id = 0;
    std::int64_t notification_id = 0;
    std::string channel;
    std::string address;  ///< email address, Telegram chat id, … (never logged)
    std::string type;     ///< notification type, e.g. account.signup
    Message message;
    int attempts = 0;  ///< attempts made before this one
};

enum class SendOutcome {
    Sent,
    Retry,  ///< a temporary failure (network, rate limit, server busy)
    Fail,   ///< a permanent one (bad address, blocked bot): no more attempts
};

struct SendResult {
    SendOutcome outcome = SendOutcome::Sent;
    std::string error;  ///< for the outbox's last_error; must not contain the address
};

class Channel {
public:
    virtual ~Channel() = default;
    [[nodiscard]] virtual std::string_view name() const = 0;
    virtual boost::asio::awaitable<SendResult> send(const Delivery& delivery) = 0;
};

}  // namespace mda::notify

#pragma once

// Message text per notification type (docs/design/notifications.md §5). Payloads are JSON
// objects written by the producers; missing fields render as empty.

#include <string_view>

#include <mda/notify/channel.hpp>

namespace mda::notify {

/// The message for `type`; an unknown type renders its type and raw payload.
[[nodiscard]] Message render(std::string_view type, std::string_view payload);

}  // namespace mda::notify

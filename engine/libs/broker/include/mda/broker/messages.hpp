#pragma once

// Order entry on the buses (ADR-0029, docs/design/paper-trading.md §10):
// - OrderRequest (10): a source (the gateway on ord-gw, the auto trader on ord-auto) asks
//   mda-broker to place, modify or cancel an order, to switch an account's kill switch, or
//   to convert a position between MIS and NRML;
// - ExecutionReport (11): mda-broker reports every change on exec.
// Requests are idempotent by (user, client order id): a repeat gets the order's current
// report. paper.db is the truth; the bus is the fast path.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include <mda/core/price.hpp>
#include <mda/messages/message.hpp>
#include <mda/oms/types.hpp>

namespace mda::broker {

enum class Action : std::uint8_t {
    New = 1,
    Modify = 2,  ///< order_id: quantity, price, trigger
    Cancel = 3,  ///< order_id
    KillSwitchOn = 4,
    KillSwitchOff = 5,
    Convert = 6,  ///< header.instrument_id, quantity: move that much of the position into `product`
};

/// header.instrument_id = the instrument (New); header.sequence = the source's sequence. 96 bytes.
struct OrderRequest {
    messages::EventHeader header;
    std::int64_t at = 0;                ///< when the source sent it (Unix ms)
    std::int64_t user = 0;              ///< app.db user id
    std::uint64_t client_order_id = 0;  ///< unique per user and source
    std::int64_t order_id = 0;          ///< Modify / Cancel: the broker's order id
    core::Price price;
    core::Price trigger;
    std::int64_t quantity = 0;
    Action action = Action::New;
    oms::Side side = oms::Side::Buy;
    oms::OrderType type = oms::OrderType::Market;
    oms::Product product = oms::Product::Mis;
    oms::Source source = oms::Source::Ui;
    std::uint8_t reserved[3] = {};
    char tag[16] = {};
};
static_assert(sizeof(OrderRequest) == 96);
static_assert(offsetof(OrderRequest, price) == 48);
static_assert(offsetof(OrderRequest, action) == 72);
static_assert(offsetof(OrderRequest, tag) == 80);

enum class ReportEvent : std::uint8_t {
    Accepted = 1,
    Rejected = 2,
    Triggered = 3,
    Fill = 4,
    Cancelled = 5,
    Modified = 6,
    Status = 7,         ///< the current state, sent again for a repeated request
    KillSwitchOn = 8,   ///< order_id 0
    KillSwitchOff = 9,  ///< order_id 0
    Settled = 10,       ///< a position settled at expiry: order_id 0, last_quantity, last_price = FSP
    Converted = 11,     ///< a position converted: order_id 0, product = the new one, side = the
                        ///< position's (Buy long), last_quantity, last_price = its average
};

/// header.instrument_id = the order's instrument; header.sequence = the broker's. 216 bytes.
struct ExecutionReport {
    messages::EventHeader header;
    std::int64_t at = 0;  ///< Unix ms
    std::int64_t user = 0;
    std::uint64_t client_order_id = 0;
    std::int64_t order_id = 0;
    std::int64_t quantity = 0;       ///< the order's quantity
    std::int64_t filled = 0;
    std::int64_t last_quantity = 0;  ///< Fill: this fill
    core::Price last_price;          ///< Fill: this fill
    core::Price average_price;
    core::Price price;
    core::Price trigger;
    oms::Paise charges = 0;          ///< Fill: this fill's charges
    ReportEvent event = ReportEvent::Accepted;
    oms::OrderStatus status = oms::OrderStatus::Open;
    oms::Side side = oms::Side::Buy;
    oms::OrderType type = oms::OrderType::Market;  ///< now (an SL becomes LIMIT once triggered)
    oms::Product product = oms::Product::Mis;
    oms::Source source = oms::Source::Ui;
    std::uint8_t reject_code = 0;    ///< rms::RejectCode; 0 = none
    std::uint8_t reserved = 0;
    char reason[96] = {};  ///< UTF-8 (₹ takes 3 bytes); risk messages fit
};
static_assert(sizeof(ExecutionReport) == 216);
static_assert(offsetof(ExecutionReport, charges) == 104);
static_assert(offsetof(ExecutionReport, event) == 112);
static_assert(offsetof(ExecutionReport, reason) == 120);

/// Copies `text` into a fixed field, cut to fit and always NUL-terminated.
template <std::size_t N>
void set_text(char (&field)[N], std::string_view text) noexcept {
    const std::size_t n = text.size() < N - 1 ? text.size() : N - 1;
    for (std::size_t i = 0; i < n; ++i) {
        field[i] = text[i];
    }
    for (std::size_t i = n; i < N; ++i) {
        field[i] = '\0';
    }
}

template <std::size_t N>
[[nodiscard]] std::string_view text_of(const char (&field)[N]) noexcept {
    std::size_t n = 0;
    while (n < N && field[n] != '\0') {
        ++n;
    }
    return {field, n};
}

}  // namespace mda::broker

namespace mda::messages {

template <>
struct MessageTraits<broker::OrderRequest> {
    static constexpr MessageType type = MessageType::OrderRequest;
    static constexpr std::uint16_t version = 1;
};
template <>
struct MessageTraits<broker::ExecutionReport> {
    static constexpr MessageType type = MessageType::ExecutionReport;
    static constexpr std::uint16_t version = 1;
};
static_assert(Message<broker::OrderRequest>);
static_assert(Message<broker::ExecutionReport>);

}  // namespace mda::messages

#pragma once

// One paper order and its state machine (docs/design/paper-trading.md §4). Validation of
// lots, ticks and limits happens before (rms, PT2); this enforces only the transitions:
//
//   accepted → OPEN (or TRIGGER_PENDING for SL / SL-M) → OPEN (partial) → COMPLETE
//   OPEN / TRIGGER_PENDING → CANCELLED;  TRIGGER_PENDING → OPEN on its trigger
//   rejected → REJECTED (never anything else)

#include <cstdint>
#include <expected>
#include <string>

#include <mda/core/instrument_id.hpp>
#include <mda/core/price.hpp>
#include <mda/oms/types.hpp>

namespace mda::oms {

/// What a source asks for.
struct OrderSpec {
    std::int64_t account = 0;
    std::uint64_t client_order_id = 0;  ///< unique per account; repeats are ignored
    Source source = Source::Ui;
    core::InstrumentId instrument;
    Side side = Side::Buy;
    OrderType type = OrderType::Market;
    Product product = Product::Mis;
    std::int64_t quantity = 0;  ///< units, a multiple of the lot size
    core::Price price;          ///< LIMIT and SL
    core::Price trigger;        ///< SL and SL-M
    std::string tag;
};

enum class OrderError : std::uint8_t {
    NotLive = 1,      ///< the order is complete, cancelled or rejected
    NotOpen = 2,      ///< a fill on an order that isn't OPEN (still waiting for its trigger)
    NotTriggerable = 3,
    Overfill = 4,     ///< more than the remaining quantity
    BadQuantity = 5,  ///< zero or negative, or a modify below the filled quantity
};

[[nodiscard]] std::string_view name(OrderError error) noexcept;

class Order {
public:
    /// An accepted order: OPEN, or TRIGGER_PENDING for SL / SL-M.
    [[nodiscard]] static Order accepted(std::int64_t id, OrderSpec spec, std::int64_t at);
    /// A rejected order; `reason` is shown to the user.
    [[nodiscard]] static Order rejected(std::int64_t id, OrderSpec spec, std::string reason,
                                        std::int64_t at);

    /// Everything that defines an order, as stored (paper.db) to restore it after a restart.
    struct Saved {
        std::int64_t id = 0;
        OrderSpec spec;
        OrderStatus status = OrderStatus::Open;
        OrderType type = OrderType::Market;
        std::int64_t filled = 0;
        std::int64_t filled_value_raw = 0;
        Paise brokerage = 0;
        Paise charges = 0;
        std::string reason;
        std::int64_t created_at = 0;
        std::int64_t updated_at = 0;
    };
    [[nodiscard]] static Order restore(Saved saved);
    [[nodiscard]] Saved save() const;

    /// SL becomes LIMIT and SL-M becomes MARKET; TRIGGER_PENDING → OPEN.
    std::expected<void, OrderError> trigger(std::int64_t at);
    /// A fill of `quantity` at `price`; COMPLETE once nothing remains.
    std::expected<void, OrderError> fill(std::int64_t quantity, core::Price price, std::int64_t at);
    std::expected<void, OrderError> cancel(std::string reason, std::int64_t at);
    /// New quantity, price and trigger for a live order; the quantity can't go below what
    /// has filled (reaching it completes the order).
    std::expected<void, OrderError> modify(std::int64_t quantity, core::Price price,
                                           core::Price trigger, std::int64_t at);
    /// Charges booked against this order (brokerage is capped per executed order).
    void add_charges(Paise brokerage, Paise total) noexcept;

    [[nodiscard]] std::int64_t id() const noexcept { return id_; }
    [[nodiscard]] const OrderSpec& spec() const noexcept { return spec_; }
    [[nodiscard]] OrderStatus status() const noexcept { return status_; }
    /// The type now: an SL becomes a LIMIT once triggered.
    [[nodiscard]] OrderType type() const noexcept { return type_; }
    [[nodiscard]] std::int64_t filled() const noexcept { return filled_; }
    [[nodiscard]] std::int64_t remaining() const noexcept { return spec_.quantity - filled_; }
    /// Volume-weighted fill price; zero before the first fill.
    [[nodiscard]] core::Price average_price() const noexcept;
    [[nodiscard]] Paise brokerage() const noexcept { return brokerage_; }
    [[nodiscard]] Paise charges() const noexcept { return charges_; }
    [[nodiscard]] const std::string& reason() const noexcept { return reason_; }
    [[nodiscard]] std::int64_t created_at() const noexcept { return created_at_; }
    [[nodiscard]] std::int64_t updated_at() const noexcept { return updated_at_; }

private:
    Order(std::int64_t id, OrderSpec spec, OrderStatus status, std::int64_t at);

    std::int64_t id_ = 0;
    OrderSpec spec_;
    OrderStatus status_ = OrderStatus::Open;
    OrderType type_ = OrderType::Market;
    std::int64_t filled_ = 0;
    std::int64_t filled_value_raw_ = 0;  ///< Σ price.raw × quantity of the fills
    Paise brokerage_ = 0;
    Paise charges_ = 0;
    std::string reason_;
    std::int64_t created_at_ = 0;
    std::int64_t updated_at_ = 0;
};

}  // namespace mda::oms

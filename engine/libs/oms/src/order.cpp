#include <utility>

#include <mda/oms/order.hpp>

namespace mda::oms {

std::string_view name(OrderError error) noexcept {
    switch (error) {
        case OrderError::NotLive:
            return "the order is no longer open";
        case OrderError::NotOpen:
            return "the order is waiting for its trigger";
        case OrderError::NotTriggerable:
            return "the order has no trigger";
        case OrderError::Overfill:
            return "fill larger than the remaining quantity";
        case OrderError::BadQuantity:
            return "invalid quantity";
    }
    return "?";
}

Order::Order(std::int64_t id, OrderSpec spec, OrderStatus status, std::int64_t at)
    : id_(id)
    , spec_(std::move(spec))
    , status_(status)
    , type_(spec_.type)
    , created_at_(at)
    , updated_at_(at) {}

Order Order::accepted(std::int64_t id, OrderSpec spec, std::int64_t at) {
    const bool stop = spec.type == OrderType::StopLimit || spec.type == OrderType::StopMarket;
    return Order(id, std::move(spec), stop ? OrderStatus::TriggerPending : OrderStatus::Open, at);
}

Order Order::rejected(std::int64_t id, OrderSpec spec, std::string reason, std::int64_t at) {
    Order order(id, std::move(spec), OrderStatus::Rejected, at);
    order.reason_ = std::move(reason);
    return order;
}

Order Order::restore(Saved saved) {
    Order order(saved.id, std::move(saved.spec), saved.status, saved.created_at);
    order.type_ = saved.type;
    order.filled_ = saved.filled;
    order.filled_value_raw_ = saved.filled_value_raw;
    order.brokerage_ = saved.brokerage;
    order.charges_ = saved.charges;
    order.reason_ = std::move(saved.reason);
    order.updated_at_ = saved.updated_at;
    return order;
}

Order::Saved Order::save() const {
    return {.id = id_,
            .spec = spec_,
            .status = status_,
            .type = type_,
            .filled = filled_,
            .filled_value_raw = filled_value_raw_,
            .brokerage = brokerage_,
            .charges = charges_,
            .reason = reason_,
            .created_at = created_at_,
            .updated_at = updated_at_};
}

std::expected<void, OrderError> Order::trigger(std::int64_t at) {
    if (status_ != OrderStatus::TriggerPending) {
        return std::unexpected(is_final(status_) ? OrderError::NotLive
                                                 : OrderError::NotTriggerable);
    }
    type_ = type_ == OrderType::StopLimit ? OrderType::Limit : OrderType::Market;
    status_ = OrderStatus::Open;
    updated_at_ = at;
    return {};
}

std::expected<void, OrderError> Order::fill(std::int64_t quantity, core::Price price,
                                            std::int64_t at) {
    if (is_final(status_)) {
        return std::unexpected(OrderError::NotLive);
    }
    if (status_ != OrderStatus::Open) {
        return std::unexpected(OrderError::NotOpen);
    }
    if (quantity <= 0) {
        return std::unexpected(OrderError::BadQuantity);
    }
    if (quantity > remaining()) {
        return std::unexpected(OrderError::Overfill);
    }
    filled_ += quantity;
    filled_value_raw_ += price.raw() * quantity;
    if (remaining() == 0) {
        status_ = OrderStatus::Complete;
    }
    updated_at_ = at;
    return {};
}

std::expected<void, OrderError> Order::cancel(std::string reason, std::int64_t at) {
    if (is_final(status_)) {
        return std::unexpected(OrderError::NotLive);
    }
    status_ = OrderStatus::Cancelled;
    reason_ = std::move(reason);
    updated_at_ = at;
    return {};
}

std::expected<void, OrderError> Order::modify(std::int64_t quantity, core::Price price,
                                              core::Price trigger, std::int64_t at) {
    if (is_final(status_)) {
        return std::unexpected(OrderError::NotLive);
    }
    if (quantity <= 0 || quantity < filled_) {
        return std::unexpected(OrderError::BadQuantity);
    }
    spec_.quantity = quantity;
    spec_.price = price;
    spec_.trigger = trigger;
    if (remaining() == 0) {
        status_ = OrderStatus::Complete;
    }
    updated_at_ = at;
    return {};
}

void Order::add_charges(Paise brokerage, Paise total) noexcept {
    brokerage_ += brokerage;
    charges_ += total;
}

core::Price Order::average_price() const noexcept {
    if (filled_ == 0) {
        return {};
    }
    // Rounded to the nearest raw unit (1/10000 rupee), as an average can fall between ticks.
    const std::int64_t half = filled_ / 2;
    return core::Price::from_raw((filled_value_raw_ + half) / filled_);
}

}  // namespace mda::oms

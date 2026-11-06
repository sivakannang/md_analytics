#include <mda/oms/types.hpp>

namespace mda::oms {

namespace {
constexpr std::int64_t kRawPerPaisa = core::Price::kScale / 100;
}  // namespace

Paise raw_to_paise(std::int64_t raw) noexcept {
    const std::int64_t half = kRawPerPaisa / 2;
    return raw >= 0 ? (raw + half) / kRawPerPaisa : -((-raw + half) / kRawPerPaisa);
}

Paise value_paise(core::Price price, std::int64_t quantity) noexcept {
    return raw_to_paise(price.raw() * quantity);
}

std::string_view name(Side side) noexcept {
    return side == Side::Buy ? "BUY" : "SELL";
}

std::string_view name(OrderType type) noexcept {
    switch (type) {
        case OrderType::Market:
            return "MARKET";
        case OrderType::Limit:
            return "LIMIT";
        case OrderType::StopLimit:
            return "SL";
        case OrderType::StopMarket:
            return "SL-M";
    }
    return "?";
}

std::string_view name(Product product) noexcept {
    return product == Product::Mis ? "MIS" : "NRML";
}

std::string_view name(OrderStatus status) noexcept {
    switch (status) {
        case OrderStatus::TriggerPending:
            return "TRIGGER_PENDING";
        case OrderStatus::Open:
            return "OPEN";
        case OrderStatus::Complete:
            return "COMPLETE";
        case OrderStatus::Cancelled:
            return "CANCELLED";
        case OrderStatus::Rejected:
            return "REJECTED";
    }
    return "?";
}

}  // namespace mda::oms

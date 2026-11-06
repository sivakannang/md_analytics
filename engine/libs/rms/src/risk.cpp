#include <mda/rms/risk.hpp>

#include <algorithm>
#include <cstdlib>
#include <format>

namespace mda::rms {

namespace {

constexpr std::int64_t kBillion = 1'000'000'000;
constexpr std::int64_t kIstOffset = 19'800;
constexpr std::int64_t kDay = 86'400;

std::int64_t seconds_ist(Millis now) {
    return ((now / 1000 + kIstOffset) % kDay + kDay) % kDay;
}

std::int32_t ist_day(Millis now) {
    return static_cast<std::int32_t>((now / 1000 + kIstOffset) / kDay);
}

Paise of(Paise amount, Ppb rate) {
    return (amount * rate + kBillion / 2) / kBillion;
}

std::string money(Paise amount) {
    const Paise a = std::abs(amount);
    return std::format("{}₹{}.{:02}", amount < 0 ? "-" : "", a / 100, a % 100);
}

std::string price_text(core::Price price) {
    return std::format("{:.2f}", price.to_double());
}

bool on_tick(core::Price price, core::Price tick) {
    return tick.raw() <= 0 || price.raw() % tick.raw() == 0;
}

Rejection reject(RejectCode code, std::string message) {
    return {.code = code, .message = std::move(message)};
}

}  // namespace

Limits auto_limits() noexcept {
    return Limits{.max_lots_per_order = 2,
                  .max_open_lots_per_instrument = 2,
                  .max_gross_notional = 40'00'000 * 100,  // ₹40 lakh: a NIFTY and a SENSEX futures lot
                  .price_band = 10'000'000,               // 1%
                  // Four runs (futures and options on two indices) entering on one signal send
                  // 8 orders at once, and an options exit is re-priced every 3 s.
                  .orders_per_minute = 30,
                  .daily_loss = 20'000'000,               // 2%
                  .window_start = 9 * 3600 + 20 * 60,
                  .window_end = 15 * 3600 + 10 * 60};
}

std::string_view name(RejectCode code) noexcept {
    switch (code) {
    case RejectCode::NotTradable:
        return "not_tradable";
    case RejectCode::BadQuantity:
        return "bad_quantity";
    case RejectCode::FreezeQuantity:
        return "freeze_quantity";
    case RejectCode::BadPrice:
        return "bad_price";
    case RejectCode::BadTrigger:
        return "bad_trigger";
    case RejectCode::OptionWriting:
        return "option_writing";
    case RejectCode::MarketClosed:
        return "market_closed";
    case RejectCode::OutsideWindow:
        return "outside_window";
    case RejectCode::KillSwitch:
        return "kill_switch";
    case RejectCode::DailyLoss:
        return "daily_loss";
    case RejectCode::OrderSize:
        return "order_size";
    case RejectCode::PositionSize:
        return "position_size";
    case RejectCode::Notional:
        return "notional";
    case RejectCode::PriceBand:
        return "price_band";
    case RejectCode::NoPrice:
        return "no_price";
    case RejectCode::Funds:
        return "funds";
    case RejectCode::OrderRate:
        return "order_rate";
    }
    return "?";
}

const Limits& RiskManager::limits(oms::Source source) const noexcept {
    return source == oms::Source::Auto ? config_.automatic : config_.ui;
}

bool RiskManager::rate_ok(std::int64_t account, oms::Source source, Millis now) {
    auto& times = recent_[{account, source}];
    while (!times.empty() && times.front() <= now - 60'000) {
        times.pop_front();
    }
    return static_cast<int>(times.size()) < limits(source).orders_per_minute;
}

Paise RiskManager::margin(const InstrumentInfo& instrument, oms::Side side, std::int64_t quantity,
                          core::Price price) const noexcept {
    const Paise value = oms::value_paise(price, quantity);
    if (instrument.contract.segment == oms::Segment::Future) {
        const Ppb rate = instrument.futures_margin > 0 ? instrument.futures_margin
                         : instrument.index            ? config_.futures_margin
                                                       : config_.stock_futures_margin;
        return of(value, rate);
    }
    return side == oms::Side::Buy ? value : 0;  // a bought option blocks its premium
}

std::expected<void, Rejection> RiskManager::check_convert(const InstrumentInfo& instrument, oms::Product to,
                                                          std::int64_t quantity, Millis now) const {
    const std::int64_t lot = std::max<std::int64_t>(1, instrument.contract.lot_size);
    if (!instrument.listed || instrument.expiry_days < ist_day(now)) {
        return std::unexpected(reject(RejectCode::NotTradable, "not tradable: expired or not listed today"));
    }
    if (quantity <= 0 || quantity % lot != 0) {
        return std::unexpected(reject(RejectCode::BadQuantity,
                                      std::format("quantity must be a multiple of the lot size ({})", lot)));
    }
    const std::int64_t time_of_day = seconds_ist(now);
    const int open = instrument.market_open > 0 ? instrument.market_open : config_.market_open;
    const int close = instrument.market_close > 0 ? instrument.market_close : config_.market_close;
    if (time_of_day < open || time_of_day >= close) {
        return std::unexpected(reject(RejectCode::MarketClosed, "the market is closed"));
    }
    if (to == oms::Product::Mis && time_of_day >= close - (config_.market_close - config_.ui.window_end)) {
        return std::unexpected(reject(RejectCode::OutsideWindow, "too late to convert to MIS: it is squared off now"));
    }
    return {};
}

std::expected<Approval, Rejection> RiskManager::check(const oms::OrderSpec& order,
                                                      const InstrumentInfo& instrument,
                                                      const Quote& quote, const AccountView& view,
                                                      Millis now) {
    using oms::OrderType;
    using oms::Side;
    const std::int64_t lot = std::max<std::int64_t>(1, instrument.contract.lot_size);
    const bool buy = order.side == Side::Buy;
    const bool limit_priced = order.type == OrderType::Limit || order.type == OrderType::StopLimit;
    const bool stop = order.type == OrderType::StopLimit || order.type == OrderType::StopMarket;

    // 1. Validation.
    if (!instrument.listed || instrument.expiry_days < ist_day(now)) {
        return std::unexpected(reject(RejectCode::NotTradable, "not tradable: expired or not listed today"));
    }
    if (order.quantity <= 0 || order.quantity % lot != 0) {
        return std::unexpected(reject(RejectCode::BadQuantity,
                                      std::format("quantity must be a multiple of the lot size ({})", lot)));
    }
    if (instrument.freeze_quantity > 0 && order.quantity > instrument.freeze_quantity) {
        return std::unexpected(reject(RejectCode::FreezeQuantity,
                                      std::format("quantity above the freeze limit ({})", instrument.freeze_quantity)));
    }
    if (limit_priced && (order.price.raw() <= 0 || !on_tick(order.price, instrument.tick))) {
        return std::unexpected(reject(RejectCode::BadPrice,
                                      std::format("price must be a positive multiple of the tick ({})",
                                                  price_text(instrument.tick))));
    }
    // The exchange's circuit limits, when known: a price or trigger outside them can't trade.
    if (quote.lower_circuit.raw() > 0 && quote.upper_circuit.raw() > 0) {
        const auto outside = [&](core::Price p) { return p < quote.lower_circuit || p > quote.upper_circuit; };
        if ((limit_priced && outside(order.price)) || (stop && order.trigger.raw() > 0 && outside(order.trigger))) {
            return std::unexpected(reject(RejectCode::PriceBand,
                                          std::format("{} outside the circuit limits ({} – {})",
                                                      limit_priced && outside(order.price) ? "price" : "trigger",
                                                      price_text(quote.lower_circuit), price_text(quote.upper_circuit))));
        }
    }
    if (stop) {
        if (order.trigger.raw() <= 0 || !on_tick(order.trigger, instrument.tick)) {
            return std::unexpected(reject(RejectCode::BadTrigger, "trigger must be a positive multiple of the tick"));
        }
        if (quote.ltp.raw() <= 0) {
            return std::unexpected(reject(RejectCode::NoPrice, "no last price to place a stop order against"));
        }
        if (buy ? order.trigger <= quote.ltp : order.trigger >= quote.ltp) {
            return std::unexpected(reject(RejectCode::BadTrigger,
                                          std::format("trigger must be {} the last price ({})",
                                                      buy ? "above" : "below", price_text(quote.ltp))));
        }
        if (order.type == OrderType::StopLimit && (buy ? order.price < order.trigger : order.price > order.trigger)) {
            return std::unexpected(reject(RejectCode::BadPrice,
                                          std::format("price must be {} the trigger", buy ? "at or above" : "at or below")));
        }
    }
    const std::int64_t time_of_day = seconds_ist(now);
    const int open = instrument.market_open > 0 ? instrument.market_open : config_.market_open;
    const int close = instrument.market_close > 0 ? instrument.market_close : config_.market_close;
    if (time_of_day < open || time_of_day >= close) {
        return std::unexpected(reject(RejectCode::MarketClosed,
                                      std::format("the market is closed ({:02}:{:02}–{:02}:{:02})", open / 3600,
                                                  open / 60 % 60, close / 3600, close / 60 % 60)));
    }

    // 2. Reducing orders only shrink a position.
    const oms::Position* position =
        view.account != nullptr ? view.account->position(instrument.contract.id, order.product) : nullptr;
    const std::int64_t held = position != nullptr ? position->quantity() : 0;
    const bool reducing = held != 0 && (held > 0) != buy && order.quantity <= std::abs(held);
    if (instrument.contract.segment == oms::Segment::Option && !buy && !reducing) {
        return std::unexpected(reject(RejectCode::OptionWriting, "selling options you don't hold isn't supported yet"));
    }

    const Limits& limit = limits(order.source);
    if (!rate_ok(order.account, order.source, now)) {
        return std::unexpected(reject(RejectCode::OrderRate,
                                      std::format("more than {} orders a minute", limit.orders_per_minute)));
    }

    Approval approval{.reducing = reducing, .margin = 0};
    if (!reducing) {
        // 3. Opening orders.
        const int window_start = open + (limit.window_start - config_.market_open);
        const int window_end = close - (config_.market_close - limit.window_end);
        if (time_of_day < window_start || time_of_day >= window_end) {
            return std::unexpected(reject(RejectCode::OutsideWindow,
                                          "outside the trading window: only orders that reduce a position"));
        }
        if (view.kill_switch || global_kill_) {
            return std::unexpected(reject(RejectCode::KillSwitch,
                                          "the kill switch is on: only orders that reduce a position"));
        }
        if (view.day_pnl < 0 && -view.day_pnl >= of(view.opening_cash, limit.daily_loss)) {
            return std::unexpected(reject(RejectCode::DailyLoss,
                                          std::format("daily loss limit reached ({}): only orders that reduce a position",
                                                      money(view.day_pnl))));
        }
        if (limit.max_lots_per_order > 0 && order.quantity / lot > limit.max_lots_per_order) {
            return std::unexpected(reject(RejectCode::OrderSize,
                                          std::format("at most {} lots per order", limit.max_lots_per_order)));
        }
        const std::int64_t own = order.source == oms::Source::Auto ? view.auto_position : held;
        const std::int64_t after =
            std::abs(own + oms::sign(order.side) * (order.quantity + view.open_order_quantity));
        if (limit.max_open_lots_per_instrument > 0 && after / lot > limit.max_open_lots_per_instrument) {
            return std::unexpected(reject(RejectCode::PositionSize,
                                          std::format("at most {} lots open in one instrument",
                                                      limit.max_open_lots_per_instrument)));
        }
        core::Price reference = order.price;
        if (!limit_priced) {
            reference = buy ? quote.best_ask : quote.best_bid;
            if (reference.raw() <= 0) {
                reference = quote.ltp;
            }
        }
        if (reference.raw() <= 0 || quote.ltp.raw() <= 0) {
            return std::unexpected(reject(RejectCode::NoPrice, "no live price for this instrument"));
        }
        if (limit_priced && limit.price_band > 0) {
            // An SL becomes a limit only when the price reaches its trigger: its limit is
            // banded around the trigger (a stop 30% below the market is fine; a limit far
            // through its own trigger isn't).
            const bool stop_limit = order.type == OrderType::StopLimit;
            const core::Price around = stop_limit ? order.trigger : quote.ltp;
            const std::int64_t band = around.raw() * limit.price_band / kBillion;
            if (std::abs(order.price.raw() - around.raw()) > band) {
                return std::unexpected(reject(RejectCode::PriceBand,
                                              std::format("price more than {:.1f}% from the {} ({})",
                                                          static_cast<double>(limit.price_band) / 1e7,
                                                          stop_limit ? "trigger" : "last price", price_text(around))));
            }
        }
        const Paise value = oms::value_paise(reference, order.quantity);
        // The auto trader's limits are on its own positions: the user's trades don't use them up.
        const Paise exposure = order.source == oms::Source::Auto ? view.auto_notional : view.gross_notional;
        if (limit.max_gross_notional > 0 && exposure + value > limit.max_gross_notional) {
            return std::unexpected(reject(RejectCode::Notional,
                                          std::format("open positions would exceed {}", money(limit.max_gross_notional))));
        }
        core::Price margin_price = reference;
        if (!limit_priced && instrument.contract.segment == oms::Segment::Option) {
            margin_price = core::Price::from_raw(reference.raw() + reference.raw() * config_.market_buffer / kBillion);
        }
        approval.margin = margin(instrument, order.side, order.quantity, margin_price);
        const Paise cash = view.account != nullptr ? view.account->cash() : 0;
        const Paise available = cash - view.blocked + std::min<Paise>(0, view.unrealised);
        if (approval.margin > available) {
            return std::unexpected(reject(RejectCode::Funds,
                                          std::format("insufficient funds: needs {}, available {}",
                                                      money(approval.margin), money(available))));
        }
    }

    // 4. Counted toward the order rate once approved.
    recent_[{order.account, order.source}].push_back(now);
    return approval;
}

}  // namespace mda::rms

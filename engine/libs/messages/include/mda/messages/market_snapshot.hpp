#pragma once

#include <cstddef>
#include <cstdint>

#include <mda/core/price.hpp>
#include <mda/core/quantity.hpp>
#include <mda/core/timestamp.hpp>
#include <mda/messages/message.hpp>

namespace mda::messages {

/// Subscription mode the snapshot came from (Kite: ltp / quote / full).
enum class FeedMode : std::uint8_t {
    Ltp = 1,
    Quote = 2,
    Full = 3,
};

/// Which field groups of a MarketSnapshot carry data.
enum SnapshotFields : std::uint32_t {
    kFieldLtp = 1u << 0,            ///< ltp
    kFieldOhlc = 1u << 1,           ///< open, high, low, close (indices too)
    kFieldVolume = 1u << 2,         ///< last_qty, avg_price, volume, buy/sell totals
    kFieldOpenInterest = 1u << 3,   ///< oi, oi_day_high, oi_day_low (derivatives, full mode)
    kFieldExchangeTime = 1u << 4,   ///< exchange_ts
    kFieldLastTradeTime = 1u << 5,  ///< last_trade_ts
};

/// Latest state of one instrument (snapshot, not delta). 152 bytes.
struct MarketSnapshot {
    EventHeader header;
    std::uint32_t fields = 0;  ///< SnapshotFields bitmask
    FeedMode mode = FeedMode::Ltp;
    std::uint8_t reserved[3] = {};

    core::Timestamp receive_ts;     ///< when mda-feedd read the frame (ns)
    core::Timestamp exchange_ts;    ///< exchange time (1 s resolution)
    core::Timestamp last_trade_ts;  ///< last trade time (1 s resolution)

    core::Price ltp;
    core::Price avg_price;  ///< volume-weighted average traded price for the day
    core::Price open;
    core::Price high;
    core::Price low;
    core::Price close;  ///< previous day's close

    core::Quantity last_qty;
    core::Quantity volume;  ///< cumulative for the day
    core::Quantity total_buy_qty;
    core::Quantity total_sell_qty;
    core::Quantity oi;
    core::Quantity oi_day_high;
    core::Quantity oi_day_low;
};

template <>
struct MessageTraits<MarketSnapshot> {
    static constexpr MessageType type = MessageType::MarketSnapshot;
    static constexpr std::uint16_t version = 1;
};

static_assert(Message<MarketSnapshot>);
static_assert(sizeof(MarketSnapshot) == 152);
static_assert(offsetof(MarketSnapshot, fields) == 16);
static_assert(offsetof(MarketSnapshot, mode) == 20);
static_assert(offsetof(MarketSnapshot, receive_ts) == 24);
static_assert(offsetof(MarketSnapshot, exchange_ts) == 32);
static_assert(offsetof(MarketSnapshot, last_trade_ts) == 40);
static_assert(offsetof(MarketSnapshot, ltp) == 48);
static_assert(offsetof(MarketSnapshot, avg_price) == 56);
static_assert(offsetof(MarketSnapshot, open) == 64);
static_assert(offsetof(MarketSnapshot, high) == 72);
static_assert(offsetof(MarketSnapshot, low) == 80);
static_assert(offsetof(MarketSnapshot, close) == 88);
static_assert(offsetof(MarketSnapshot, last_qty) == 96);
static_assert(offsetof(MarketSnapshot, volume) == 104);
static_assert(offsetof(MarketSnapshot, total_buy_qty) == 112);
static_assert(offsetof(MarketSnapshot, total_sell_qty) == 120);
static_assert(offsetof(MarketSnapshot, oi) == 128);
static_assert(offsetof(MarketSnapshot, oi_day_high) == 136);
static_assert(offsetof(MarketSnapshot, oi_day_low) == 144);

}  // namespace mda::messages

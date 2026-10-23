#include <mda/kite/tick_decoder.hpp>

namespace mda::kite {
namespace {

using core::Price;
using core::Quantity;
using core::Timestamp;
using messages::FeedMode;
using messages::MarketSnapshot;

// Packet sizes (docs/design/kite-adapter.md §2).
constexpr std::size_t kLtpSize = 8;
constexpr std::size_t kQuoteSize = 44;
constexpr std::size_t kFullSize = 184;
constexpr std::size_t kIndexQuoteSize = 28;
constexpr std::size_t kIndexFullSize = 32;
constexpr std::size_t kDepthOffset = 64;
constexpr std::size_t kDepthEntrySize = 12;

/// Reads fields of one packet whose length has already been validated.
class PacketReader {
public:
    PacketReader(std::span<const std::byte> packet, Segment segment) noexcept
        : packet_(packet), segment_(segment) {}

    [[nodiscard]] std::uint32_t u32(std::size_t offset) const noexcept {
        return core::load_be<std::uint32_t>(packet_, offset);
    }
    [[nodiscard]] std::uint16_t u16(std::size_t offset) const noexcept {
        return core::load_be<std::uint16_t>(packet_, offset);
    }
    [[nodiscard]] Price price(std::size_t offset) const noexcept {
        const std::uint32_t value = u32(offset);
        switch (segment_) {
            case Segment::Cds:  // divisor 10^7 -> our 10^4
                return Price::from_raw(value / 1'000);
            case Segment::Bcd:  // divisor 10^4 == our scale
                return Price::from_raw(value);
            default:  // paise
                return Price::from_paise(value);
        }
    }
    [[nodiscard]] Quantity quantity(std::size_t offset) const noexcept {
        return Quantity(u32(offset));
    }
    [[nodiscard]] Timestamp seconds(std::size_t offset) const noexcept {
        return Timestamp::from_seconds(u32(offset));
    }

private:
    std::span<const std::byte> packet_;
    Segment segment_;
};

void decode_tradable(const PacketReader& r, std::size_t size, Segment segment,
                     MarketSnapshot& s) noexcept {
    s.ltp = r.price(4);
    s.fields = messages::kFieldLtp;
    s.mode = FeedMode::Ltp;
    if (size == kLtpSize) {
        return;
    }
    s.last_qty = r.quantity(8);
    s.avg_price = r.price(12);
    s.volume = r.quantity(16);
    s.total_buy_qty = r.quantity(20);
    s.total_sell_qty = r.quantity(24);
    s.open = r.price(28);
    s.high = r.price(32);
    s.low = r.price(36);
    s.close = r.price(40);
    s.fields |= messages::kFieldOhlc | messages::kFieldVolume;
    s.mode = FeedMode::Quote;
    if (size == kQuoteSize) {
        return;
    }
    s.last_trade_ts = r.seconds(44);
    s.oi = r.quantity(48);
    s.oi_day_high = r.quantity(52);
    s.oi_day_low = r.quantity(56);
    s.exchange_ts = r.seconds(60);
    s.fields |= messages::kFieldExchangeTime | messages::kFieldLastTradeTime;
    if (is_derivative(segment)) {
        s.fields |= messages::kFieldOpenInterest;
    }
    s.mode = FeedMode::Full;
}

void decode_index(const PacketReader& r, std::size_t size, MarketSnapshot& s) noexcept {
    s.ltp = r.price(4);
    s.fields = messages::kFieldLtp;
    s.mode = FeedMode::Ltp;
    if (size == kLtpSize) {
        return;
    }
    // Index layout order differs from tradable packets: high, low, open, close.
    s.high = r.price(8);
    s.low = r.price(12);
    s.open = r.price(16);
    s.close = r.price(20);
    // 24..28 is the signed price change (ltp - close): derivable, not stored.
    s.fields |= messages::kFieldOhlc;
    s.mode = FeedMode::Quote;
    if (size == kIndexQuoteSize) {
        return;
    }
    s.exchange_ts = r.seconds(28);
    s.fields |= messages::kFieldExchangeTime;
    s.mode = FeedMode::Full;
}

void decode_depth(const PacketReader& r, messages::DepthSnapshot& depth) noexcept {
    for (std::size_t i = 0; i < 2 * messages::kDepthLevels; ++i) {
        const std::size_t offset = kDepthOffset + i * kDepthEntrySize;
        messages::DepthLevel& level =
            i < messages::kDepthLevels ? depth.bids[i] : depth.asks[i - messages::kDepthLevels];
        level.quantity = r.u32(offset);
        level.price = r.price(offset + 4);
        level.orders = r.u16(offset + 8);
    }
}

bool valid_size(std::size_t size, bool index) noexcept {
    return index ? (size == kLtpSize || size == kIndexQuoteSize || size == kIndexFullSize)
                 : (size == kLtpSize || size == kQuoteSize || size == kFullSize);
}

}  // namespace

PacketResult decode_packet(std::span<const std::byte> packet, core::Timestamp receive_ts,
                           const TokenMap& tokens, DecodedPacket& out) noexcept {
    if (packet.size() < 4) {
        return PacketResult::Malformed;
    }
    const auto token = core::load_be<std::uint32_t>(packet, 0);
    const Segment segment = segment_of(token);
    const bool index = segment == Segment::Indices;
    if (!valid_size(packet.size(), index)) {
        return PacketResult::Malformed;
    }
    const core::InstrumentId id = tokens.find(token);
    if (!id.valid()) {
        return PacketResult::UnknownToken;
    }

    const PacketReader reader(packet, segment);
    out.snapshot = MarketSnapshot{};
    out.snapshot.header = messages::make_header<MarketSnapshot>(id, 0);
    out.snapshot.receive_ts = receive_ts;
    if (index) {
        decode_index(reader, packet.size(), out.snapshot);
    } else {
        decode_tradable(reader, packet.size(), segment, out.snapshot);
    }

    out.has_depth = !index && packet.size() == kFullSize;
    if (out.has_depth) {
        out.depth = messages::DepthSnapshot{};
        out.depth.header = messages::make_header<messages::DepthSnapshot>(id, 0);
        out.depth.receive_ts = receive_ts;
        decode_depth(reader, out.depth);
    }
    return PacketResult::Decoded;
}

}  // namespace mda::kite

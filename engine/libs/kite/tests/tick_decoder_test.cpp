#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include <mda/kite/tick_decoder.hpp>

namespace mda::kite {
namespace {

using core::InstrumentId;
using core::Price;
using core::Quantity;
using core::Timestamp;
using messages::DepthSnapshot;
using messages::FeedMode;
using messages::MarketSnapshot;

// Tokens: low byte is the segment (docs/design/kite-adapter.md §2).
constexpr std::uint32_t kNiftyFutToken = (12'345u << 8) | 2;   // NFO
constexpr std::uint32_t kRelianceToken = (738'561u << 8) | 1;  // NSE equity
constexpr std::uint32_t kNiftyIndexToken = 256'265;            // low byte 9 = indices
constexpr std::uint32_t kUsdInrToken = (4'242u << 8) | 3;      // CDS
constexpr std::uint32_t kUnmappedToken = (999u << 8) | 2;

const Timestamp kReceived = Timestamp::from_ns(1'761'110'000'123'456'789);

/// Builds big-endian Kite packets and frames.
class Bytes {
public:
    Bytes& u32(std::uint32_t v) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            bytes_.push_back(static_cast<std::byte>((v >> shift) & 0xFF));
        }
        return *this;
    }
    Bytes& u16(std::uint16_t v) {
        bytes_.push_back(static_cast<std::byte>(v >> 8));
        bytes_.push_back(static_cast<std::byte>(v & 0xFF));
        return *this;
    }
    Bytes& raw(std::span<const std::byte> more) {
        bytes_.insert(bytes_.end(), more.begin(), more.end());
        return *this;
    }
    [[nodiscard]] const std::vector<std::byte>& get() const { return bytes_; }

private:
    std::vector<std::byte> bytes_;
};

std::vector<std::byte> ltp_packet(std::uint32_t token, std::uint32_t ltp) {
    return Bytes().u32(token).u32(ltp).get();
}

std::vector<std::byte> quote_packet(std::uint32_t token) {
    return Bytes()
        .u32(token)
        .u32(2'458'050)  // ltp 24580.50
        .u32(75)         // last qty
        .u32(2'455'000)  // avg 24550.00
        .u32(1'234'500)  // volume
        .u32(300'000)    // total buy qty
        .u32(250'000)    // total sell qty
        .u32(2'440'000)  // open
        .u32(2'465'000)  // high
        .u32(2'435'000)  // low
        .u32(2'442'000)  // close (previous day)
        .get();
}

std::vector<std::byte> full_packet(std::uint32_t token) {
    Bytes b;
    b.raw(quote_packet(token))
        .u32(1'761'110'000)                   // last trade time
        .u32(9'876'000)                       // OI
        .u32(9'900'000)                       // OI day high
        .u32(9'800'000)                       // OI day low
        .u32(1'761'110'001);                  // exchange time
    for (std::uint32_t i = 0; i < 10; ++i) {  // 5 bids then 5 asks
        const bool bid = i < 5;
        const std::uint32_t level = bid ? i : i - 5;
        b.u32(100 * (level + 1))                                       // quantity
            .u32(bid ? 2'458'000 - 5 * level : 2'458'100 + 5 * level)  // price
            .u16(static_cast<std::uint16_t>(level + 1))                // orders
            .u16(0);                                                   // padding
    }
    return b.get();
}

std::vector<std::byte> index_packet(bool full) {
    Bytes b;
    b.u32(kNiftyIndexToken)
        .u32(2'450'010)  // ltp
        .u32(2'460'000)  // high
        .u32(2'430'000)  // low
        .u32(2'440'000)  // open
        .u32(2'442'000)  // close
        .u32(8'010);     // change (signed)
    if (full) {
        b.u32(1'761'110'002);
    }
    return b.get();
}

std::vector<std::byte> frame(const std::vector<std::vector<std::byte>>& packets) {
    Bytes b;
    b.u16(static_cast<std::uint16_t>(packets.size()));
    for (const auto& packet : packets) {
        b.u16(static_cast<std::uint16_t>(packet.size())).raw(packet);
    }
    return b.get();
}

class TickDecoderTest : public ::testing::Test {
protected:
    TickDecoderTest() {
        tokens_.add(kNiftyFutToken, InstrumentId(101));
        tokens_.add(kRelianceToken, InstrumentId(102));
        tokens_.add(kNiftyIndexToken, InstrumentId(103));
        tokens_.add(kUsdInrToken, InstrumentId(104));
    }

    struct Delivered {
        MarketSnapshot snapshot;
        std::optional<DepthSnapshot> depth;
    };

    std::vector<Delivered> decode(const std::vector<std::byte>& bytes,
                                  FrameResult expected = FrameResult::Ok) {
        std::vector<Delivered> out;
        const FrameResult result =
            decoder_.decode(bytes, kReceived, [&](const MarketSnapshot& s, const DepthSnapshot* d) {
                out.push_back({s, d != nullptr ? std::optional(*d) : std::nullopt});
            });
        EXPECT_EQ(result, expected);
        return out;
    }

    TokenMap tokens_;
    TickDecoder decoder_{tokens_};
};

// --------------------------------------------------------------------------- packet modes

TEST_F(TickDecoderTest, DecodesLtpPacket) {
    const auto out = decode(frame({ltp_packet(kNiftyFutToken, 2'458'050)}));
    ASSERT_EQ(out.size(), 1u);
    const MarketSnapshot& s = out[0].snapshot;
    EXPECT_EQ(s.header.instrument_id, InstrumentId(101));
    EXPECT_EQ(s.mode, FeedMode::Ltp);
    EXPECT_EQ(s.fields, messages::kFieldLtp);
    EXPECT_EQ(s.ltp, Price::from_paise(2'458'050));
    EXPECT_EQ(s.receive_ts, kReceived);
    EXPECT_FALSE(out[0].depth.has_value());
}

TEST_F(TickDecoderTest, DecodesQuotePacket) {
    const auto out = decode(frame({quote_packet(kRelianceToken)}));
    ASSERT_EQ(out.size(), 1u);
    const MarketSnapshot& s = out[0].snapshot;
    EXPECT_EQ(s.mode, FeedMode::Quote);
    EXPECT_EQ(s.fields, messages::kFieldLtp | messages::kFieldOhlc | messages::kFieldVolume);
    EXPECT_EQ(core::to_string(s.ltp), "24580.50");
    EXPECT_EQ(s.last_qty, Quantity(75));
    EXPECT_EQ(s.avg_price, Price::from_paise(2'455'000));
    EXPECT_EQ(s.volume, Quantity(1'234'500));
    EXPECT_EQ(s.total_buy_qty, Quantity(300'000));
    EXPECT_EQ(s.total_sell_qty, Quantity(250'000));
    EXPECT_EQ(s.open, Price::from_paise(2'440'000));
    EXPECT_EQ(s.high, Price::from_paise(2'465'000));
    EXPECT_EQ(s.low, Price::from_paise(2'435'000));
    EXPECT_EQ(s.close, Price::from_paise(2'442'000));
}

TEST_F(TickDecoderTest, DecodesFullPacketWithOpenInterestAndDepth) {
    const auto out = decode(frame({full_packet(kNiftyFutToken)}));
    ASSERT_EQ(out.size(), 1u);
    const MarketSnapshot& s = out[0].snapshot;
    EXPECT_EQ(s.mode, FeedMode::Full);
    EXPECT_TRUE(s.fields & messages::kFieldOpenInterest);
    EXPECT_TRUE(s.fields & messages::kFieldExchangeTime);
    EXPECT_EQ(s.last_trade_ts, Timestamp::from_seconds(1'761'110'000));
    EXPECT_EQ(s.exchange_ts, Timestamp::from_seconds(1'761'110'001));
    EXPECT_EQ(s.oi, Quantity(9'876'000));
    EXPECT_EQ(s.oi_day_high, Quantity(9'900'000));
    EXPECT_EQ(s.oi_day_low, Quantity(9'800'000));

    ASSERT_TRUE(out[0].depth.has_value());
    const DepthSnapshot& d = *out[0].depth;
    EXPECT_EQ(d.header.instrument_id, InstrumentId(101));
    EXPECT_EQ(d.bids[0].price, Price::from_paise(2'458'000));
    EXPECT_EQ(d.bids[0].quantity, 100u);
    EXPECT_EQ(d.bids[0].orders, 1u);
    EXPECT_EQ(d.bids[4].price, Price::from_paise(2'457'980));
    EXPECT_EQ(d.asks[0].price, Price::from_paise(2'458'100));
    EXPECT_EQ(d.asks[4].quantity, 500u);
    EXPECT_EQ(d.asks[4].orders, 5u);
}

TEST_F(TickDecoderTest, EquityFullPacketHasNoOpenInterestFlag) {
    const auto out = decode(frame({full_packet(kRelianceToken)}));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_FALSE(out[0].snapshot.fields & messages::kFieldOpenInterest);
    EXPECT_TRUE(out[0].depth.has_value());
}

TEST_F(TickDecoderTest, DecodesIndexPacketsWithTheirOwnFieldOrder) {
    for (const bool full : {false, true}) {
        const auto out = decode(frame({index_packet(full)}));
        ASSERT_EQ(out.size(), 1u);
        const MarketSnapshot& s = out[0].snapshot;
        EXPECT_EQ(s.header.instrument_id, InstrumentId(103));
        EXPECT_EQ(s.ltp, Price::from_paise(2'450'010));
        EXPECT_EQ(s.high, Price::from_paise(2'460'000));  // high comes before low/open
        EXPECT_EQ(s.low, Price::from_paise(2'430'000));
        EXPECT_EQ(s.open, Price::from_paise(2'440'000));
        EXPECT_EQ(s.close, Price::from_paise(2'442'000));
        EXPECT_FALSE(s.fields & messages::kFieldVolume);  // indices have no volume
        EXPECT_EQ(s.mode, full ? FeedMode::Full : FeedMode::Quote);
        EXPECT_EQ(s.exchange_ts.is_set(), full);
        EXPECT_FALSE(out[0].depth.has_value());
    }
}

TEST_F(TickDecoderTest, ScalesCurrencyDerivativePrices) {
    // CDS prices use a 10^7 divisor: 83.2525 arrives as 832'525'000.
    const auto out = decode(frame({ltp_packet(kUsdInrToken, 832'525'000)}));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(core::to_string(out[0].snapshot.ltp), "83.2525");
}

// --------------------------------------------------------------------------- framing

TEST_F(TickDecoderTest, DecodesEveryPacketInAMultiPacketFrame) {
    const auto out = decode(frame({ltp_packet(kNiftyFutToken, 100), quote_packet(kRelianceToken),
                                   index_packet(true), full_packet(kNiftyFutToken)}));
    ASSERT_EQ(out.size(), 4u);
    EXPECT_EQ(out[1].snapshot.header.instrument_id, InstrumentId(102));
    EXPECT_EQ(decoder_.stats().packets, 4u);
}

TEST_F(TickDecoderTest, OneByteFrameIsAHeartbeat) {
    const std::vector<std::byte> heartbeat = {std::byte{0}};
    EXPECT_TRUE(decode(heartbeat, FrameResult::Heartbeat).empty());
    EXPECT_EQ(decoder_.stats().heartbeats, 1u);
}

TEST_F(TickDecoderTest, SkipsAndCountsUnknownTokens) {
    const auto out = decode(frame({ltp_packet(kUnmappedToken, 1), ltp_packet(kNiftyFutToken, 2)}));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].snapshot.ltp, Price::from_paise(2));
    EXPECT_EQ(decoder_.stats().unknown_tokens, 1u);
}

TEST_F(TickDecoderTest, RejectsPacketWithUnknownLength) {
    auto bad = ltp_packet(kNiftyFutToken, 1);
    bad.push_back(std::byte{0});  // 9 bytes: no such layout
    const auto out = decode(frame({ltp_packet(kNiftyFutToken, 7), bad}), FrameResult::Malformed);
    EXPECT_EQ(out.size(), 1u);  // the good packet before it was delivered
    EXPECT_EQ(decoder_.stats().malformed, 1u);
}

TEST_F(TickDecoderTest, RejectsTruncatedFrames) {
    const auto good = frame({full_packet(kNiftyFutToken)});
    for (const std::size_t cut :
         {std::size_t{0}, std::size_t{2}, std::size_t{3}, good.size() - 1}) {
        const std::vector<std::byte> truncated(good.begin(),
                                               good.begin() + static_cast<std::ptrdiff_t>(cut));
        EXPECT_TRUE(decode(truncated, FrameResult::Malformed).empty()) << "cut at " << cut;
    }
}

TEST_F(TickDecoderTest, IndexTokenWithTradableLengthIsMalformed) {
    // An index token with a 44-byte (tradable quote) layout is not a valid combination.
    EXPECT_TRUE(decode(frame({quote_packet(kNiftyIndexToken)}), FrameResult::Malformed).empty());
}

// --------------------------------------------------------------------------- robustness

// Poor man's fuzzing: random frames — fully random, and valid frames with random
// corruptions — must never crash or read out of bounds (run under ASan/UBSan).
TEST_F(TickDecoderTest, SurvivesRandomAndCorruptedInput) {
    std::mt19937_64 rng(20261022);
    const std::vector<std::byte> valid =
        frame({full_packet(kNiftyFutToken), index_packet(true), quote_packet(kRelianceToken)});
    std::uint64_t delivered = 0;
    auto sink = [&](const MarketSnapshot&, const DepthSnapshot*) { ++delivered; };

    for (int i = 0; i < 200'000; ++i) {
        std::vector<std::byte> input;
        if (i % 2 == 0) {
            input.resize(rng() % 600);
            for (auto& b : input) {
                b = static_cast<std::byte>(rng());
            }
        } else {
            input = valid;
            for (int flips = 0; flips < 4; ++flips) {
                input[rng() % input.size()] = static_cast<std::byte>(rng());
            }
            input.resize(rng() % (input.size() + 1));
        }
        (void)decoder_.decode(input, kReceived, sink);
    }
    EXPECT_EQ(decoder_.stats().frames, 200'000u);
    EXPECT_GT(delivered, 0u);  // some corrupted-but-valid frames still decode
}

}  // namespace
}  // namespace mda::kite

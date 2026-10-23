#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <unistd.h>

#include <mda/bus/bus.hpp>
#include <mda/messages/circuit_limits.hpp>
#include <mda/messages/depth_snapshot.hpp>
#include <mda/messages/feed_status.hpp>
#include <mda/messages/market_snapshot.hpp>
#include <mda/messages/raw_frame.hpp>

namespace mda::messages {
namespace {

using core::InstrumentId;
using core::Price;
using core::Quantity;
using core::Timestamp;

MarketSnapshot sample_snapshot() {
    MarketSnapshot s{};
    s.header = make_header<MarketSnapshot>(InstrumentId(48'213), 77);
    s.fields = kFieldLtp | kFieldOhlc | kFieldVolume | kFieldOpenInterest | kFieldExchangeTime;
    s.mode = FeedMode::Full;
    s.receive_ts = Timestamp::from_ns(1'761'000'000'123'456'789);
    s.exchange_ts = Timestamp::from_seconds(1'761'000'000);
    s.ltp = Price::from_paise(12'345);
    s.close = Price::from_paise(12'000);
    s.volume = Quantity(1'500'000);
    s.oi = Quantity(9'876'000);
    return s;
}

TEST(MessagesTest, HeaderCarriesTypeVersion) {
    const MarketSnapshot s = sample_snapshot();
    EXPECT_EQ(s.header.schema_version, MessageTraits<MarketSnapshot>::version);
    EXPECT_EQ(s.header.instrument_id, InstrumentId(48'213));
    EXPECT_EQ(s.header.sequence, 77u);
}

TEST(MessagesTest, MarketSnapshotRoundTrips) {
    const MarketSnapshot original = sample_snapshot();
    const auto bytes = as_bytes(original);
    ASSERT_EQ(bytes.size(), 152u);

    const auto decoded = decode<MarketSnapshot>(to_wire(MessageType::MarketSnapshot), bytes);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->ltp, original.ltp);
    EXPECT_EQ(decoded->oi, original.oi);
    EXPECT_EQ(decoded->receive_ts, original.receive_ts);
    EXPECT_EQ(decoded->mode, FeedMode::Full);
    EXPECT_EQ(decoded->header.sequence, 77u);
}

TEST(MessagesTest, DecodeRejectsWrongTypeSizeOrVersion) {
    const MarketSnapshot original = sample_snapshot();
    const auto bytes = as_bytes(original);

    EXPECT_EQ(decode<MarketSnapshot>(to_wire(MessageType::DepthSnapshot), bytes).error(),
              DecodeError::WrongType);
    EXPECT_EQ(
        decode<MarketSnapshot>(to_wire(MessageType::MarketSnapshot), bytes.first(100)).error(),
        DecodeError::WrongSize);

    MarketSnapshot future = original;
    future.header.schema_version = 99;  // written by a newer build
    EXPECT_EQ(
        decode<MarketSnapshot>(to_wire(MessageType::MarketSnapshot), as_bytes(future)).error(),
        DecodeError::UnsupportedVersion);
}

TEST(MessagesTest, CircuitLimitsRoundTrip) {
    CircuitLimits limits{};
    limits.header = make_header<CircuitLimits>(InstrumentId{42}, 3);
    limits.lower = core::Price::from_raw(8'334'000);
    limits.upper = core::Price::from_raw(10'186'000);
    const auto decoded =
        decode<CircuitLimits>(to_wire(MessageType::CircuitLimits), as_bytes(limits));
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->header.instrument_id, InstrumentId{42});
    EXPECT_EQ(decoded->upper, core::Price::from_raw(10'186'000));
    EXPECT_FALSE(
        decode<FeedStatus>(to_wire(MessageType::CircuitLimits), as_bytes(limits)).has_value());
}

TEST(MessagesTest, DepthAndFeedStatusRoundTrip) {
    DepthSnapshot depth{};
    depth.header = make_header<DepthSnapshot>(InstrumentId(5), 1);
    depth.bids[0] = {.price = Price::from_paise(12'340), .quantity = 750, .orders = 3};
    depth.asks[4] = {.price = Price::from_paise(12'360), .quantity = 1'500, .orders = 9};
    const auto d = decode<DepthSnapshot>(to_wire(MessageType::DepthSnapshot), as_bytes(depth));
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->bids[0].quantity, 750u);
    EXPECT_EQ(d->asks[4].price, Price::from_paise(12'360));

    FeedStatus status{};
    status.header = make_header<FeedStatus>(InstrumentId{}, 2);
    status.state = FeedState::AuthExpired;
    status.connection_id = 3;
    const auto f = decode<FeedStatus>(to_wire(MessageType::FeedStatus), as_bytes(status));
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->state, FeedState::AuthExpired);
    EXPECT_EQ(f->connection_id, 3u);
}

TEST(MessagesTest, RawFrameRoundTripsAndValidatesLength) {
    const std::vector<std::byte> frame = {std::byte{0x00}, std::byte{0x01}, std::byte{0xAB}};
    std::array<std::byte, 64> buffer{};
    RawFrameHeader header{};
    header.header.sequence = 9;
    header.receive_ts = Timestamp::from_ns(42);
    header.connection_id = 2;

    const auto encoded = encode_raw_frame(buffer, header, frame);
    ASSERT_EQ(encoded.size(), raw_frame_size(frame.size()));

    const auto decoded = decode_raw_frame(to_wire(MessageType::RawFrame), encoded);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->header.length, 3u);
    EXPECT_EQ(decoded->header.connection_id, 2u);
    EXPECT_TRUE(std::ranges::equal(decoded->frame, frame));

    EXPECT_EQ(
        decode_raw_frame(to_wire(MessageType::RawFrame), encoded.first(encoded.size() - 1)).error(),
        DecodeError::WrongSize);
}

// End to end: a snapshot published on a real bus arrives intact in a reader.
TEST(MessagesTest, SnapshotTravelsThroughTheBus) {
    const std::string bus_name = "msg" + std::to_string(::getpid());
    auto writer = bus::Writer::create_or_attach({.name = bus_name, .capacity = 64 * 1024});
    ASSERT_TRUE(writer.has_value());
    auto reader = bus::Reader::attach(bus_name, {.register_slot = false});
    ASSERT_TRUE(reader.has_value());

    const MarketSnapshot original = sample_snapshot();
    ASSERT_TRUE(writer->publish(to_wire(MessageType::MarketSnapshot), as_bytes(original)));

    std::optional<MarketSnapshot> received;
    reader->poll([&](bus::MsgType type, std::span<const std::byte> payload) {
        if (auto decoded = decode<MarketSnapshot>(type, payload)) {
            received = *decoded;
        }
    });
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->ltp, original.ltp);
    EXPECT_EQ(received->header.instrument_id, original.header.instrument_id);
    (void)bus::remove_bus(bus_name);
}

}  // namespace
}  // namespace mda::messages

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <mda/bus/ring.hpp>
#include <mda/bus/ring_reader.hpp>
#include <mda/bus/ring_writer.hpp>

namespace mda::bus {
namespace {

constexpr MsgType kTestType = 7;

/// Ring storage in ordinary memory (Unit 2 will put the same layout in shared memory).
class TestRing {
public:
    explicit TestRing(std::size_t capacity) : storage_(capacity / 8) {}

    RingCounters& counters() { return *counters_; }
    std::span<std::byte> bytes() { return std::as_writable_bytes(std::span(storage_)); }
    RingWriter writer() { return RingWriter::create(*counters_, bytes()).value(); }
    RingReader reader() { return RingReader::create(*counters_, bytes()).value(); }

private:
    std::unique_ptr<RingCounters> counters_ = std::make_unique<RingCounters>();
    std::vector<std::uint64_t> storage_;  // 8-byte aligned
};

/// Deterministic payload whose first 8 bytes (when it has them) hold the sequence number.
std::vector<std::byte> make_payload(std::uint64_t seq, std::size_t size) {
    std::vector<std::byte> payload(size);
    for (std::size_t i = 0; i < size; ++i) {
        payload[i] = static_cast<std::byte>((seq * 31 + i * 7) & 0xFF);
    }
    if (size >= sizeof(seq)) {
        std::memcpy(payload.data(), &seq, sizeof(seq));
    }
    return payload;
}

struct Received {
    MsgType type;
    std::vector<std::byte> payload;
};

/// Polls once; returns the message if one was delivered.
std::optional<Received> poll_message(RingReader& reader) {
    std::optional<Received> received;
    const PollResult result = reader.poll([&](MsgType type, std::span<const std::byte> payload) {
        received = Received{type, {payload.begin(), payload.end()}};
    });
    EXPECT_EQ(result.status == PollStatus::Message, received.has_value());
    return received;
}

void publish(RingWriter& writer, std::uint64_t seq, std::size_t size) {
    ASSERT_TRUE(writer.publish(kTestType, make_payload(seq, size)).has_value());
}

void expect_message(RingReader& reader, std::uint64_t seq, std::size_t size) {
    const auto received = poll_message(reader);
    ASSERT_TRUE(received.has_value()) << "seq " << seq;
    EXPECT_EQ(received->type, kTestType);
    EXPECT_EQ(received->payload, make_payload(seq, size)) << "seq " << seq;
}

// --------------------------------------------------------------------------- validation

TEST(RingTest, RejectsInvalidRingMemory) {
    TestRing ring(4096);
    EXPECT_EQ(validate_ring(ring.bytes().first(3000)).error(), RingError::CapacityNotPowerOfTwo);
    EXPECT_EQ(validate_ring(ring.bytes().first(512)).error(), RingError::CapacityTooSmall);
    EXPECT_EQ(validate_ring(ring.bytes().subspan(1, 1024)).error(), RingError::Misaligned);
    EXPECT_TRUE(validate_ring(ring.bytes()).has_value());
    EXPECT_FALSE(RingWriter::create(ring.counters(), ring.bytes().first(3000)).has_value());
    EXPECT_FALSE(RingReader::create(ring.counters(), ring.bytes().first(3000)).has_value());
}

TEST(RingTest, EnforcesMessageLimits) {
    TestRing ring(1024);
    auto writer = ring.writer();
    auto reader = ring.reader();
    ASSERT_EQ(writer.max_message_length(), 128u);

    EXPECT_EQ(writer.publish(kTestType, make_payload(0, 129)).error(),
              PublishError::MessageTooLarge);
    EXPECT_EQ(writer.publish(kPaddingMsgType, make_payload(0, 8)).error(),
              PublishError::ReservedMsgType);
    EXPECT_EQ(reader.poll([](auto, auto) {}).status, PollStatus::Empty);  // nothing published

    publish(writer, 1, 128);
    expect_message(reader, 1, 128);
}

// --------------------------------------------------------------------------- basic delivery

TEST(RingTest, EmptyRingReturnsEmpty) {
    TestRing ring(1024);
    auto reader = ring.reader();
    EXPECT_EQ(reader.poll([](auto, auto) { FAIL(); }).status, PollStatus::Empty);
}

TEST(RingTest, DeliversSingleMessage) {
    TestRing ring(1024);
    auto writer = ring.writer();
    auto reader = ring.reader();

    publish(writer, 42, 37);
    expect_message(reader, 42, 37);
    EXPECT_EQ(reader.poll([](auto, auto) {}).status, PollStatus::Empty);
    EXPECT_EQ(reader.position(), writer.position());
}

TEST(RingTest, DeliversZeroLengthMessage) {
    TestRing ring(1024);
    auto writer = ring.writer();
    auto reader = ring.reader();

    publish(writer, 0, 0);
    expect_message(reader, 0, 0);
    EXPECT_EQ(writer.position(), 8u);  // header only
}

// --------------------------------------------------------------------------- wrap-around

TEST(RingTest, RecordEndingExactlyAtRingEndNeedsNoPadding) {
    TestRing ring(1024);
    auto writer = ring.writer();
    auto reader = ring.reader();

    // 120-byte payload + 8-byte header = 128-byte records; 8 fill the ring exactly.
    for (std::uint64_t seq = 0; seq < 8; ++seq) {
        publish(writer, seq, 120);
        expect_message(reader, seq, 120);
    }
    EXPECT_EQ(writer.position(), 1024u);

    publish(writer, 8, 120);  // starts at offset 0, no padding
    EXPECT_EQ(writer.position(), 1024u + 128u);
    expect_message(reader, 8, 120);
}

TEST(RingTest, WrapsWithPaddingRecord) {
    TestRing ring(1024);
    auto writer = ring.writer();
    auto reader = ring.reader();

    // 96-byte records: 10 use 960 bytes; the 11th doesn't fit in the remaining 64.
    for (std::uint64_t seq = 0; seq < 10; ++seq) {
        publish(writer, seq, 88);
        expect_message(reader, seq, 88);
    }
    EXPECT_EQ(writer.position(), 960u);

    publish(writer, 10, 88);  // 64 bytes of padding, then the record at offset 0
    EXPECT_EQ(writer.position(), 1024u + 96u);
    expect_message(reader, 10, 88);  // padding is skipped transparently
    EXPECT_EQ(reader.position(), writer.position());
}

TEST(RingTest, PreservesOrderAndContentAcrossManyWraps) {
    TestRing ring(1024);
    auto writer = ring.writer();
    auto reader = ring.reader();

    for (std::uint64_t seq = 0; seq < 10'000; ++seq) {
        const std::size_t size = (seq * 13) % 129;  // 0..128, exercises every alignment
        publish(writer, seq, size);
        expect_message(reader, seq, size);
    }
    EXPECT_GT(writer.position(), 100u * 1024u);  // wrapped many times
}

// --------------------------------------------------------------------------- readers

TEST(RingTest, IndependentReadersEachSeeEveryMessage) {
    TestRing ring(1024);
    auto writer = ring.writer();
    auto first = ring.reader();
    auto second = ring.reader();

    for (std::uint64_t seq = 0; seq < 5; ++seq) {
        publish(writer, seq, 40);
    }
    for (std::uint64_t seq = 0; seq < 5; ++seq) {
        expect_message(first, seq, 40);
    }
    for (std::uint64_t seq = 0; seq < 5; ++seq) {
        expect_message(second, seq, 40);
    }
}

TEST(RingTest, LateReaderSeesOnlyNewMessages) {
    TestRing ring(1024);
    auto writer = ring.writer();
    publish(writer, 1, 16);

    auto reader = ring.reader();
    EXPECT_EQ(reader.poll([](auto, auto) {}).status, PollStatus::Empty);
    publish(writer, 2, 16);
    expect_message(reader, 2, 16);
}

TEST(RingTest, LappedReaderIsToldAndResumesAtLatest) {
    TestRing ring(1024);
    auto writer = ring.writer();
    auto reader = ring.reader();

    // 30 records of 96 bytes = 2880 bytes: the writer laps the idle reader.
    for (std::uint64_t seq = 0; seq < 30; ++seq) {
        publish(writer, seq, 88);
    }

    const PollResult lapped = reader.poll([](auto, auto) { FAIL(); });
    EXPECT_EQ(lapped.status, PollStatus::Lapped);
    EXPECT_GT(lapped.lost_bytes, 1024u);

    expect_message(reader, 29, 88);  // resumes at the most recent record
    EXPECT_EQ(reader.poll([](auto, auto) {}).status, PollStatus::Empty);

    publish(writer, 30, 88);
    expect_message(reader, 30, 88);
}

TEST(RingTest, RestartedWriterContinuesWhereThePreviousOneStopped) {
    TestRing ring(1024);
    auto reader = ring.reader();
    {
        auto writer = ring.writer();
        for (std::uint64_t seq = 0; seq < 3; ++seq) {
            publish(writer, seq, 50);
        }
    }  // writer "crashes"

    auto restarted = ring.writer();
    EXPECT_EQ(restarted.position(), 3u * 64u);
    for (std::uint64_t seq = 3; seq < 6; ++seq) {
        publish(restarted, seq, 50);
    }
    for (std::uint64_t seq = 0; seq < 6; ++seq) {
        expect_message(reader, seq, 50);
    }
}

// --------------------------------------------------------------------------- concurrency

// A writer thread floods a small ring while a reader thread polls. The reader is lapped
// often; every message it does deliver must be intact and in order — never torn.
// Run under ThreadSanitizer in CI (ci-tsan preset).
TEST(RingTest, ConcurrentReaderNeverSeesTornMessages) {
    constexpr std::uint64_t kMessages = 200'000;
    TestRing ring(4096);
    auto writer = ring.writer();
    auto reader = ring.reader();
    std::atomic<bool> writer_done{false};

    std::thread writer_thread([&] {
        for (std::uint64_t seq = 0; seq < kMessages; ++seq) {
            const std::size_t size = 8 + (seq * 37) % 200;
            (void)writer.publish(kTestType, make_payload(seq, size));
        }
        writer_done.store(true, std::memory_order_release);
    });

    std::uint64_t delivered = 0;
    std::uint64_t laps = 0;
    std::uint64_t corrupt = 0;
    std::optional<std::uint64_t> last_seq;
    bool out_of_order = false;
    auto handler = [&](MsgType type, std::span<const std::byte> payload) {
        std::uint64_t seq = 0;
        std::memcpy(&seq, payload.data(), sizeof(seq));
        if (type != kTestType || !std::ranges::equal(payload, make_payload(seq, payload.size()))) {
            ++corrupt;
        }
        if (last_seq && seq <= *last_seq) {
            out_of_order = true;
        }
        last_seq = seq;
        ++delivered;
    };

    while (true) {
        const bool done = writer_done.load(std::memory_order_acquire);
        const PollResult result = reader.poll(handler);
        if (result.status == PollStatus::Lapped) {
            ++laps;
        }
        if (done && result.status == PollStatus::Empty) {
            break;
        }
    }
    writer_thread.join();

    EXPECT_EQ(corrupt, 0u);
    EXPECT_FALSE(out_of_order);
    EXPECT_GT(delivered, 0u);
    EXPECT_EQ(last_seq, kMessages - 1);  // the final message is always delivered
    RecordProperty("delivered", static_cast<int>(delivered));
    RecordProperty("laps", static_cast<int>(laps));
}

}  // namespace
}  // namespace mda::bus

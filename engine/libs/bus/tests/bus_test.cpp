#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <mda/bus/bus.hpp>

namespace mda::bus {
namespace {

constexpr MsgType kTestType = 3;
constexpr std::size_t kCapacity = 64 * 1024;

std::vector<std::byte> payload_for(std::uint64_t value) {
    std::vector<std::byte> payload(sizeof(value));
    std::memcpy(payload.data(), &value, sizeof(value));
    return payload;
}

std::optional<std::uint64_t> poll_value(Reader& reader) {
    std::optional<std::uint64_t> value;
    reader.poll([&](MsgType type, std::span<const std::byte> payload) {
        EXPECT_EQ(type, kTestType);
        std::uint64_t v = 0;
        std::memcpy(&v, payload.data(), sizeof(v));
        value = v;
    });
    return value;
}

/// Each test gets its own bus name (tests may run in parallel processes) and removes
/// the segment afterwards.
class BusTest : public ::testing::Test {
protected:
    void SetUp() override {
        static std::atomic<int> counter{0};
        name_ = "t" + std::to_string(::getpid()) + "_" + std::to_string(counter++);
    }
    void TearDown() override { (void)remove_bus(name_); }

    [[nodiscard]] BusConfig config(std::size_t capacity = kCapacity) const {
        return {.name = name_, .capacity = capacity};
    }

    std::string name_;
};

// --------------------------------------------------------------------------- basics

TEST_F(BusTest, WriterCreatesBusAndReaderReceivesThroughSharedMemory) {
    auto writer = Writer::create_or_attach(config());
    ASSERT_TRUE(writer.has_value()) << to_string(writer.error());
    // A separate mapping of the same segment: the data really travels through shm.
    auto reader = Reader::attach(name_);
    ASSERT_TRUE(reader.has_value()) << to_string(reader.error());

    for (std::uint64_t value = 0; value < 1000; ++value) {
        ASSERT_TRUE(writer->publish(kTestType, payload_for(value)).has_value());
        EXPECT_EQ(poll_value(*reader), value);
    }
    EXPECT_EQ(poll_value(*reader), std::nullopt);
}

TEST_F(BusTest, ReaderBeforeWriterGetsNotFound) {
    auto reader = Reader::attach(name_);
    ASSERT_FALSE(reader.has_value());
    EXPECT_EQ(reader.error().code, BusErrc::NotFound);
}

TEST_F(BusTest, RejectsInvalidNamesAndCapacities) {
    EXPECT_EQ(Writer::create_or_attach({.name = "", .capacity = kCapacity}).error().code,
              BusErrc::InvalidName);
    EXPECT_EQ(Writer::create_or_attach({.name = "Has_Upper", .capacity = kCapacity}).error().code,
              BusErrc::InvalidName);
    EXPECT_EQ(Writer::create_or_attach({.name = "slash/name", .capacity = kCapacity}).error().code,
              BusErrc::InvalidName);
    EXPECT_EQ(Writer::create_or_attach({.name = std::string(21, 'a'), .capacity = kCapacity})
                  .error()
                  .code,
              BusErrc::InvalidName);
    EXPECT_EQ(Writer::create_or_attach(config(3000)).error().code, BusErrc::InvalidCapacity);
    EXPECT_EQ(Writer::create_or_attach(config(512)).error().code, BusErrc::InvalidCapacity);
    EXPECT_EQ(Reader::attach("Bad Name").error().code, BusErrc::InvalidName);
}

TEST_F(BusTest, ReportsWriterStatus) {
    auto writer = Writer::create_or_attach(config());
    ASSERT_TRUE(writer.has_value());
    writer->heartbeat();
    auto reader = Reader::attach(name_);
    ASSERT_TRUE(reader.has_value());

    const WriterStatus status = reader->writer_status();
    EXPECT_EQ(status.pid, static_cast<std::uint32_t>(::getpid()));
    EXPECT_EQ(status.epoch, 1u);
    EXPECT_GT(status.heartbeat_ns, 0u);
}

// --------------------------------------------------------------------------- writer ownership

TEST_F(BusTest, SecondWriterIsRefusedWhileFirstIsAlive) {
    auto first = Writer::create_or_attach(config());
    ASSERT_TRUE(first.has_value());
    auto second = Writer::create_or_attach(config());
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error().code, BusErrc::WriterAlreadyActive);
}

TEST_F(BusTest, RestartedWriterContinuesAndReaderKeepsReading) {
    auto reader_holder = std::optional<Reader>{};
    {
        auto writer = Writer::create_or_attach(config());
        ASSERT_TRUE(writer.has_value());
        reader_holder.emplace(*Reader::attach(name_));
        ASSERT_TRUE(writer->publish(kTestType, payload_for(1)).has_value());
    }  // clean shutdown releases ownership

    auto restarted = Writer::create_or_attach(config());
    ASSERT_TRUE(restarted.has_value()) << to_string(restarted.error());
    EXPECT_EQ(restarted->epoch(), 2u);
    ASSERT_TRUE(restarted->publish(kTestType, payload_for(2)).has_value());

    EXPECT_EQ(poll_value(*reader_holder), 1u);
    EXPECT_EQ(poll_value(*reader_holder), 2u);
}

TEST_F(BusTest, NewWriterTakesOverFromCrashedWriter) {
    // The child becomes the writer and exits without running destructors, leaving its
    // (now dead) pid in the segment — exactly what a crash looks like.
    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        auto writer = Writer::create_or_attach(config());
        if (writer) {
            (void)writer->publish(kTestType, payload_for(7));
        }
        ::_exit(writer ? 0 : 1);
    }
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);  // reaped: the pid is now dead
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    auto reader = Reader::attach(name_);
    ASSERT_TRUE(reader.has_value());
    EXPECT_EQ(reader->writer_status().pid, static_cast<std::uint32_t>(child));

    auto writer = Writer::create_or_attach(config());
    ASSERT_TRUE(writer.has_value()) << to_string(writer.error());
    EXPECT_EQ(writer->epoch(), 2u);
    EXPECT_EQ(reader->writer_status().pid, static_cast<std::uint32_t>(::getpid()));

    ASSERT_TRUE(writer->publish(kTestType, payload_for(8)).has_value());
    EXPECT_EQ(poll_value(*reader), 8u);
}

// --------------------------------------------------------------------------- validation

TEST_F(BusTest, WriterRefusesExistingBusWithDifferentCapacity) {
    {
        ASSERT_TRUE(Writer::create_or_attach(config(kCapacity)).has_value());
    }
    auto writer = Writer::create_or_attach(config(kCapacity * 2));
    ASSERT_FALSE(writer.has_value());
    EXPECT_EQ(writer.error().code, BusErrc::CapacityMismatch);
}

TEST_F(BusTest, UninitializedSegmentIsNotReady) {
    // A zero-filled segment: what a reader sees if the creator crashed mid-initialization.
    auto memory = SharedMemory::create(segment_name(name_), 128 * 1024);
    ASSERT_TRUE(memory.has_value());
    EXPECT_EQ(Reader::attach(name_).error().code, BusErrc::NotReady);
    EXPECT_EQ(Writer::create_or_attach(config()).error().code, BusErrc::NotReady);
}

TEST_F(BusTest, ForeignSegmentIsLayoutMismatch) {
    auto memory = SharedMemory::create(segment_name(name_), 128 * 1024);
    ASSERT_TRUE(memory.has_value());
    std::memset(memory->bytes().data(), 0xAB, 64);  // not our magic
    EXPECT_EQ(Reader::attach(name_).error().code, BusErrc::LayoutMismatch);
    EXPECT_EQ(Writer::create_or_attach(config()).error().code, BusErrc::LayoutMismatch);
}

TEST_F(BusTest, RemoveBusDeletesTheSegment) {
    {
        ASSERT_TRUE(Writer::create_or_attach(config()).has_value());
    }
    ASSERT_TRUE(remove_bus(name_).has_value());
    EXPECT_EQ(Reader::attach(name_).error().code, BusErrc::NotFound);
    EXPECT_EQ(remove_bus(name_).error().code, BusErrc::NotFound);
}

TEST_F(BusTest, ProcessesAttachedBeforeRemovalKeepWorking) {
    auto writer = Writer::create_or_attach(config());
    ASSERT_TRUE(writer.has_value());
    auto reader = Reader::attach(name_);
    ASSERT_TRUE(reader.has_value());

    ASSERT_TRUE(remove_bus(name_).has_value());
    ASSERT_TRUE(writer->publish(kTestType, payload_for(5)).has_value());
    EXPECT_EQ(poll_value(*reader), 5u);
}

// --------------------------------------------------------------------------- stats and slots

// An 8-byte payload occupies a 16-byte record (8-byte header + payload).
constexpr std::uint64_t kRecordBytes = 16;

BusSnapshot inspect(const std::string& name) {
    auto snapshot = inspect_bus(name);
    EXPECT_TRUE(snapshot.has_value()) << to_string(snapshot.error());
    return snapshot.value_or(BusSnapshot{});
}

TEST_F(BusTest, WriterStatsCountPublishesAndRejections) {
    auto writer = Writer::create_or_attach(config());
    ASSERT_TRUE(writer.has_value());
    for (std::uint64_t value = 0; value < 5; ++value) {
        ASSERT_TRUE(writer->publish(kTestType, payload_for(value)).has_value());
    }
    const std::vector<std::byte> oversized(writer->max_message_length() + 1);
    EXPECT_FALSE(writer->publish(kTestType, oversized).has_value());
    EXPECT_FALSE(writer->publish(kPaddingMsgType, payload_for(0)).has_value());

    const WriterStats expected{.messages = 5, .bytes = 40, .rejected = 2};
    EXPECT_EQ(writer->stats().messages, expected.messages);
    EXPECT_EQ(writer->stats().bytes, expected.bytes);
    EXPECT_EQ(writer->stats().rejected, expected.rejected);

    const BusSnapshot snapshot = inspect(name_);
    EXPECT_EQ(snapshot.name, name_);
    EXPECT_EQ(snapshot.capacity, kCapacity);
    EXPECT_TRUE(snapshot.writer_alive);
    EXPECT_EQ(snapshot.writer_stats.messages, expected.messages);
    EXPECT_EQ(snapshot.writer_stats.bytes, expected.bytes);
    EXPECT_EQ(snapshot.writer_stats.rejected, expected.rejected);
    EXPECT_EQ(snapshot.tail, 5 * kRecordBytes);
}

TEST_F(BusTest, WriterStatsContinueAcrossWriterRestart) {
    {
        auto writer = Writer::create_or_attach(config());
        ASSERT_TRUE(writer.has_value());
        for (std::uint64_t value = 0; value < 3; ++value) {
            ASSERT_TRUE(writer->publish(kTestType, payload_for(value)).has_value());
        }
    }
    auto restarted = Writer::create_or_attach(config());
    ASSERT_TRUE(restarted.has_value());
    ASSERT_TRUE(restarted->publish(kTestType, payload_for(3)).has_value());
    EXPECT_EQ(restarted->stats().messages, 4u);
    EXPECT_EQ(inspect(name_).writer_stats.messages, 4u);
}

TEST_F(BusTest, RegisteredReaderShowsLagAndCounts) {
    auto writer = Writer::create_or_attach(config());
    ASSERT_TRUE(writer.has_value());
    auto reader = Reader::attach(name_, {.name = "engine"});
    ASSERT_TRUE(reader.has_value());
    ASSERT_TRUE(reader->registered());

    for (std::uint64_t value = 0; value < 10; ++value) {
        ASSERT_TRUE(writer->publish(kTestType, payload_for(value)).has_value());
    }
    for (std::uint64_t value = 0; value < 4; ++value) {
        EXPECT_EQ(poll_value(*reader), value);
    }

    const BusSnapshot snapshot = inspect(name_);
    ASSERT_EQ(snapshot.readers.size(), 1u);
    const ReaderInfo& info = snapshot.readers.front();
    EXPECT_EQ(info.name, "engine");
    EXPECT_EQ(info.pid, static_cast<std::uint32_t>(::getpid()));
    EXPECT_TRUE(info.process_alive);
    EXPECT_EQ(info.stats.messages, 4u);
    EXPECT_EQ(info.position, 4 * kRecordBytes);
    EXPECT_EQ(info.lag_bytes, 6 * kRecordBytes);
    EXPECT_EQ(reader->stats().messages, 4u);
}

TEST_F(BusTest, LapsAndLossesAreReported) {
    auto writer = Writer::create_or_attach(config());
    ASSERT_TRUE(writer.has_value());
    auto reader = Reader::attach(name_, {.name = "slow"});
    ASSERT_TRUE(reader.has_value());

    // Twice the ring's worth of records while the reader is idle.
    for (std::uint64_t value = 0; value < 2 * kCapacity / kRecordBytes; ++value) {
        ASSERT_TRUE(writer->publish(kTestType, payload_for(value)).has_value());
    }
    EXPECT_EQ(reader->poll([](auto, auto) {}).status, PollStatus::Lapped);

    const BusSnapshot snapshot = inspect(name_);
    ASSERT_EQ(snapshot.readers.size(), 1u);
    EXPECT_EQ(snapshot.readers.front().stats.laps, 1u);
    EXPECT_GT(snapshot.readers.front().stats.lost_bytes, kCapacity);
    EXPECT_EQ(reader->stats().laps, 1u);
}

TEST_F(BusTest, OptedOutReaderIsInvisibleButReceives) {
    auto writer = Writer::create_or_attach(config());
    ASSERT_TRUE(writer.has_value());
    auto reader = Reader::attach(name_, {.name = "debug", .register_slot = false});
    ASSERT_TRUE(reader.has_value());
    EXPECT_FALSE(reader->registered());

    ASSERT_TRUE(writer->publish(kTestType, payload_for(1)).has_value());
    EXPECT_EQ(poll_value(*reader), 1u);
    EXPECT_TRUE(inspect(name_).readers.empty());
}

TEST_F(BusTest, ReaderStillAttachesWhenAllSlotsAreTaken) {
    auto writer = Writer::create_or_attach(config());
    ASSERT_TRUE(writer.has_value());
    std::vector<Reader> readers;
    for (int i = 0; i < 16; ++i) {
        auto reader = Reader::attach(name_, {.name = "r" + std::to_string(i)});
        ASSERT_TRUE(reader.has_value() && reader->registered());
        readers.push_back(std::move(*reader));
    }

    auto extra = Reader::attach(name_, {.name = "extra"});
    ASSERT_TRUE(extra.has_value());
    EXPECT_FALSE(extra->registered());
    ASSERT_TRUE(writer->publish(kTestType, payload_for(9)).has_value());
    EXPECT_EQ(poll_value(*extra), 9u);
    EXPECT_EQ(inspect(name_).readers.size(), 16u);
}

TEST_F(BusTest, DetachingFreesTheSlot) {
    auto writer = Writer::create_or_attach(config());
    ASSERT_TRUE(writer.has_value());
    {
        auto reader = Reader::attach(name_, {.name = "short-lived"});
        ASSERT_TRUE(reader.has_value() && reader->registered());
        EXPECT_EQ(inspect(name_).readers.size(), 1u);
    }
    EXPECT_TRUE(inspect(name_).readers.empty());
}

TEST_F(BusTest, SlotOfCrashedReaderIsReclaimed) {
    auto writer = Writer::create_or_attach(config());
    ASSERT_TRUE(writer.has_value());

    // The child registers a reader and exits without cleanup: a crashed reader.
    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        auto reader = Reader::attach(name_, {.name = "crashed"});
        ::_exit(reader && reader->registered() ? 0 : 1);
    }
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    BusSnapshot snapshot = inspect(name_);
    ASSERT_EQ(snapshot.readers.size(), 1u);
    EXPECT_EQ(snapshot.readers.front().name, "crashed");
    EXPECT_FALSE(snapshot.readers.front().process_alive);

    // Fill the 15 free slots; the 16th reader must reclaim the dead reader's slot.
    std::vector<Reader> readers;
    for (int i = 0; i < 16; ++i) {
        auto reader = Reader::attach(name_, {.name = "r" + std::to_string(i)});
        ASSERT_TRUE(reader.has_value() && reader->registered()) << "reader " << i;
        readers.push_back(std::move(*reader));
    }
    snapshot = inspect(name_);
    EXPECT_EQ(snapshot.readers.size(), 16u);
    EXPECT_TRUE(std::ranges::all_of(snapshot.readers, [](const ReaderInfo& r) {
        return r.process_alive && r.name != "crashed";
    }));
}

TEST_F(BusTest, LongReaderNamesAreTruncated) {
    auto writer = Writer::create_or_attach(config());
    ASSERT_TRUE(writer.has_value());
    auto reader = Reader::attach(name_, {.name = std::string(40, 'x')});
    ASSERT_TRUE(reader.has_value());
    EXPECT_EQ(inspect(name_).readers.front().name, std::string(kMaxReaderNameLength, 'x'));
}

TEST_F(BusTest, InspectingMissingBusIsNotFound) {
    EXPECT_EQ(inspect_bus(name_).error().code, BusErrc::NotFound);
}

}  // namespace
}  // namespace mda::bus

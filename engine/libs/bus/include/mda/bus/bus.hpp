#pragma once

// Public API of the market data bus: a broadcast ring in a POSIX shared-memory
// segment (ADR-0007, docs/design/market-data-bus.md).
//
//   auto writer = Writer::create_or_attach({.name = "md", .capacity = 256 << 20});
//   auto reader = Reader::attach("md", {.name = "engine"});   // in another process
//   writer->publish(type, bytes);
//   reader->poll([](MsgType type, std::span<const std::byte> payload) { ... });

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <mda/bus/error.hpp>
#include <mda/bus/ring.hpp>
#include <mda/bus/ring_reader.hpp>
#include <mda/bus/ring_writer.hpp>
#include <mda/bus/shared_memory.hpp>

namespace mda::bus {

namespace detail {
struct SegmentHeader;
struct ReaderSlot;
}  // namespace detail

inline constexpr std::size_t kMaxBusNameLength = 20;
inline constexpr std::size_t kMaxReaderNameLength = 24;

struct BusConfig {
    std::string name;                          ///< [a-z0-9_-], 1..20 characters
    std::size_t capacity = 256 * 1024 * 1024;  ///< ring bytes, power of two
};

struct ReaderOptions {
    std::string name = "reader";  ///< shown by monitoring tools; truncated to 24 characters
    bool register_slot = true;    ///< false: attach invisibly (debug tools)
};

/// Writer identity and liveness as recorded in the segment.
struct WriterStatus {
    std::uint32_t pid = 0;           ///< 0 when no writer is attached
    std::uint64_t epoch = 0;         ///< number of writer takeovers since creation
    std::uint64_t heartbeat_ns = 0;  ///< system clock, ns since the Unix epoch
};

struct WriterStats {
    std::uint64_t messages = 0;
    std::uint64_t bytes = 0;     ///< payload bytes
    std::uint64_t rejected = 0;  ///< publishes refused (too large / reserved type)
};

struct ReaderStats {
    std::uint64_t messages = 0;
    std::uint64_t laps = 0;
    std::uint64_t lost_bytes = 0;
};

/// POSIX shared-memory object name for a bus ("/mda.<name>").
[[nodiscard]] std::string segment_name(std::string_view bus_name);

/// Removes a bus segment. Processes that have it mapped keep working on the old memory.
[[nodiscard]] std::expected<void, BusError> remove_bus(std::string_view bus_name);

/// The single writer of a bus.
///
/// Creates the segment if it does not exist, otherwise attaches and continues from the
/// published tail (writer restart). Refuses if another live process is the writer; takes
/// over (incrementing the epoch) if the previous writer died.
class Writer {
public:
    [[nodiscard]] static std::expected<Writer, BusError> create_or_attach(const BusConfig& config);

    Writer(Writer&& other) noexcept;
    Writer& operator=(Writer&&) = delete;
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;
    /// Releases writer ownership; the segment and its data remain for readers.
    ~Writer();

    /// Never blocks. See RingWriter::publish. Updates the writer stats in the segment.
    std::expected<void, PublishError> publish(MsgType type,
                                              std::span<const std::byte> payload) noexcept;

    /// Records liveness; call periodically (e.g. every 100 ms) from the writer's loop.
    void heartbeat() noexcept;

    [[nodiscard]] WriterStats stats() const noexcept { return stats_; }
    [[nodiscard]] std::uint64_t epoch() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept { return ring_.capacity(); }
    [[nodiscard]] std::size_t max_message_length() const noexcept {
        return ring_.max_message_length();
    }

private:
    Writer(SharedMemory memory, detail::SegmentHeader* header, RingWriter ring) noexcept;

    SharedMemory memory_;  // declared first: destroyed last, after users of the mapping
    detail::SegmentHeader* header_;
    RingWriter ring_;
    WriterStats stats_;  // local copy; mirrored into the segment on every publish
};

/// One reader of a bus. Starts at the current tail (only new messages are delivered).
///
/// By default the reader registers in one of the segment's 16 reader slots so tools can
/// show its lag and losses. If all slots are taken it still attaches, unregistered.
class Reader {
public:
    [[nodiscard]] static std::expected<Reader, BusError> attach(std::string_view bus_name,
                                                                const ReaderOptions& options = {});

    Reader(Reader&& other) noexcept;
    Reader& operator=(Reader&&) = delete;
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    /// Frees the reader slot.
    ~Reader();

    /// Delivers at most one message. Never blocks. See RingReader::poll.
    template <MessageHandler H>
    PollResult poll(H&& handler) noexcept(noexcept(handler(MsgType{},
                                                           std::span<const std::byte>{}))) {
        const PollResult result = ring_.poll(handler);
        if (result.status != PollStatus::Empty) {
            record(result);  // Empty polls (the common idle case) touch nothing shared
        }
        return result;
    }

    /// Records liveness in the reader slot; call periodically from the reader's loop.
    void heartbeat() noexcept;

    [[nodiscard]] bool registered() const noexcept { return slot_ != nullptr; }
    [[nodiscard]] ReaderStats stats() const noexcept { return stats_; }
    [[nodiscard]] WriterStatus writer_status() const noexcept;
    [[nodiscard]] std::uint64_t position() const noexcept { return ring_.position(); }

private:
    Reader(SharedMemory memory, detail::SegmentHeader* header, RingReader ring,
           detail::ReaderSlot* slot) noexcept;
    void record(PollResult result) noexcept;

    SharedMemory memory_;
    detail::SegmentHeader* header_;
    RingReader ring_;
    detail::ReaderSlot* slot_;  // null when unregistered
    ReaderStats stats_;
};

// --------------------------------------------------------------------------- monitoring

struct ReaderInfo {
    std::size_t slot = 0;
    std::uint32_t pid = 0;
    bool process_alive = false;
    std::string name;
    std::uint64_t position = 0;
    std::uint64_t lag_bytes = 0;  ///< writer tail - reader position
    ReaderStats stats;
    std::uint64_t heartbeat_ns = 0;
};

/// A point-in-time view of a bus for monitoring. Counters are read individually, so
/// they may be a few messages apart from each other.
struct BusSnapshot {
    std::string name;
    std::uint64_t capacity = 0;
    std::uint64_t created_ns = 0;
    WriterStatus writer;
    bool writer_alive = false;
    WriterStats writer_stats;
    std::uint64_t tail = 0;
    std::vector<ReaderInfo> readers;  ///< registered readers only
};

/// Maps the bus read-only and reads its header, stats and reader slots. Never
/// writes to the segment, so it can't disturb the processes using it.
[[nodiscard]] std::expected<BusSnapshot, BusError> inspect_bus(std::string_view bus_name);

/// Bus names currently present. Linux only (/dev/shm); empty on macOS, which has no
/// way to enumerate POSIX shared-memory objects.
[[nodiscard]] std::vector<std::string> list_buses();

}  // namespace mda::bus

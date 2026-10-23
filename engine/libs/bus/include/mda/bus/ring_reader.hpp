#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include <mda/bus/ring.hpp>

namespace mda::bus {

enum class PollStatus {
    Message,  ///< one message was delivered to the handler
    Empty,    ///< nothing new published
    Lapped,   ///< the writer overwrote unread records; reader resumed at the latest record
};

struct PollResult {
    PollStatus status;
    /// For Lapped: bytes of records skipped (an upper bound on what was lost).
    std::uint64_t lost_bytes = 0;
};

template <typename H>
concept MessageHandler = std::invocable<H&, MsgType, std::span<const std::byte>>;

/// One of any number of independent readers of a broadcast ring.
///
/// Readers keep their position privately; the writer never knows they exist and never
/// waits for them. A reader that falls a full ring behind is told so (PollStatus::Lapped)
/// and continues from the latest record — recovery of the lost data is the caller's job.
class RingReader {
public:
    /// Attaches at the current tail: delivers only messages published after this call.
    [[nodiscard]] static std::expected<RingReader, RingError> create(RingCounters& counters,
                                                                     std::span<std::byte> ring);

    /// Delivers at most one message to `handler(type, payload)`. Never blocks.
    /// `payload` is a private copy, valid only during the call.
    template <MessageHandler H>
    PollResult poll(H&& handler) noexcept(noexcept(handler(MsgType{},
                                                           std::span<const std::byte>{}))) {
        const ReadResult result = read_next();
        if (result.status == PollStatus::Message) {
            handler(result.type, result.payload);
        }
        return {result.status, result.lost_bytes};
    }

    /// Byte position of the next record to read.
    [[nodiscard]] std::uint64_t position() const noexcept { return cursor_; }

private:
    struct ReadResult {
        PollStatus status;
        MsgType type = 0;
        std::span<const std::byte> payload;
        std::uint64_t lost_bytes = 0;
    };

    RingReader(RingCounters& counters, std::span<std::byte> ring);
    ReadResult read_next() noexcept;
    [[nodiscard]] bool lapped(std::uint64_t position) const noexcept;
    ReadResult resync_to_latest() noexcept;

    RingCounters* counters_;
    std::byte* ring_;
    std::size_t capacity_;
    std::uint64_t mask_;
    std::size_t max_message_length_;
    std::uint64_t cursor_;
    std::vector<std::uint64_t> scratch_;  // private copy of the current payload
};

}  // namespace mda::bus

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

#include <mda/bus/ring.hpp>

namespace mda::bus {

enum class PublishError {
    MessageTooLarge,  ///< payload longer than max_message_length()
    ReservedMsgType,  ///< type 0 is reserved for padding
};

/// The single writer of a broadcast ring. Never blocks and never waits for readers.
///
/// Exactly one RingWriter may exist per ring at a time (single-writer principle). A new
/// writer attached to existing counters continues from the published tail, so a
/// restarted writer process is transparent to readers.
class RingWriter {
public:
    [[nodiscard]] static std::expected<RingWriter, RingError> create(
        RingCounters& counters, std::span<std::byte> ring) noexcept;

    /// Copies `payload` into the ring and publishes it to all readers.
    std::expected<void, PublishError> publish(MsgType type,
                                              std::span<const std::byte> payload) noexcept;

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t max_message_length() const noexcept { return max_message_length_; }
    /// Byte position up to which records are published.
    [[nodiscard]] std::uint64_t position() const noexcept { return tail_; }

private:
    RingWriter(RingCounters& counters, std::span<std::byte> ring) noexcept;

    RingCounters* counters_;
    std::byte* ring_;
    std::size_t capacity_;
    std::uint64_t mask_;
    std::size_t max_message_length_;
    std::uint64_t tail_;  // single writer: local copy of counters_->tail
};

}  // namespace mda::bus

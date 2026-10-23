#include <atomic>

#include <mda/bus/ring_writer.hpp>

#include "ring_access.hpp"

namespace mda::bus {

std::expected<RingWriter, RingError> RingWriter::create(RingCounters& counters,
                                                        std::span<std::byte> ring) noexcept {
    if (auto valid = validate_ring(ring); !valid) {
        return std::unexpected(valid.error());
    }
    return RingWriter(counters, ring);
}

RingWriter::RingWriter(RingCounters& counters, std::span<std::byte> ring) noexcept
    : counters_(&counters)
    , ring_(ring.data())
    , capacity_(ring.size())
    , mask_(ring.size() - 1)
    , max_message_length_(mda::bus::max_message_length(ring.size()))
    , tail_(counters.tail.load(std::memory_order_acquire)) {}

std::expected<void, PublishError> RingWriter::publish(MsgType type,
                                                      std::span<const std::byte> payload) noexcept {
    if (type == kPaddingMsgType) {
        return std::unexpected(PublishError::ReservedMsgType);
    }
    if (payload.size() > max_message_length_) {
        return std::unexpected(PublishError::MessageTooLarge);
    }

    std::uint64_t tail = tail_;
    auto offset = static_cast<std::size_t>(tail & mask_);
    const std::uint64_t record_length = align_record(kRecordHeaderSize + payload.size());
    const std::uint64_t to_end = capacity_ - offset;

    if (to_end < record_length) {
        // Not enough room before the end: pad to the end and start at offset 0.
        // Records never wrap, so readers can always copy them in one piece.
        counters_->tail_intent.store(tail + to_end + record_length, std::memory_order_relaxed);
        // Readers must observe the new intent before any byte we overwrite next
        // (pairs with the acquire fence in RingReader::read_next).
        std::atomic_thread_fence(std::memory_order_release);
        detail::store_header(
            ring_, offset,
            {static_cast<std::uint32_t>(to_end - kRecordHeaderSize), kPaddingMsgType});
        tail += to_end;
        offset = 0;
    } else {
        counters_->tail_intent.store(tail + record_length, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
    }

    detail::store_header(ring_, offset, {static_cast<std::uint32_t>(payload.size()), type});
    detail::store_payload(ring_, offset + kRecordHeaderSize, payload);

    counters_->latest.store(tail, std::memory_order_release);
    tail_ = tail + record_length;
    counters_->tail.store(tail_, std::memory_order_release);  // publish
    return {};
}

}  // namespace mda::bus

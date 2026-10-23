#include <algorithm>
#include <atomic>
#include <cassert>

#include <mda/bus/ring_reader.hpp>

#include "ring_access.hpp"

namespace mda::bus {

std::expected<RingReader, RingError> RingReader::create(RingCounters& counters,
                                                        std::span<std::byte> ring) {
    if (auto valid = validate_ring(ring); !valid) {
        return std::unexpected(valid.error());
    }
    return RingReader(counters, ring);
}

RingReader::RingReader(RingCounters& counters, std::span<std::byte> ring)
    : counters_(&counters)
    , ring_(ring.data())
    , capacity_(ring.size())
    , mask_(ring.size() - 1)
    , max_message_length_(mda::bus::max_message_length(ring.size()))
    , cursor_(counters.tail.load(std::memory_order_acquire))
    , scratch_((max_message_length_ + 7) / 8) {}

bool RingReader::lapped(std::uint64_t position) const noexcept {
    // The writer is about to overwrite everything below tail_intent - capacity.
    return counters_->tail_intent.load(std::memory_order_acquire) > position + capacity_;
}

RingReader::ReadResult RingReader::resync_to_latest() noexcept {
    const std::uint64_t latest = counters_->latest.load(std::memory_order_acquire);
    const std::uint64_t lost = latest > cursor_ ? latest - cursor_ : 0;
    cursor_ = std::max(cursor_, latest);
    return {.status = PollStatus::Lapped, .lost_bytes = lost};
}

RingReader::ReadResult RingReader::read_next() noexcept {
    const std::uint64_t tail = counters_->tail.load(std::memory_order_acquire);
    if (cursor_ >= tail) {
        return {.status = PollStatus::Empty};
    }
    if (lapped(cursor_)) {
        return resync_to_latest();
    }

    // Everything read below may be overwritten concurrently, so nothing is trusted
    // until the post-copy validation. Bounds checks only keep a torn read in range.
    std::uint64_t position = cursor_;
    auto offset = static_cast<std::size_t>(position & mask_);
    detail::RecordHeader header = detail::load_header(ring_, offset);
    if (header.type == kPaddingMsgType) {
        // Padding always runs to the end of the ring; don't rely on its length field.
        position += capacity_ - offset;
        offset = 0;
        header = detail::load_header(ring_, offset);
    }

    const std::uint64_t record_length = align_record(kRecordHeaderSize + header.length);
    const bool in_bounds =
        header.length <= max_message_length_ && offset + record_length <= capacity_;
    if (in_bounds) {
        detail::load_payload(ring_, offset + kRecordHeaderSize, header.length, scratch_.data());
    }

    // Pairs with the writer's release fence: if any word we copied was overwritten, we
    // are guaranteed to see the tail intent that preceded the overwrite.
    std::atomic_thread_fence(std::memory_order_acquire);
    if (lapped(cursor_)) {
        return resync_to_latest();
    }
    if (!in_bounds) {
        // An intact header can't be out of bounds; only a writer bug or memory
        // corruption gets here. Skip ahead rather than read garbage.
        assert(false && "corrupt record header in ring");
        return resync_to_latest();
    }

    cursor_ = position + record_length;
    return {.status = PollStatus::Message,
            .type = header.type,
            .payload = std::as_bytes(std::span(scratch_)).first(header.length)};
}

}  // namespace mda::bus

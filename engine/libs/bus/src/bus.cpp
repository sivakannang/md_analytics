#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <new>
#include <utility>

#ifdef __linux__
#include <filesystem>
#endif

#include <signal.h>
#include <unistd.h>

#include <mda/bus/bus.hpp>

#include "segment_layout.hpp"

namespace mda::bus {
namespace {

using detail::ReaderSlot;
using detail::SegmentHeader;

constexpr auto kRelaxed = std::memory_order_relaxed;

std::uint64_t now_ns() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

std::uint32_t self_pid() noexcept {
    return static_cast<std::uint32_t>(::getpid());
}

bool process_alive(std::uint32_t pid) noexcept {
    if (pid == 0) {
        return false;  // kill(0, ...) would target our own process group
    }
    // Signal 0 checks existence without sending anything. EPERM: exists, not ours.
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
}

bool valid_name(std::string_view name) noexcept {
    return !name.empty() && name.size() <= kMaxBusNameLength &&
           std::ranges::all_of(name, [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
           });
}

bool valid_capacity(std::size_t capacity) noexcept {
    return std::has_single_bit(capacity) && capacity >= kMinCapacity;
}

std::span<std::byte> ring_bytes(const SharedMemory& memory, const SegmentHeader& header) {
    return memory.bytes().subspan(header.ring_offset, header.capacity);
}

/// Checks that a mapped segment was fully initialized by a compatible build.
std::expected<SegmentHeader*, BusError> validate(const SharedMemory& memory) {
    if (memory.bytes().size() < sizeof(SegmentHeader)) {
        return std::unexpected(BusError{BusErrc::NotReady});
    }
    auto* header = reinterpret_cast<SegmentHeader*>(memory.bytes().data());
    const std::uint64_t magic = header->magic.load(std::memory_order_acquire);
    if (magic == 0) {
        return std::unexpected(BusError{BusErrc::NotReady});
    }
    if (magic != detail::kMagic || header->layout_version != detail::kLayoutVersion ||
        header->ring_offset != detail::kRingOffset || !valid_capacity(header->capacity) ||
        header->ring_offset + header->capacity > memory.bytes().size()) {
        return std::unexpected(BusError{BusErrc::LayoutMismatch});
    }
    return header;
}

/// Initializes a freshly created (zero-filled) segment and makes this process its writer.
SegmentHeader* initialize(const SharedMemory& memory, const BusConfig& config) {
    auto* header = new (memory.bytes().data()) SegmentHeader{};
    header->layout_version = detail::kLayoutVersion;
    header->ring_offset = detail::kRingOffset;
    header->capacity = config.capacity;
    header->created_ns = now_ns();
    std::memcpy(header->bus_name, config.name.data(), config.name.size());
    header->writer_pid.store(self_pid(), kRelaxed);
    header->writer_epoch.store(1, kRelaxed);
    header->heartbeat_ns.store(now_ns(), kRelaxed);
    // Publish: everything above is visible to anyone who sees the magic.
    header->magic.store(detail::kMagic, std::memory_order_release);
    return header;
}

/// Becomes the writer of an existing segment, unless a live process already is.
std::expected<void, BusError> claim_writer(SegmentHeader& header) {
    const std::uint32_t self = self_pid();
    std::uint32_t current = header.writer_pid.load(std::memory_order_acquire);
    while (true) {
        if (process_alive(current)) {
            return std::unexpected(BusError{BusErrc::WriterAlreadyActive});
        }
        // Free, or the previous writer died: take over.
        if (header.writer_pid.compare_exchange_weak(current, self, std::memory_order_acq_rel)) {
            break;
        }
    }
    header.writer_epoch.fetch_add(1, std::memory_order_acq_rel);
    header.heartbeat_ns.store(now_ns(), std::memory_order_release);
    return {};
}

// --------------------------------------------------------------------------- reader slots

void store_slot_name(ReaderSlot& slot, std::string_view name) noexcept {
    std::array<char, detail::kReaderNameCapacity> chars{};
    std::memcpy(chars.data(), name.data(), std::min(name.size(), chars.size()));
    for (std::size_t i = 0; i < detail::kReaderNameWords; ++i) {
        std::uint64_t word = 0;
        std::memcpy(&word, chars.data() + i * 8, 8);
        slot.name[i].store(word, kRelaxed);
    }
}

std::string load_slot_name(const ReaderSlot& slot) {
    std::array<char, detail::kReaderNameCapacity> chars{};
    for (std::size_t i = 0; i < detail::kReaderNameWords; ++i) {
        const std::uint64_t word = slot.name[i].load(kRelaxed);
        std::memcpy(chars.data() + i * 8, &word, 8);
    }
    return {chars.data(), ::strnlen(chars.data(), chars.size())};
}

/// Fills a slot this process has just taken ownership of.
ReaderSlot* initialize_slot(ReaderSlot& slot, std::string_view name, std::uint64_t position) {
    slot.active.store(0, std::memory_order_release);  // fields being rewritten
    store_slot_name(slot, name);
    slot.position.store(position, kRelaxed);
    slot.messages.store(0, kRelaxed);
    slot.laps.store(0, kRelaxed);
    slot.lost_bytes.store(0, kRelaxed);
    slot.heartbeat_ns.store(now_ns(), kRelaxed);
    slot.active.store(1, std::memory_order_release);
    return &slot;
}

/// Claims a free slot, else reclaims one whose process has died. Null if none.
ReaderSlot* claim_slot(SegmentHeader& header, std::string_view name, std::uint64_t position) {
    const std::uint32_t self = self_pid();
    for (ReaderSlot& slot : header.readers) {
        std::uint32_t expected = 0;
        if (slot.pid.compare_exchange_strong(expected, self, std::memory_order_acq_rel)) {
            return initialize_slot(slot, name, position);
        }
    }
    for (ReaderSlot& slot : header.readers) {
        std::uint32_t owner = slot.pid.load(std::memory_order_acquire);
        if (owner != 0 && !process_alive(owner) &&
            slot.pid.compare_exchange_strong(owner, self, std::memory_order_acq_rel)) {
            return initialize_slot(slot, name, position);
        }
    }
    return nullptr;
}

}  // namespace

std::string segment_name(std::string_view bus_name) {
    return "/mda." + std::string(bus_name);
}

std::expected<void, BusError> remove_bus(std::string_view bus_name) {
    if (!valid_name(bus_name)) {
        return std::unexpected(BusError{BusErrc::InvalidName});
    }
    return SharedMemory::unlink(segment_name(bus_name));
}

// --------------------------------------------------------------------------- Writer

std::expected<Writer, BusError> Writer::create_or_attach(const BusConfig& config) {
    if (!valid_name(config.name)) {
        return std::unexpected(BusError{BusErrc::InvalidName});
    }
    if (!valid_capacity(config.capacity)) {
        return std::unexpected(BusError{BusErrc::InvalidCapacity});
    }
    const std::string name = segment_name(config.name);

    SegmentHeader* header = nullptr;
    auto memory = SharedMemory::create(name, detail::kRingOffset + config.capacity);
    if (memory) {
        header = initialize(*memory, config);
    } else if (memory.error().code == BusErrc::AlreadyExists) {
        memory = SharedMemory::open(name);
        if (!memory) {
            return std::unexpected(memory.error());
        }
        auto validated = validate(*memory);
        if (!validated) {
            return std::unexpected(validated.error());
        }
        header = *validated;
        if (header->capacity != config.capacity) {
            return std::unexpected(BusError{BusErrc::CapacityMismatch});
        }
        if (auto claimed = claim_writer(*header); !claimed) {
            return std::unexpected(claimed.error());
        }
        memory->prefault(SharedMemory::Prefault::Read);  // live data: never write-touch
    } else {
        return std::unexpected(memory.error());
    }

    auto ring = RingWriter::create(header->counters, ring_bytes(*memory, *header));
    if (!ring) {
        return std::unexpected(BusError{BusErrc::LayoutMismatch});
    }
    return Writer(std::move(*memory), header, *ring);
}

Writer::Writer(SharedMemory memory, detail::SegmentHeader* header, RingWriter ring) noexcept
    : memory_(std::move(memory))
    , header_(header)
    , ring_(ring)
    // A restarted writer continues the counts of its predecessor.
    , stats_{.messages = header->writer_stats.messages.load(kRelaxed),
             .bytes = header->writer_stats.bytes.load(kRelaxed),
             .rejected = header->writer_stats.rejected.load(kRelaxed)} {}

Writer::Writer(Writer&& other) noexcept
    : memory_(std::move(other.memory_))
    , header_(std::exchange(other.header_, nullptr))
    , ring_(other.ring_)
    , stats_(other.stats_) {}

Writer::~Writer() {
    if (header_ != nullptr) {
        // Release ownership only if it is still ours.
        std::uint32_t self = self_pid();
        header_->writer_pid.compare_exchange_strong(self, 0, std::memory_order_acq_rel);
    }
}

std::expected<void, PublishError> Writer::publish(MsgType type,
                                                  std::span<const std::byte> payload) noexcept {
    auto result = ring_.publish(type, payload);
    // Single writer: plain stores of locally kept totals, no atomic read-modify-write.
    auto& shared = header_->writer_stats;
    if (result) {
        ++stats_.messages;
        stats_.bytes += payload.size();
        shared.messages.store(stats_.messages, kRelaxed);
        shared.bytes.store(stats_.bytes, kRelaxed);
    } else {
        ++stats_.rejected;
        shared.rejected.store(stats_.rejected, kRelaxed);
    }
    return result;
}

void Writer::heartbeat() noexcept {
    header_->heartbeat_ns.store(now_ns(), std::memory_order_release);
}

std::uint64_t Writer::epoch() const noexcept {
    return header_->writer_epoch.load(std::memory_order_acquire);
}

// --------------------------------------------------------------------------- Reader

std::expected<Reader, BusError> Reader::attach(std::string_view bus_name,
                                               const ReaderOptions& options) {
    if (!valid_name(bus_name)) {
        return std::unexpected(BusError{BusErrc::InvalidName});
    }
    auto memory = SharedMemory::open(segment_name(bus_name));
    if (!memory) {
        return std::unexpected(memory.error());
    }
    auto header = validate(*memory);
    if (!header) {
        return std::unexpected(header.error());
    }
    memory->prefault(SharedMemory::Prefault::Read);

    auto ring = RingReader::create((*header)->counters, ring_bytes(*memory, **header));
    if (!ring) {
        return std::unexpected(BusError{BusErrc::LayoutMismatch});
    }
    ReaderSlot* slot =
        options.register_slot ? claim_slot(**header, options.name, ring->position()) : nullptr;
    return Reader(std::move(*memory), *header, std::move(*ring), slot);
}

Reader::Reader(SharedMemory memory, detail::SegmentHeader* header, RingReader ring,
               detail::ReaderSlot* slot) noexcept
    : memory_(std::move(memory)), header_(header), ring_(std::move(ring)), slot_(slot) {}

Reader::Reader(Reader&& other) noexcept
    : memory_(std::move(other.memory_))
    , header_(std::exchange(other.header_, nullptr))
    , ring_(std::move(other.ring_))
    , slot_(std::exchange(other.slot_, nullptr))
    , stats_(other.stats_) {}

Reader::~Reader() {
    if (slot_ != nullptr) {
        slot_->active.store(0, std::memory_order_release);
        std::uint32_t self = self_pid();
        slot_->pid.compare_exchange_strong(self, 0, std::memory_order_acq_rel);
    }
}

void Reader::record(PollResult result) noexcept {
    if (result.status == PollStatus::Message) {
        ++stats_.messages;
    } else {
        ++stats_.laps;
        stats_.lost_bytes += result.lost_bytes;
    }
    if (slot_ == nullptr) {
        return;
    }
    // Owner-only fields on the reader's own cache line: relaxed stores, no contention.
    slot_->position.store(ring_.position(), kRelaxed);
    if (result.status == PollStatus::Message) {
        slot_->messages.store(stats_.messages, kRelaxed);
    } else {
        slot_->laps.store(stats_.laps, kRelaxed);
        slot_->lost_bytes.store(stats_.lost_bytes, kRelaxed);
    }
}

void Reader::heartbeat() noexcept {
    if (slot_ != nullptr) {
        slot_->heartbeat_ns.store(now_ns(), std::memory_order_release);
    }
}

WriterStatus Reader::writer_status() const noexcept {
    return {.pid = header_->writer_pid.load(std::memory_order_acquire),
            .epoch = header_->writer_epoch.load(std::memory_order_acquire),
            .heartbeat_ns = header_->heartbeat_ns.load(std::memory_order_acquire)};
}

// --------------------------------------------------------------------------- monitoring

std::expected<BusSnapshot, BusError> inspect_bus(std::string_view bus_name) {
    if (!valid_name(bus_name)) {
        return std::unexpected(BusError{BusErrc::InvalidName});
    }
    auto memory = SharedMemory::open(segment_name(bus_name), SharedMemory::Access::ReadOnly);
    if (!memory) {
        return std::unexpected(memory.error());
    }
    auto validated = validate(*memory);
    if (!validated) {
        return std::unexpected(validated.error());
    }
    const SegmentHeader& header = **validated;  // read-only mapping: loads only

    BusSnapshot snapshot;
    snapshot.name.assign(header.bus_name, ::strnlen(header.bus_name, sizeof(header.bus_name)));
    snapshot.capacity = header.capacity;
    snapshot.created_ns = header.created_ns;
    snapshot.writer = {.pid = header.writer_pid.load(std::memory_order_acquire),
                       .epoch = header.writer_epoch.load(std::memory_order_acquire),
                       .heartbeat_ns = header.heartbeat_ns.load(std::memory_order_acquire)};
    snapshot.writer_alive = process_alive(snapshot.writer.pid);
    snapshot.writer_stats = {.messages = header.writer_stats.messages.load(kRelaxed),
                             .bytes = header.writer_stats.bytes.load(kRelaxed),
                             .rejected = header.writer_stats.rejected.load(kRelaxed)};
    snapshot.tail = header.counters.tail.load(std::memory_order_acquire);

    for (std::size_t i = 0; i < detail::kReaderSlotCount; ++i) {
        const ReaderSlot& slot = header.readers[i];
        const std::uint32_t pid = slot.pid.load(std::memory_order_acquire);
        if (pid == 0 || slot.active.load(std::memory_order_acquire) == 0) {
            continue;
        }
        ReaderInfo info;
        info.slot = i;
        info.pid = pid;
        info.process_alive = process_alive(pid);
        info.name = load_slot_name(slot);
        info.position = slot.position.load(kRelaxed);
        info.lag_bytes = snapshot.tail > info.position ? snapshot.tail - info.position : 0;
        info.stats = {.messages = slot.messages.load(kRelaxed),
                      .laps = slot.laps.load(kRelaxed),
                      .lost_bytes = slot.lost_bytes.load(kRelaxed)};
        info.heartbeat_ns = slot.heartbeat_ns.load(kRelaxed);
        snapshot.readers.push_back(std::move(info));
    }
    return snapshot;
}

std::vector<std::string> list_buses() {
    std::vector<std::string> names;
#ifdef __linux__
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator("/dev/shm", error)) {
        const std::string file = entry.path().filename().string();
        if (file.starts_with("mda.")) {
            names.push_back(file.substr(4));
        }
    }
    std::ranges::sort(names);
#endif
    return names;
}

}  // namespace mda::bus

#pragma once

#include <cstddef>
#include <cstdint>

#include <mda/core/price.hpp>
#include <mda/core/timestamp.hpp>
#include <mda/messages/message.hpp>

namespace mda::messages {

inline constexpr std::size_t kDepthLevels = 5;

/// One price level. 16 bytes. Sizes as u32 (Kite sends 32-bit depth quantities).
struct DepthLevel {
    core::Price price;
    std::uint32_t quantity = 0;
    std::uint32_t orders = 0;
};
static_assert(sizeof(DepthLevel) == 16);

/// Best 5 bids and asks (full mode only). Sent separately from MarketSnapshot because
/// few consumers need depth and it is the largest part. 184 bytes.
struct DepthSnapshot {
    EventHeader header;
    core::Timestamp receive_ts;
    DepthLevel bids[kDepthLevels];  ///< best first
    DepthLevel asks[kDepthLevels];  ///< best first
};

template <>
struct MessageTraits<DepthSnapshot> {
    static constexpr MessageType type = MessageType::DepthSnapshot;
    static constexpr std::uint16_t version = 1;
};

static_assert(Message<DepthSnapshot>);
static_assert(sizeof(DepthSnapshot) == 184);
static_assert(offsetof(DepthSnapshot, receive_ts) == 16);
static_assert(offsetof(DepthSnapshot, bids) == 24);
static_assert(offsetof(DepthSnapshot, asks) == 104);

}  // namespace mda::messages

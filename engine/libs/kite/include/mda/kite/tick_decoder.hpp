#pragma once

// Decoder for Kite's binary WebSocket market data (docs/design/kite-adapter.md).
//
//   TickDecoder decoder(token_map);
//   decoder.decode(frame, receive_ts, [&](const MarketSnapshot& s, const DepthSnapshot* d) {
//       ... publish s (and *d when not null) ...
//   });

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

#include <mda/core/byte_order.hpp>
#include <mda/core/timestamp.hpp>
#include <mda/kite/token_map.hpp>
#include <mda/messages/depth_snapshot.hpp>
#include <mda/messages/market_snapshot.hpp>

namespace mda::kite {

enum class PacketResult {
    Decoded,
    UnknownToken,  ///< well-formed, but the token isn't in today's map
    Malformed,     ///< length doesn't match any known layout
};

/// One decoded packet. `depth` is valid only when `has_depth`.
struct DecodedPacket {
    messages::MarketSnapshot snapshot;
    messages::DepthSnapshot depth;
    bool has_depth = false;
};

/// Decodes a single packet (the bytes after its length prefix).
[[nodiscard]] PacketResult decode_packet(std::span<const std::byte> packet,
                                         core::Timestamp receive_ts, const TokenMap& tokens,
                                         DecodedPacket& out) noexcept;

enum class FrameResult {
    Ok,         ///< all packets well-formed (some may have had unknown tokens)
    Heartbeat,  ///< 1-byte keep-alive
    Malformed,  ///< framing or a packet was invalid; packets before it were delivered
};

struct DecoderStats {
    std::uint64_t frames = 0;
    std::uint64_t heartbeats = 0;
    std::uint64_t packets = 0;  ///< decoded and delivered
    std::uint64_t unknown_tokens = 0;
    std::uint64_t malformed = 0;  ///< malformed frames
};

template <typename F>
concept PacketSink =
    std::invocable<F&, const messages::MarketSnapshot&, const messages::DepthSnapshot*>;

class TickDecoder {
public:
    explicit TickDecoder(const TokenMap& tokens) noexcept : tokens_(&tokens) {}

    /// Decodes one binary frame, calling `sink(snapshot, depth_or_null)` per packet.
    template <PacketSink Sink>
    FrameResult decode(
        std::span<const std::byte> frame, core::Timestamp receive_ts,
        Sink&& sink) noexcept(noexcept(sink(std::declval<messages::MarketSnapshot&>(), nullptr))) {
        ++stats_.frames;
        if (frame.size() == 1) {
            ++stats_.heartbeats;
            return FrameResult::Heartbeat;
        }
        if (frame.size() < 2) {
            return malformed();
        }
        const auto count = core::load_be<std::uint16_t>(frame, 0);
        std::size_t offset = 2;
        for (std::uint16_t i = 0; i < count; ++i) {
            if (frame.size() - offset < 2) {
                return malformed();
            }
            const auto length = core::load_be<std::uint16_t>(frame, offset);
            offset += 2;
            if (frame.size() - offset < length) {
                return malformed();
            }
            const auto packet = frame.subspan(offset, length);
            offset += length;

            switch (decode_packet(packet, receive_ts, *tokens_, scratch_)) {
                case PacketResult::Decoded:
                    ++stats_.packets;
                    sink(scratch_.snapshot, scratch_.has_depth ? &scratch_.depth : nullptr);
                    break;
                case PacketResult::UnknownToken:
                    ++stats_.unknown_tokens;
                    break;
                case PacketResult::Malformed:
                    return malformed();
            }
        }
        return FrameResult::Ok;
    }

    [[nodiscard]] const DecoderStats& stats() const noexcept { return stats_; }

private:
    FrameResult malformed() noexcept {
        ++stats_.malformed;
        return FrameResult::Malformed;
    }

    const TokenMap* tokens_;
    DecodedPacket scratch_{};  // reused: no allocation per packet
    DecoderStats stats_;
};

}  // namespace mda::kite

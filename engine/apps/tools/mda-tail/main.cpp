// mda-tail — print decoded market data from the bus as it arrives.
//
//   mda-tail [--config config/mda.yaml] [--filter TEXT] [--depth]
//
// --filter keeps only instruments whose contract key contains TEXT (case-insensitive),
// e.g. --filter "nifty 50" or --filter fut. --depth also prints the 5-level book.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>
#include <unordered_map>

#include <mda/bus/bus.hpp>
#include <mda/bus/wait_strategy.hpp>
#include <mda/instruments/registry.hpp>
#include <mda/messages/depth_snapshot.hpp>
#include <mda/messages/feed_status.hpp>
#include <mda/messages/market_snapshot.hpp>
#include <mda/runtime/config.hpp>

namespace {

using namespace mda;

std::string lower(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// HH:MM:SS.mmm in IST.
std::string ist_time(core::Timestamp t) {
    const std::int64_t ns = t.ns() + std::int64_t{330} * 60 * 1'000'000'000;
    const std::time_t seconds = static_cast<std::time_t>(ns / 1'000'000'000);
    std::tm utc{};
    ::gmtime_r(&seconds, &utc);
    char text[32];
    std::snprintf(text, sizeof(text), "%02d:%02d:%02d.%03lld", utc.tm_hour, utc.tm_min, utc.tm_sec,
                  static_cast<long long>((ns / 1'000'000) % 1000));
    return text;
}

const char* state_name(messages::FeedState state) {
    switch (state) {
        case messages::FeedState::Connecting:
            return "connecting";
        case messages::FeedState::Connected:
            return "connected";
        case messages::FeedState::Disconnected:
            return "disconnected";
        case messages::FeedState::Stale:
            return "stale";
        case messages::FeedState::AuthExpired:
            return "auth expired";
    }
    return "?";
}

}  // namespace

int main(int argc, char** argv) {
    std::string filter;
    bool show_depth = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--filter" && i + 1 < argc) {
            filter = lower(argv[++i]);
        } else if (arg == "--depth") {
            show_depth = true;
        }
    }
    const auto config = runtime::load_config(runtime::config_path_from_args(argc, argv));
    if (!config) {
        std::fprintf(stderr, "error: %s\n", config.error().c_str());
        return 1;
    }

    // Names: the registry maps InstrumentId -> contract key.
    std::unordered_map<std::uint32_t, std::string> names;
    if (auto loaded = instruments::InstrumentRegistry::load(config->kite.registry_file)) {
        for (const auto& instrument : loaded->registry.all()) {
            names[instrument.id.value()] = instruments::to_string(instrument.key);
        }
    }
    auto name_of = [&](core::InstrumentId id) -> std::string {
        const auto it = names.find(id.value());
        return it != names.end() ? it->second : "id " + std::to_string(id.value());
    };
    auto wanted = [&](core::InstrumentId id) {
        return filter.empty() || lower(name_of(id)).find(filter) != std::string::npos;
    };

    auto reader = bus::Reader::attach(config->md_bus.name, {.name = "mda-tail"});
    if (!reader) {
        std::fprintf(stderr, "error: bus '%s': %s (is mda-feedd running?)\n",
                     config->md_bus.name.c_str(), bus::to_string(reader.error()).c_str());
        return 1;
    }
    std::printf("tailing bus '%s'%s — Ctrl+C to stop\n", config->md_bus.name.c_str(),
                filter.empty() ? "" : (" filter '" + filter + "'").c_str());
    std::fflush(stdout);

    bus::Backoff wait;
    while (true) {
        const auto result = reader->poll([&](bus::MsgType type,
                                             std::span<const std::byte> payload) {
            if (auto s = messages::decode<messages::MarketSnapshot>(type, payload)) {
                if (!wanted(s->header.instrument_id)) {
                    return;
                }
                const double change = s->close.raw() != 0
                                          ? 100.0 * static_cast<double>((s->ltp - s->close).raw()) /
                                                static_cast<double>(s->close.raw())
                                          : 0.0;
                std::printf(
                    "%s  %-42s ltp %10s  %+6.2f%%  vol %12lld  oi %12lld  exch %s  lag %4lld ms\n",
                    ist_time(s->receive_ts).c_str(), name_of(s->header.instrument_id).c_str(),
                    core::to_string(s->ltp).c_str(), change,
                    static_cast<long long>(s->volume.value()),
                    static_cast<long long>(s->oi.value()),
                    s->exchange_ts.is_set() ? ist_time(s->exchange_ts).substr(0, 8).c_str() : "-",
                    static_cast<long long>(s->exchange_ts.is_set()
                                               ? (s->receive_ts - s->exchange_ts).count() /
                                                     1'000'000
                                               : 0));
            } else if (auto d = messages::decode<messages::DepthSnapshot>(type, payload)) {
                if (!show_depth || !wanted(d->header.instrument_id)) {
                    return;
                }
                for (std::size_t i = 0; i < messages::kDepthLevels; ++i) {
                    std::printf("      bid %8u @ %10s (%3u)   ask %8u @ %10s (%3u)\n",
                                d->bids[i].quantity, core::to_string(d->bids[i].price).c_str(),
                                d->bids[i].orders, d->asks[i].quantity,
                                core::to_string(d->asks[i].price).c_str(), d->asks[i].orders);
                }
            } else if (auto f = messages::decode<messages::FeedStatus>(type, payload)) {
                std::printf("%s  [feed] %s, %u instruments, %u reconnects\n",
                            ist_time(f->timestamp).c_str(), state_name(f->state),
                            f->subscribed_instruments, f->reconnects);
            }
            std::fflush(stdout);
        });
        if (result.status == bus::PollStatus::Empty) {
            wait.idle();
        } else {
            wait.reset();
            if (result.status == bus::PollStatus::Lapped) {
                std::printf("[lapped: %llu bytes skipped]\n",
                            static_cast<unsigned long long>(result.lost_bytes));
            }
        }
    }
}

// mda-replayer — replays a recorded session from the tick journal (ADR-0026).
//
//   mda-replayer --config config/replay.yaml [--date 2026-10-23] [--from 09:15] [--to 15:40]
//                [--speed 1|10|max]
//
// Reads <journal_dir>/<date>/<HH>.mdj, decodes each raw frame with the same Kite decoder as
// mda-feedd (tokens from that day's saved instruments.csv) and publishes MarketSnapshot,
// DepthSnapshot and FeedStatus on the config's md bus, paced by the recorded receive times
// (divided by --speed; "max" = no waiting). Run mda-engine and mda-gateway with the same
// config to see the session. It refuses the live bus name "md": a replay must never mix
// with live data. When the recording ends it keeps the bus open until Ctrl+C, so the
// replayed state stays visible.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

#include <spdlog/spdlog.h>

#include <mda/bus/bus.hpp>
#include <mda/instruments/registry.hpp>
#include <mda/journal/journal.hpp>
#include <mda/kite/instrument_import.hpp>
#include <mda/kite/tick_decoder.hpp>
#include <mda/messages/depth_snapshot.hpp>
#include <mda/messages/feed_status.hpp>
#include <mda/messages/market_snapshot.hpp>
#include <mda/messages/raw_frame.hpp>
#include <mda/runtime/config.hpp>
#include <mda/runtime/logging.hpp>

namespace {

using namespace mda;
using Clock = std::chrono::steady_clock;

constexpr std::int64_t kIst = 5 * 3600 + 30 * 60;

std::atomic<bool> g_stop{false};

void on_signal(int) {
    g_stop.store(true);
}

struct Options {
    std::string date;         ///< YYYY-MM-DD (IST); default today
    int from_minute = 0;      ///< IST minute of day, inclusive
    int to_minute = 24 * 60;  ///< exclusive
    double speed = 1.0;       ///< 0 = as fast as possible
};

int usage() {
    std::fprintf(
        stderr,
        "usage: mda-replayer --config FILE [--date YYYY-MM-DD] [--from HH:MM] [--to HH:MM] "
        "[--speed N|max]\n");
    return 2;
}

std::optional<int> minute_of_day(const std::string& text) {
    int h = 0;
    int m = 0;
    if (std::sscanf(text.c_str(), "%d:%d", &h, &m) != 2 || h < 0 || h > 24 || m < 0 || m > 59) {
        return std::nullopt;
    }
    return h * 60 + m;
}

std::string today_ist() {
    const auto now = std::chrono::system_clock::now() + std::chrono::seconds(kIst);
    const std::chrono::year_month_day ymd{std::chrono::floor<std::chrono::days>(now)};
    char text[16];
    std::snprintf(text, sizeof text, "%04d-%02u-%02u", static_cast<int>(ymd.year()),
                  static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()));
    return text;
}

int run(const runtime::Config& config, const Options& options) {
    if (config.md_bus.name == "md") {
        spdlog::error("replay: the config's md bus is the live one ('md'); use config/replay.yaml");
        return 2;
    }
    const auto day = config.recorder.journal_dir / options.date;
    const auto files = journal::hour_files(day);
    if (files.empty()) {
        spdlog::error("replay: no journal files in {}", day.string());
        return 1;
    }

    // That day's tokens: the permanent registry (read only) plus the saved instrument list.
    auto loaded = instruments::InstrumentRegistry::load(config.kite.registry_file);
    if (!loaded) {
        spdlog::error("registry: cannot load {}", config.kite.registry_file.string());
        return 1;
    }
    std::ifstream csv_file(day / "instruments.csv", std::ios::binary);
    if (!csv_file) {
        spdlog::error("replay: {} is missing", (day / "instruments.csv").string());
        return 1;
    }
    std::stringstream csv;
    csv << csv_file.rdbuf();
    auto imported = kite::import_instruments(csv.str(), loaded->registry);
    if (!imported) {
        spdlog::error("replay: {} has no header or missing columns",
                      (day / "instruments.csv").string());
        return 1;
    }
    kite::TickDecoder decoder(imported->tokens);

    auto writer = bus::Writer::create_or_attach(
        {.name = config.md_bus.name, .capacity = config.md_bus.capacity_mb * 1024 * 1024});
    if (!writer) {
        spdlog::error("bus: {}: {}", config.md_bus.name, bus::to_string(writer.error()));
        return 1;
    }
    spdlog::info(
        "replay: {} ({} hour files) onto '{}' at {}", options.date, files.size(),
        config.md_bus.name,
        options.speed == 0 ? std::string("full speed") : std::to_string(options.speed) + "x");

    const auto in_window = [&](std::int64_t receive_ns) {
        const std::int64_t s = receive_ns / 1'000'000'000;
        const int minute = static_cast<int>(((s + kIst) % 86'400 + 86'400) % 86'400 / 60);
        return minute >= options.from_minute && minute < options.to_minute;
    };
    std::uint64_t sequence = 0;
    std::uint64_t frames = 0;
    std::uint64_t snapshots = 0;
    std::optional<std::int64_t> first_ns;
    const auto wall_start = Clock::now();
    auto next_heartbeat = Clock::now();
    const auto beat = [&] {
        if (Clock::now() >= next_heartbeat) {
            next_heartbeat = Clock::now() + std::chrono::milliseconds(100);
            writer->heartbeat();
        }
    };

    for (const auto& file : files) {
        auto reader = journal::Reader::open(file);
        if (!reader) {
            spdlog::warn("replay: {}", reader.error());
            continue;
        }
        while (!g_stop.load()) {
            const auto record = reader->next();
            if (!record) {
                break;
            }
            if (record->type == journal::kGapType) {
                spdlog::warn("replay: the recording has a gap here ({} bytes lost)",
                             journal::decode_gap(record->payload).lost_bytes);
                continue;
            }
            if (auto status =
                    messages::decode<messages::FeedStatus>(record->type, record->payload)) {
                if (in_window(status->timestamp.ns())) {
                    status->header.sequence = ++sequence;
                    (void)writer->publish(record->type, messages::as_bytes(*status));
                }
                continue;
            }
            const auto raw = messages::decode_raw_frame(record->type, record->payload);
            if (!raw || !in_window(raw->header.receive_ts.ns())) {
                continue;
            }
            // Pace by the recorded receive times.
            const std::int64_t at = raw->header.receive_ts.ns();
            if (!first_ns) {
                first_ns = at;
            }
            if (options.speed > 0) {
                const auto due =
                    wall_start + std::chrono::nanoseconds(static_cast<std::int64_t>(
                                     static_cast<double>(at - *first_ns) / options.speed));
                while (Clock::now() < due && !g_stop.load()) {
                    beat();
                    std::this_thread::sleep_until(
                        std::min(due, Clock::now() + std::chrono::milliseconds(100)));
                }
            }
            beat();
            ++frames;
            decoder.decode(raw->frame, raw->header.receive_ts,
                           [&](const messages::MarketSnapshot& snapshot,
                               const messages::DepthSnapshot* depth) {
                               messages::MarketSnapshot s = snapshot;
                               s.header.sequence = ++sequence;
                               (void)writer->publish(
                                   messages::to_wire(messages::MessageType::MarketSnapshot),
                                   messages::as_bytes(s));
                               if (depth != nullptr) {
                                   messages::DepthSnapshot d = *depth;
                                   d.header.sequence = ++sequence;
                                   (void)writer->publish(
                                       messages::to_wire(messages::MessageType::DepthSnapshot),
                                       messages::as_bytes(d));
                               }
                               ++snapshots;
                           });
        }
        if (reader->torn()) {
            spdlog::warn("replay: {} ends in a torn record (recorder still writing, or a crash)",
                         file.string());
        }
    }
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - wall_start).count();
    spdlog::info(
        "replay: done: {} frames, {} snapshots in {} s; keeping the bus open (Ctrl+C to stop)",
        frames, snapshots, elapsed);
    while (!g_stop.load()) {
        beat();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options options{.date = today_ist()};
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool has_value = i + 1 < argc;
        if (arg == "--config" && has_value) {
            ++i;  // read by config_path_from_args
        } else if (arg == "--date" && has_value) {
            options.date = argv[++i];
        } else if ((arg == "--from" || arg == "--to") && has_value) {
            const auto minute = minute_of_day(argv[++i]);
            if (!minute) {
                return usage();
            }
            (arg == "--from" ? options.from_minute : options.to_minute) = *minute;
        } else if (arg == "--speed" && has_value) {
            const std::string value = argv[++i];
            options.speed = value == "max" ? 0.0 : std::atof(value.c_str());
            if (value != "max" && options.speed <= 0) {
                return usage();
            }
        } else {
            return usage();
        }
    }
    const auto config = runtime::load_config(runtime::config_path_from_args(argc, argv));
    if (!config) {
        std::fprintf(stderr, "error: %s\n", config.error().c_str());
        return 1;
    }
    runtime::setup_logging("mda-replayer", config->logging);
    spdlog::info("mda-replayer starting (config {})", config->source.string());
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    const int code = run(*config, options);
    spdlog::info("mda-replayer stopped (exit code {})", code);
    runtime::shutdown_logging();
    return code;
}

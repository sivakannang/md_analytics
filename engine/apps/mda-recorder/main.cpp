// mda-recorder — writes the tick journal (ADR-0026).
//
//   mda-recorder [--config config/mda.yaml]
//
// Reads bus "md" and appends every RawFrame (each Kite WebSocket frame as received, with
// receive time and sequence) and FeedStatus to <journal_dir>/<IST date>/<HH>.mdj. If it
// falls behind and is lapped, it writes a Gap record and carries on: the bus never waits
// for it, so a slow disk can't slow the feed. Also keeps a copy of each day's Kite
// instrument list (replay needs it to map tokens) and deletes days older than keep_days.
//
// One thread. Flushes every second; fsyncs each hour file when it closes. Each minute it
// also writes <data>/run/recorder.status (JSON) for the admin system monitor
// (docs/design/system-monitor.md §3): today's gaps and the last minute's records.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <optional>
#include <thread>

#include <unistd.h>

#include <spdlog/spdlog.h>

#include <mda/bus/bus.hpp>
#include <mda/bus/wait_strategy.hpp>
#include <mda/journal/journal.hpp>
#include <mda/messages/feed_status.hpp>
#include <mda/messages/message.hpp>
#include <mda/runtime/config.hpp>
#include <mda/runtime/logging.hpp>

namespace {

using namespace mda;
using Clock = std::chrono::steady_clock;

std::atomic<bool> g_stop{false};

void on_signal(int) {
    g_stop.store(true);
}

std::int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::int64_t unix_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// Waits (checking for a stop) until bus `name` exists, then attaches.
std::optional<bus::Reader> attach(const std::string& name) {
    bool reported = false;
    while (!g_stop.load()) {
        if (auto reader = bus::Reader::attach(name, {.name = "mda-recorder"})) {
            return std::move(*reader);
        } else if (!reported) {
            spdlog::warn("bus '{}' not available ({}); waiting for mda-feedd", name, bus::to_string(reader.error()));
            reported = true;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return std::nullopt;
}

/// Copies today's Kite instrument list next to the journal when it is new or changed
/// (mda-feedd keeps only today's, and replaces it when it downloads a fresher one).
void keep_instruments(const runtime::Config& config, const std::filesystem::path& day) {
    const std::string date = day.filename().string();
    const auto source = config.kite.instruments_dir / ("kite_instruments_" + date + ".csv");
    const auto copy = day / "instruments.csv";
    std::error_code ec;
    if (!std::filesystem::exists(source, ec)) {
        return;
    }
    if (std::filesystem::exists(copy, ec) &&
        std::filesystem::last_write_time(copy, ec) >= std::filesystem::last_write_time(source, ec)) {
        return;
    }
    std::filesystem::create_directories(day, ec);
    std::filesystem::copy_file(source, copy, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        spdlog::warn("journal: copying {}: {}", source.string(), ec.message());
    } else {
        spdlog::info("journal: instrument list for {} saved", date);
    }
}

/// The status file the gateway reads; written to a temporary name and renamed, so a reader
/// never sees half a file.
void write_status(const std::filesystem::path& path, std::uint64_t gaps_today, std::uint64_t records,
                  std::uint64_t bytes, const std::filesystem::path& writing) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const auto temporary = path.string() + ".tmp";
    std::FILE* file = std::fopen(temporary.c_str(), "w");
    if (file == nullptr) {
        return;
    }
    std::fprintf(file,
                 "{\"time\":%lld,\"pid\":%d,\"gaps_today\":%llu,\"records_last_minute\":%llu,"
                 "\"bytes_last_minute\":%llu,\"writing\":\"%s\"}\n",
                 static_cast<long long>(unix_now()), static_cast<int>(getpid()),
                 static_cast<unsigned long long>(gaps_today), static_cast<unsigned long long>(records),
                 static_cast<unsigned long long>(bytes), writing.filename().string().c_str());
    std::fclose(file);
    std::filesystem::rename(temporary, path, ec);
}

int run(const runtime::Config& config) {
    if (!config.recorder.enabled) {
        spdlog::info("recording is off (recorder.enabled: false in {})", config.source.string());
        return 0;
    }
    const auto& root = config.recorder.journal_dir;
    if (const std::size_t pruned = journal::prune(root, unix_now(), config.recorder.keep_days); pruned > 0) {
        spdlog::info("journal: {} day folders older than {} days deleted", pruned, config.recorder.keep_days);
    }
    auto reader = attach(config.md_bus.name);
    if (!reader) {
        return 0;  // stopped while waiting
    }
    spdlog::info("bus: reading '{}'; journal in {}", config.md_bus.name, root.string());

    std::optional<journal::Writer> writer;
    std::filesystem::path current;
    const auto open_hour = [&]() -> bool {
        const auto path = journal::hour_file(root, unix_now());
        if (writer && path == current) {
            return true;
        }
        if (writer) {
            if (auto closed = writer->close(); !closed) {
                spdlog::error("journal: {}", closed.error());
            }
            spdlog::info("journal: {} closed ({} MB)", current.string(), writer->size() / (1024 * 1024));
            writer.reset();
        }
        auto opened = journal::Writer::open(path);
        if (!opened) {
            spdlog::error("journal: {}", opened.error());
            return false;
        }
        writer.emplace(std::move(*opened));
        current = path;
        spdlog::info("journal: writing {}", path.string());
        keep_instruments(config, path.parent_path());
        if (const std::size_t pruned = journal::prune(root, unix_now(), config.recorder.keep_days); pruned > 0) {
            spdlog::info("journal: {} old day folders deleted", pruned);
        }
        return true;
    };
    if (!open_hour()) {
        return 1;
    }

    const auto raw = messages::to_wire(messages::MessageType::RawFrame);
    const auto status = messages::to_wire(messages::MessageType::FeedStatus);
    std::uint64_t records = 0;
    std::uint64_t bytes = 0;
    std::uint64_t gaps = 0;
    std::uint64_t gaps_today = 0;
    std::filesystem::path gaps_day = current.parent_path();
    const auto status_file = root.parent_path() / "run" / "recorder.status";
    bool write_failed = false;
    bus::Backoff wait;
    auto next_second = Clock::now();
    auto next_minute = Clock::now() + std::chrono::minutes(1);
    while (!g_stop.load()) {
        const auto result = reader->poll([&](bus::MsgType type, std::span<const std::byte> payload) {
            if ((type != raw && type != status) || !writer) {
                return;
            }
            if (auto done = writer->append(type, payload); !done) {
                if (!write_failed) {
                    spdlog::error("journal: {}", done.error());
                    write_failed = true;
                }
                return;
            }
            write_failed = false;
            ++records;
            bytes += payload.size();
        });
        if (result.status == bus::PollStatus::Empty) {
            wait.idle();
        } else {
            wait.reset();
            if (result.status == bus::PollStatus::Lapped) {
                ++gaps;
                ++gaps_today;
                spdlog::warn("journal: lapped on '{}'; {} bytes lost", config.md_bus.name, result.lost_bytes);
                if (writer) {
                    (void)writer->append_gap({.time_ns = unix_now_ns(), .lost_bytes = result.lost_bytes});
                }
            }
        }

        if (const auto now = Clock::now(); now >= next_second) {
            next_second = now + std::chrono::seconds(1);
            if (writer) {
                (void)writer->flush();
            }
            (void)open_hour();  // rotates on the IST hour
            reader->heartbeat();
            if (now >= next_minute) {
                next_minute = now + std::chrono::minutes(1);
                if (writer) {
                    keep_instruments(config, current.parent_path());
                }
                spdlog::info("summary: {} records, {} KB in the last minute; {} gaps", records, bytes / 1024, gaps);
                if (current.parent_path() != gaps_day) {
                    gaps_day = current.parent_path();  // a new IST day: its own gap count
                    gaps_today = 0;
                }
                write_status(status_file, gaps_today, records, bytes, current);
                records = bytes = 0;
            }
        }
    }
    if (writer) {
        if (auto closed = writer->close(); !closed) {
            spdlog::error("journal: {}", closed.error());
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const auto config = runtime::load_config(runtime::config_path_from_args(argc, argv));
    if (!config) {
        std::fprintf(stderr, "error: %s\n", config.error().c_str());
        return 1;
    }
    runtime::setup_logging("mda-recorder", config->logging);
    spdlog::info("mda-recorder starting (config {}, log level {})", config->source.string(), config->logging.level);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    const int code = run(*config);
    spdlog::info("mda-recorder stopped (exit code {})", code);
    runtime::shutdown_logging();
    return code;
}

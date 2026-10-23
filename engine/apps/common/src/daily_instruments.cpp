#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <spdlog/spdlog.h>

#include <mda/apps/daily_instruments.hpp>
#include <mda/kite/instrument_import.hpp>
#include <mda/kite/run_sync.hpp>

namespace mda::apps {
namespace {

/// Today's date in IST ("2026-10-22"): Kite's instrument list changes once per day.
std::string ist_date_impl() {
    const auto now = std::chrono::system_clock::now() + std::chrono::minutes(330);
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    ::gmtime_r(&seconds, &utc);
    char text[16];
    std::strftime(text, sizeof(text), "%Y-%m-%d", &utc);
    return text;
}

/// True if `file` was written after 08:30 IST today, i.e. after Kite's daily instrument
/// refresh (expired contracts removed, new ones added). Earlier copies are stale.
bool written_after_morning_refresh(const std::filesystem::path& file) {
    std::error_code error;
    const auto written = std::filesystem::last_write_time(file, error);
    if (error) {
        return false;
    }
    // file_clock -> system_clock via the current offset (libc++ lacks clock_cast).
    const auto written_system = std::chrono::system_clock::now() +
                                std::chrono::duration_cast<std::chrono::system_clock::duration>(
                                    written - std::filesystem::file_time_type::clock::now());
    const auto ist_now = std::chrono::system_clock::now() + std::chrono::minutes(330);
    const auto ist_midnight = std::chrono::floor<std::chrono::days>(ist_now);
    const auto refresh = ist_midnight + std::chrono::hours(8) + std::chrono::minutes(30) -
                         std::chrono::minutes(330);  // back to UTC
    return written_system >= refresh;
}

/// Today's instrument CSV: from the cache if downloaded after this morning's refresh,
/// else downloaded and cached.
std::optional<std::string> instruments_csv(kite::RestClient& client,
                                           const runtime::KiteConfig& config) {
    const auto cache = config.instruments_dir / ("kite_instruments_" + ist_date_impl() + ".csv");
    if (std::filesystem::exists(cache) && !written_after_morning_refresh(cache)) {
        spdlog::info("instruments: cached list {} predates today's 08:30 IST refresh; downloading",
                     cache.string());
    } else if (std::filesystem::exists(cache)) {
        std::ifstream file(cache, std::ios::binary);
        std::stringstream buffer;
        buffer << file.rdbuf();
        spdlog::info("instruments: using today's cached list {} ({:.1f} MB)", cache.string(),
                     static_cast<double>(buffer.str().size()) / 1048576.0);
        return buffer.str();
    }
    const auto start = std::chrono::steady_clock::now();
    auto csv = kite::run_sync(client.instruments());
    if (!csv) {
        spdlog::error("instruments: download failed: {}", kite::to_string(csv.error()));
        return std::nullopt;
    }
    std::filesystem::create_directories(config.instruments_dir);
    std::ofstream(cache, std::ios::binary) << *csv;
    spdlog::info("instruments: downloaded {:.1f} MB in {} ms, cached at {}",
                 static_cast<double>(csv->size()) / 1048576.0,
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - start)
                     .count(),
                 cache.string());
    return std::move(*csv);
}

}  // namespace

std::string ist_date() {
    return ist_date_impl();
}

std::optional<DailyInstruments> load_today_instruments(const runtime::KiteConfig& config,
                                                       kite::RestClient& rest) {
    auto loaded = instruments::InstrumentRegistry::load(config.registry_file);
    if (!loaded) {
        spdlog::error("registry: cannot load {}", config.registry_file.string());
        return std::nullopt;
    }
    auto& registry = loaded->registry;
    const auto csv = instruments_csv(rest, config);
    if (!csv) {
        return std::nullopt;
    }
    auto imported = kite::import_instruments(*csv, registry);
    if (!imported) {
        spdlog::error("instruments: CSV has no header or missing columns");
        return std::nullopt;
    }
    const auto& stats = imported->stats;
    spdlog::info(
        "instruments: {} rows, {} imported, {} new, {} unsupported, {} malformed, {} unlinked",
        stats.rows, stats.imported, stats.new_instruments, stats.skipped_unsupported,
        stats.malformed, stats.unlinked);
    if (auto saved = registry.save_new(config.registry_file); !saved) {
        spdlog::error("registry: cannot write {}", config.registry_file.string());
        return std::nullopt;
    } else {
        spdlog::info("registry: {} instruments ({} new ids saved)", registry.size(), *saved);
    }
    return DailyInstruments{.registry = std::move(registry), .tokens = std::move(imported->tokens)};
}

}  // namespace mda::apps

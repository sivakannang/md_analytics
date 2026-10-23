// mda-backfill — fill bars.db with Kite's historical 1-minute candles (+ OI) for the
// minutes we did not record (ADR-0021, docs/design/option-chain-and-bars.md §3).
//
//   mda-backfill [--config config/mda.yaml] [--days N] [--instrument KEY]...
//
// Scope (without --instrument): every instrument already in bars.db (what we record), the
// configured instruments, and every watchlist instrument — those listed today (Kite has no
// history for expired contracts). Range: the last N days (default 30) up to the minute
// before now. Backfilled bars never replace live ones (BarStore's rule), so running it any
// time is safe and only fills gaps. Runs alongside mda-engine (SQLite WAL).

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

#include <mda/appdb/schema.hpp>
#include <mda/apps/backfill.hpp>
#include <mda/apps/daily_instruments.hpp>
#include <mda/bars/bar_store.hpp>
#include <mda/core/timestamp.hpp>
#include <mda/kite/rest_client.hpp>
#include <mda/kite/run_sync.hpp>
#include <mda/kite/session.hpp>
#include <mda/runtime/config.hpp>
#include <mda/runtime/logging.hpp>
#include <mda/watchlists/watchlists.hpp>

namespace {

using namespace mda;
using Clock = std::chrono::steady_clock;

constexpr auto kRequestGap = std::chrono::milliseconds(350);  // ~3 requests per second
constexpr int kAttempts = 4;
constexpr std::int64_t kIst = 5 * 3600 + 30 * 60;

int usage() {
    std::fprintf(stderr, "usage: mda-backfill [--config FILE] [--days N] [--instrument KEY]...\n");
    return 2;
}

int run(const runtime::Config& config, int days, const std::vector<std::string>& only) {
    const auto session = kite::load_session(config.kite.session_file);
    if (!session) {
        spdlog::error("session: {} ({}); run mda-login", kite::to_string(session.error()),
                      config.kite.session_file.string());
        return 1;
    }
    const auto ca_file =
        config.kite.ca_file.empty() ? kite::default_ca_file() : std::optional(config.kite.ca_file);
    if (!ca_file) {
        spdlog::error("tls: no CA bundle found; set kite.ca_file");
        return 1;
    }
    auto tls = kite::make_tls_context(*ca_file);
    if (!tls) {
        spdlog::error("tls: {}", kite::to_string(tls.error()));
        return 1;
    }
    kite::RestClient rest(*tls, session->session);
    auto daily = apps::load_today_instruments(config.kite, rest);
    if (!daily) {
        return 1;
    }
    auto store = bars::BarStore::open(config.engine.bars_db);
    if (!store) {
        spdlog::error("bars: cannot open {}: {}", config.engine.bars_db.string(),
                      store.error().message);
        return 1;
    }

    // Scope.
    std::set<std::uint32_t> ids;
    if (!only.empty()) {
        for (const auto& text : only) {
            const auto key = instruments::parse_contract_key(text);
            const auto id = key ? daily->registry.find(*key) : core::InstrumentId{};
            if (!id.valid()) {
                spdlog::error("instrument '{}' is not known", text);
                return 1;
            }
            ids.insert(id.value());
        }
    } else {
        auto recorded = store->database().prepare("SELECT DISTINCT instrument_id FROM bars_1m");
        if (recorded) {
            while (recorded->step().value_or(storage::StepResult::Done) ==
                   storage::StepResult::Row) {
                ids.insert(static_cast<std::uint32_t>(recorded->column_int64(0)));
            }
        }
        for (const auto& text : config.feed.instruments) {
            if (const auto key = instruments::parse_contract_key(text)) {
                if (const auto id = daily->registry.find(*key); id.valid()) {
                    ids.insert(id.value());
                }
            }
        }
        if (auto app = storage::Database::open(config.database.path)) {
            if (app->migrate(appdb::migrations())) {
                watchlists::WatchlistService lists(*app);
                if (const auto on_lists = lists.all_instrument_ids()) {
                    for (const auto id : *on_lists) {
                        ids.insert(static_cast<std::uint32_t>(id));
                    }
                }
            }
        }
    }
    std::vector<std::pair<core::InstrumentId, std::uint32_t>> work;  // (id, today's token)
    std::size_t expired = 0;
    for (const auto value : ids) {
        const core::InstrumentId id(value);
        if (const auto token = daily->tokens.token_for(id); token != 0) {
            work.emplace_back(id, token);
        } else {
            ++expired;
        }
    }

    const std::int64_t now = core::Timestamp::now().ns() / 1'000'000'000;
    const std::int64_t current_minute = now - now % 60;
    const std::int64_t today = now - (now + kIst) % 86'400;  // 00:00 IST
    const std::int64_t first =
        today - static_cast<std::int64_t>(days - 1) * 86'400 + (9 * 3600 + 15 * 60);
    spdlog::info("backfill: {} instruments ({} skipped: not listed today), {} → now, {} days",
                 work.size(), expired, apps::ist_text(first), days);

    const auto started = Clock::now();
    std::size_t candles = 0;
    std::size_t failed = 0;
    std::size_t done = 0;
    auto next_request = Clock::now();
    for (const auto& [id, token] : work) {
        std::vector<kite::Candle> all;
        bool ok = true;
        for (std::int64_t from = first; from < now && ok;
             from += apps::kMaxBackfillDaysPerRequest * 86'400) {
            const std::int64_t to =
                std::min(now, from + apps::kMaxBackfillDaysPerRequest * 86'400 - 1);
            for (int attempt = 1;; ++attempt) {
                std::this_thread::sleep_until(next_request);
                next_request = Clock::now() + kRequestGap;
                auto chunk = kite::run_sync(rest.historical(token, "minute", apps::ist_text(from),
                                                            apps::ist_text(to), true));
                if (chunk) {
                    all.insert(all.end(), chunk->begin(), chunk->end());
                    break;
                }
                if (chunk.error().code == kite::KiteError::Code::AuthExpired) {
                    spdlog::error("backfill: Kite rejected the session ({}); run mda-login",
                                  chunk.error().message);
                    return 1;
                }
                if (attempt == kAttempts) {
                    spdlog::warn("backfill: instrument {} failed after {} attempts: {}", id.value(),
                                 attempt, chunk.error().message);
                    ok = false;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::seconds(1 << (attempt - 1)));
            }
        }
        if (!ok) {
            ++failed;
            continue;
        }
        const auto bars = apps::to_bars(id, all, current_minute);
        if (auto written = store->write(bars); !written) {
            spdlog::error("backfill: writing instrument {} failed: {}", id.value(),
                          written.error().message);
            ++failed;
            continue;
        }
        candles += bars.size();
        if (++done % 50 == 0) {
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - started).count();
            spdlog::info("backfill: {}/{} instruments, {} candles, {} failed, {} s", done,
                         work.size(), candles, failed, elapsed);
        }
    }
    spdlog::info("backfill: done — {} instruments, {} candles, {} failed, {} s", done, candles,
                 failed,
                 std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - started).count());
    return failed == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    int days = 30;
    std::vector<std::string> only;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            ++i;  // read by config_path_from_args
        } else if (arg == "--days" && i + 1 < argc) {
            days = std::atoi(argv[++i]);
        } else if (arg == "--instrument" && i + 1 < argc) {
            only.emplace_back(argv[++i]);
        } else {
            return usage();
        }
    }
    if (days < 1 || days > 365) {
        std::fprintf(stderr, "error: --days must be 1-365\n");
        return 2;
    }
    const auto config = runtime::load_config(runtime::config_path_from_args(argc, argv));
    if (!config) {
        std::fprintf(stderr, "error: %s\n", config.error().c_str());
        return 1;
    }
    runtime::setup_logging("mda-backfill", config->logging);
    const int code = run(*config, days, only);
    runtime::shutdown_logging();
    return code;
}

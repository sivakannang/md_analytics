// mda-feedd — Kite market data feed handler (docs/design/kite-connectivity.md).
//
//   mda-feedd [--config config/mda.yaml]
//
// Startup: config + logging -> session (validated via REST) -> today's instrument list
// (cached per day) -> registry + token map -> subscription list -> bus "md" -> WebSocket.
// Every frame is published raw (for the recorder) and decoded (snapshots + depth).
// Single thread: decode ≈ 100 ns/packet, publish ≈ 5 ns (kite-connectivity §5).
// It also backfills bar history for subscribed instruments that have none (ADR-0025).

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <mda/appdb/outbox.hpp>
#include <mda/appdb/schema.hpp>
#include <mda/apps/backfill.hpp>
#include <mda/apps/daily_instruments.hpp>
#include <mda/bars/bar_store.hpp>
#include <mda/bus/bus.hpp>
#include <mda/catalog/catalog.hpp>
#include <mda/instruments/registry.hpp>
#include <mda/kite/instrument_import.hpp>
#include <mda/kite/rest_client.hpp>
#include <mda/kite/run_sync.hpp>
#include <mda/kite/session.hpp>
#include <mda/kite/tick_decoder.hpp>
#include <mda/kite/ws_client.hpp>
#include <mda/messages/circuit_limits.hpp>
#include <mda/messages/feed_status.hpp>
#include <mda/messages/raw_frame.hpp>
#include <mda/notify/system_alerts.hpp>
#include <mda/runtime/config.hpp>
#include <mda/runtime/logging.hpp>
#include <mda/storage/database.hpp>
#include <mda/subscriptions/chain_windows.hpp>
#include <mda/watchlists/watchlists.hpp>

namespace {

using namespace mda;
namespace asio = boost::asio;

/// Resolves the configured subscriptions to today's Kite tokens.
std::vector<std::uint32_t> resolve_subscriptions(const runtime::FeedConfig& feed,
                                                 const instruments::InstrumentRegistry& registry,
                                                 const kite::TokenMap& tokens) {
    std::set<std::uint32_t> resolved;
    for (const std::string& text : feed.instruments) {
        const auto key = instruments::parse_contract_key(text);
        const core::InstrumentId id = key ? registry.find(*key) : core::InstrumentId{};
        const std::uint32_t token = id.valid() ? tokens.token_for(id) : 0;
        if (token == 0) {
            spdlog::warn("subscriptions: '{}' is not listed today; skipped", text);
            continue;
        }
        resolved.insert(token);
    }
    for (const std::string& underlying : feed.futures_of) {
        int found = 0;
        for (const auto& instrument : registry.all()) {
            if (instrument.listed_today && instrument.key.kind == instruments::Kind::Future &&
                instrument.key.underlying == underlying) {
                if (const std::uint32_t token = tokens.token_for(instrument.id); token != 0) {
                    resolved.insert(token);
                    ++found;
                }
            }
        }
        if (found == 0) {
            spdlog::warn("subscriptions: no listed futures for '{}'", underlying);
        }
    }
    return {resolved.begin(), resolved.end()};
}

constexpr std::size_t kMaxTokensPerConnection = 3000;  // Kite's limit

/// Kite tokens of watchlist instruments that are listed today.
std::vector<std::uint32_t> watchlist_tokens(std::span<const std::int64_t> ids,
                                            const kite::TokenMap& tokens) {
    std::vector<std::uint32_t> out;
    for (const std::int64_t id : ids) {
        const auto token = tokens.token_for(core::InstrumentId(static_cast<std::uint32_t>(id)));
        if (token != 0) {
            out.push_back(token);
        }
    }
    return out;
}

/// Sorted union of the static and watchlist tokens, capped at Kite's per-connection limit.
std::vector<std::uint32_t> merge_tokens(std::vector<std::uint32_t> base,
                                        std::span<const std::uint32_t> extra) {
    base.insert(base.end(), extra.begin(), extra.end());
    std::ranges::sort(base);
    base.erase(std::ranges::unique(base).begin(), base.end());
    if (base.size() > kMaxTokensPerConnection) {
        spdlog::warn(
            "subscriptions: {} instruments wanted, Kite allows {} per connection; extra dropped",
            base.size(), kMaxTokensPerConnection);
        base.resize(kMaxTokensPerConnection);
    }
    return base;
}

/// Publishes every frame (raw) and its decoded packets on the bus, with one sequence.
class FeedPublisher {
public:
    FeedPublisher(bus::Writer& writer, const kite::TokenMap& tokens)
        : writer_(&writer), decoder_(tokens) {}

    /// Called with every decoded snapshot (the chain windows watch spot prices).
    std::function<void(const messages::MarketSnapshot&)> on_snapshot;

    void on_frame(std::span<const std::byte> frame, core::Timestamp received) {
        messages::RawFrameHeader raw{};
        raw.header.sequence = ++sequence_;
        raw.receive_ts = received;
        raw.connection_id = 1;
        raw_buffer_.resize(messages::raw_frame_size(frame.size()));
        (void)writer_->publish(messages::to_wire(messages::MessageType::RawFrame),
                               messages::encode_raw_frame(raw_buffer_, raw, frame));

        decoder_.decode(
            frame, received,
            [this](const messages::MarketSnapshot& snapshot, const messages::DepthSnapshot* depth) {
                if (on_snapshot) {
                    on_snapshot(snapshot);
                }
                messages::MarketSnapshot s = snapshot;
                s.header.sequence = ++sequence_;
                (void)writer_->publish(messages::to_wire(messages::MessageType::MarketSnapshot),
                                       messages::as_bytes(s));
                if (depth != nullptr) {
                    messages::DepthSnapshot d = *depth;
                    d.header.sequence = ++sequence_;
                    (void)writer_->publish(messages::to_wire(messages::MessageType::DepthSnapshot),
                                           messages::as_bytes(d));
                }
            });
    }

    void on_state(kite::WsState state, std::uint32_t subscribed, std::uint32_t reconnects) {
        state_ = state;
        messages::FeedStatus status{};
        status.header = messages::make_header<messages::FeedStatus>({}, ++sequence_);
        status.timestamp = core::Timestamp::now();
        status.connection_id = 1;
        status.subscribed_instruments = subscribed;
        status.reconnects = reconnects;
        switch (state) {
            case kite::WsState::Connecting:
                status.state = messages::FeedState::Connecting;
                break;
            case kite::WsState::Connected:
                status.state = messages::FeedState::Connected;
                break;
            case kite::WsState::Disconnected:
                status.state = messages::FeedState::Disconnected;
                break;
            case kite::WsState::AuthExpired:
                status.state = messages::FeedState::AuthExpired;
                break;
        }
        (void)writer_->publish(messages::to_wire(messages::MessageType::FeedStatus),
                               messages::as_bytes(status));
    }

    void publish_limits(core::InstrumentId instrument, const kite::CircuitLimit& limit) {
        messages::CircuitLimits message{};
        message.header = messages::make_header<messages::CircuitLimits>(instrument, ++sequence_);
        message.fetched_ts = core::Timestamp::now();
        message.lower = limit.lower;
        message.upper = limit.upper;
        (void)writer_->publish(messages::to_wire(messages::MessageType::CircuitLimits),
                               messages::as_bytes(message));
    }

    /// Re-sends the current state: readers that attached after the last change (status
    /// events are otherwise only sent on change) learn it within a second.
    void republish_state(std::uint32_t subscribed, std::uint32_t reconnects) {
        if (state_) {
            on_state(*state_, subscribed, reconnects);
        }
    }

    [[nodiscard]] const kite::DecoderStats& decoder_stats() const { return decoder_.stats(); }

private:
    bus::Writer* writer_;
    kite::TickDecoder decoder_;
    std::uint64_t sequence_ = 0;
    std::vector<std::byte> raw_buffer_;
    std::optional<kite::WsState> state_;
};

/// Keeps circuit limits on the bus (they are not in the tick stream): fetches them from
/// Kite's quote API for newly subscribed instruments within ~2 s, re-fetches everything
/// every `refresh` (bands can be revised intraday), and re-publishes all every 60 s so a
/// gateway that starts later learns them. Indices have no band and are skipped.
asio::awaitable<void> circuit_limits_loop(const runtime::KiteConfig& kite_config,
                                          asio::ssl::context& tls,
                                          const instruments::InstrumentRegistry& registry,
                                          const kite::TokenMap& tokens,
                                          const std::vector<std::uint32_t>& subscriptions,
                                          FeedPublisher& publisher, std::chrono::minutes refresh) {
    using Clock = std::chrono::steady_clock;
    asio::steady_timer timer(co_await asio::this_coro::executor);
    std::unordered_map<std::uint32_t, kite::CircuitLimit> known;  // by token
    auto next_full = Clock::now();
    auto next_republish = Clock::now() + std::chrono::seconds(60);
    bool reported_error = false;

    while (true) {
        const bool full = Clock::now() >= next_full;
        std::vector<std::string> keys;
        for (const std::uint32_t token : subscriptions) {
            if (!full && known.contains(token)) {
                continue;
            }
            const auto* instrument = registry.get(tokens.find(token));
            if (instrument != nullptr && instrument->key.kind != instruments::Kind::Index) {
                keys.push_back(kite::quote_key(instrument->key.exchange, instrument->key.market,
                                               instrument->trading_symbol));
            }
        }
        if (full) {
            next_full = Clock::now() + refresh;
        }
        if (!keys.empty()) {
            const auto session = kite::load_session(kite_config.session_file);
            if (session) {
                kite::RestClient rest(tls, session->session);
                const auto started = Clock::now();
                std::size_t fetched = 0;
                for (std::size_t at = 0; at < keys.size(); at += kite::kMaxQuoteInstruments) {
                    if (at > 0) {  // Kite allows ~1 quote request per second
                        timer.expires_after(std::chrono::milliseconds(1100));
                        co_await timer.async_wait(asio::use_awaitable);
                    }
                    const auto count = std::min(kite::kMaxQuoteInstruments, keys.size() - at);
                    const auto limits =
                        co_await rest.circuit_limits(std::span(keys).subspan(at, count));
                    if (!limits) {
                        if (!reported_error) {
                            spdlog::warn("circuits: quote request failed: {}",
                                         limits.error().message);
                            reported_error = true;
                        }
                        break;
                    }
                    reported_error = false;
                    for (const auto& limit : *limits) {
                        known[limit.instrument_token] = limit;
                        publisher.publish_limits(tokens.find(limit.instrument_token), limit);
                        ++fetched;
                    }
                }
                if (fetched > 0 && full) {
                    spdlog::info("circuits: limits of {} instruments fetched in {} ms", fetched,
                                 std::chrono::duration_cast<std::chrono::milliseconds>(
                                     Clock::now() - started)
                                     .count());
                }
            }
        }
        if (Clock::now() >= next_republish) {
            next_republish = Clock::now() + std::chrono::seconds(60);
            for (const std::uint32_t token : subscriptions) {
                if (const auto found = known.find(token); found != known.end()) {
                    publisher.publish_limits(tokens.find(token), found->second);
                }
            }
        }
        timer.expires_after(std::chrono::seconds(2));
        co_await timer.async_wait(asio::use_awaitable);
    }
}

/// Runs `work` on `executor` and resumes the caller on its own executor with the result.
template <typename Work>
asio::awaitable<std::invoke_result_t<Work>> run_on(asio::any_io_executor executor, Work work) {
    using Result = std::invoke_result_t<Work>;
    co_return co_await asio::co_spawn(
        executor, [work = std::move(work)]() mutable -> asio::awaitable<Result> { co_return work(); },
        asio::use_awaitable);
}

/// ADR-0025: fills bar history for subscribed instruments that have none. Every 30 s it
/// checks the subscriptions it hasn't checked in this phase (market hours or not): during
/// market hours only today's minutes, outside them the past `days`. bars.db is touched only
/// on `store_executor`; Kite requests are ~3 per second.
asio::awaitable<void> auto_backfill_loop(const runtime::KiteConfig& kite_config, asio::ssl::context& tls,
                                         const instruments::InstrumentRegistry& registry,
                                         const kite::TokenMap& tokens,
                                         const std::vector<std::uint32_t>& subscriptions,
                                         bars::BarStore& store, asio::any_io_executor store_executor,
                                         int days, std::function<void()> on_session_rejected) {
    using Clock = std::chrono::steady_clock;
    const auto unix_now = [] { return core::Timestamp::now().ns() / 1'000'000'000; };
    asio::steady_timer timer(co_await asio::this_coro::executor);
    std::unordered_set<std::uint32_t> checked;  // tokens, this phase
    bool market = apps::in_market_hours(unix_now());
    bool paused = false;  // the session was rejected: wait for the next phase
    auto next_request = Clock::now();
    timer.expires_after(std::chrono::seconds(5));  // let the watchlists load first
    co_await timer.async_wait(asio::use_awaitable);

    while (true) {
        const std::int64_t now = unix_now();
        if (const bool in_hours = apps::in_market_hours(now); in_hours != market) {
            market = in_hours;
            checked.clear();
            paused = false;
        }
        std::vector<std::pair<std::uint32_t, core::InstrumentId>> todo;
        if (!paused) {
            for (const std::uint32_t token : subscriptions) {
                if (checked.insert(token).second) {
                    if (const auto id = tokens.find(token); id.valid()) {
                        todo.emplace_back(token, id);
                    }
                }
            }
        }
        const auto windows = co_await run_on(store_executor, [&] {
            std::vector<std::tuple<std::uint32_t, core::InstrumentId, apps::BackfillWindow>> out;
            for (const auto& [token, id] : todo) {
                const auto coverage = apps::bar_coverage(store, id.value(), now, days);
                if (!coverage) {
                    spdlog::warn("backfill: bars.db: {}", coverage.error().message);
                    break;
                }
                if (const auto window = apps::auto_backfill_window(now, *coverage, days)) {
                    out.emplace_back(token, id, *window);
                }
            }
            return out;
        });
        const auto session = windows.empty() ? std::nullopt : std::optional(kite::load_session(kite_config.session_file));
        std::size_t filled = 0;
        std::size_t empty = 0;
        std::size_t written_bars = 0;
        for (const auto& [token, id, window] : windows) {
            if (!session || !*session) {
                break;
            }
            kite::RestClient rest(tls, (*session)->session);
            std::vector<kite::Candle> candles;
            bool ok = true;
            const std::int64_t step = apps::kMaxBackfillDaysPerRequest * 86'400;
            for (std::int64_t from = window.from; from < window.to && ok; from += step) {
                timer.expires_at(next_request);
                co_await timer.async_wait(asio::use_awaitable);
                next_request = Clock::now() + std::chrono::milliseconds(350);
                const auto chunk = co_await rest.historical(token, "minute", apps::ist_text(from),
                                                            apps::ist_text(std::min(window.to, from + step - 1)), true);
                if (chunk) {
                    candles.insert(candles.end(), chunk->begin(), chunk->end());
                    continue;
                }
                ok = false;
                if (chunk.error().code == kite::KiteError::Code::AuthExpired) {
                    spdlog::warn("backfill: Kite rejected the session; paused until the next phase (run mda-login)");
                    paused = true;
                    on_session_rejected();
                } else {
                    spdlog::warn("backfill: instrument {} failed: {}", id.value(), chunk.error().message);
                }
            }
            if (paused) {
                break;
            }
            if (!ok) {
                continue;
            }
            const std::int64_t current_minute = unix_now() / 60 * 60;
            const auto bars = apps::to_bars(id, candles, current_minute);
            const auto written = co_await run_on(store_executor, [&] { return store.write(bars); });
            const auto* instrument = registry.get(id);
            const std::string symbol = instrument != nullptr ? instrument->trading_symbol : std::to_string(id.value());
            if (!written) {
                spdlog::warn("backfill: {}: writing failed: {}", symbol, written.error().message);
            } else if (bars.empty()) {  // common for far strikes that never traded
                spdlog::debug("backfill: {}: no candles at Kite", symbol);
                ++empty;
            } else {
                spdlog::info("backfill: {}: {} bars from {}", symbol, bars.size(), apps::ist_text(window.from));
                ++filled;
                written_bars += bars.size();
            }
        }
        if (!windows.empty()) {
            spdlog::info("backfill: {} of {} instruments filled with {} bars ({} had no candles at Kite)", filled,
                         windows.size(), written_bars, empty);
        }
        timer.expires_after(std::chrono::seconds(30));
        co_await timer.async_wait(asio::use_awaitable);
    }
}

asio::awaitable<void> every(std::chrono::milliseconds period, std::function<void()> work) {
    asio::steady_timer timer(co_await asio::this_coro::executor);
    while (true) {
        timer.expires_after(period);
        co_await timer.async_wait(asio::use_awaitable);
        work();
    }
}

int run(const runtime::Config& config) {
    // Session (re-read before every connection so the morning login is picked up live).
    const auto& kite_config = config.kite;
    auto session = kite::load_session(kite_config.session_file);
    if (!session) {
        spdlog::error("session: {} ({}); run mda-login", kite::to_string(session.error()),
                      kite_config.session_file.string());
        return 1;
    }
    spdlog::info("session: {} (api_key {})", session->source,
                 kite::redact(session->session.api_key));

    const auto ca_file =
        kite_config.ca_file.empty() ? kite::default_ca_file() : std::optional(kite_config.ca_file);
    if (!ca_file) {
        spdlog::error("tls: no CA bundle found; set kite.ca_file");
        return 1;
    }
    auto tls = kite::make_tls_context(*ca_file);
    if (!tls) {
        spdlog::error("tls: {}", kite::to_string(tls.error()));
        return 1;
    }
    spdlog::info("tls: CA bundle {}", ca_file->string());

    kite::RestClient rest(*tls, session->session);
    const auto profile = kite::run_sync(rest.profile());
    if (!profile) {
        spdlog::error("session: token check failed: {}; run mda-login",
                      kite::to_string(profile.error()));
        return 2;
    }
    spdlog::info("session: token valid for Kite user {}", profile->user_id);

    // Instruments: registry (permanent ids) + today's token map.
    auto loaded = apps::load_today_instruments(kite_config, rest);
    if (!loaded) {
        return 1;
    }
    auto& registry = loaded->registry;
    const kite::TokenMap& tokens = loaded->tokens;

    // Publish today's instrument list to the app DB catalog (searched by the gateway).
    // The connection stays open: watchlists are read from it while running.
    auto db = storage::Database::open(config.database.path);
    {
        if (!db) {
            spdlog::error("catalog: cannot open {}: {}", config.database.path.string(),
                          db.error().message);
            return 1;
        }
        if (auto migrated = db->migrate(appdb::migrations()); !migrated) {
            spdlog::error("catalog: migrating {}: {}", config.database.path.string(),
                          migrated.error().message);
            return 1;
        }
        const auto start = std::chrono::steady_clock::now();
        const auto synced =
            catalog::sync(*db, registry, core::Timestamp::now().ns() / 1'000'000'000);
        if (!synced) {
            spdlog::error("catalog: sync failed: {}", synced.error().message);
            return 1;
        }
        spdlog::info("catalog: {} listed instruments synced to {} in {} ms", synced->listed,
                     config.database.path.string(),
                     std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - start)
                         .count());
    }

    // Subscriptions: the static list plus every watchlist instrument (re-read while running).
    const auto static_tokens = resolve_subscriptions(config.feed, registry, tokens);
    watchlists::WatchlistService lists(*db);
    std::vector<std::uint32_t> on_watchlists;
    if (config.feed.watchlists_refresh.count() > 0) {
        if (auto ids = lists.all_instrument_ids()) {
            on_watchlists = watchlist_tokens(*ids, tokens);
        } else {
            spdlog::warn("watchlists: cannot read: {}", ids.error().message);
        }
    }
    // Option chains around ATM (ADR-0020), seeded from the quote API so windows exist before
    // the first tick (e.g. during pre-open); then the spot ticks keep them centred.
    std::vector<subscriptions::ChainSpec> chain_specs;
    for (const auto& chain : config.feed.chains) {
        const auto key = instruments::parse_contract_key(chain.spot);
        const core::InstrumentId spot = key ? registry.find(*key) : core::InstrumentId{};
        if (!spot.valid()) {
            spdlog::warn("chains: spot '{}' of {} is not a known instrument; chain skipped",
                         chain.spot, chain.underlying);
            continue;
        }
        chain_specs.push_back({.underlying = chain.underlying,
                               .spot = spot,
                               .expiries = chain.expiries,
                               .strikes = chain.strikes});
    }
    subscriptions::ChainWindows chains(registry, std::move(chain_specs));
    for (const auto& line : chains.describe()) {
        spdlog::info("chains: {}", line);
    }
    if (!chains.spots().empty()) {
        std::vector<std::string> keys;
        for (const auto spot : chains.spots()) {
            const auto* instrument = registry.get(spot);
            keys.push_back(kite::quote_key(instrument->key.exchange, instrument->key.market,
                                           instrument->trading_symbol));
        }
        if (const auto quotes = kite::run_sync(rest.circuit_limits(keys))) {
            for (const auto& quote : *quotes) {
                (void)chains.on_spot(tokens.find(quote.instrument_token), quote.last_price);
            }
        } else {
            spdlog::warn("chains: no spot quote at startup ({}); windows start with the first tick",
                         quotes.error().message);
        }
    }
    const auto log_recentres = [&] {
        for (const auto& r : chains.take_recentres()) {
            const unsigned month = static_cast<unsigned>(r.expiry.month());
            const unsigned day = static_cast<unsigned>(r.expiry.day());
            if (r.from.raw() == 0) {
                spdlog::info("chains: {} {:02}-{:02} centred at {}", r.underlying, day, month,
                             core::to_string(r.to));
            } else {
                spdlog::info("chains: {} {:02}-{:02} re-centred {} -> {}", r.underlying, day, month,
                             core::to_string(r.from), core::to_string(r.to));
            }
        }
    };
    log_recentres();
    const auto chain_tokens = [&](std::int64_t now) {
        std::vector<std::uint32_t> out;
        for (const auto id : chains.wanted(now)) {
            if (const std::uint32_t token = tokens.token_for(id); token != 0) {
                out.push_back(token);
            }
        }
        return out;
    };
    const auto unix_now = [] { return core::Timestamp::now().ns() / 1'000'000'000; };
    std::vector<std::uint32_t> chain_now = chain_tokens(unix_now());

    std::vector<std::uint32_t> subscriptions =
        merge_tokens(merge_tokens(static_tokens, on_watchlists), chain_now);
    spdlog::info(
        "subscriptions: {} instruments ({} configured, {} from watchlists, {} from chains; mode "
        "{})",
        subscriptions.size(), static_tokens.size(), on_watchlists.size(), chain_now.size(),
        config.feed.mode);
    if (subscriptions.empty()) {
        spdlog::warn("subscriptions: nothing to subscribe yet; waiting for watchlists");
    }

    // Bus.
    auto writer = bus::Writer::create_or_attach(
        {.name = config.md_bus.name, .capacity = config.md_bus.capacity_mb * 1024 * 1024});
    if (!writer) {
        spdlog::error("bus: {} : {}", config.md_bus.name, bus::to_string(writer.error()));
        return 1;
    }
    spdlog::info("bus: writer on '{}' ({} MB, epoch {})", config.md_bus.name,
                 config.md_bus.capacity_mb, writer->epoch());

    // System alerts (N6): the feed's condition, as the WebSocket last reported it, and the
    // way to raise an alert (set once the DB worker exists; nothing runs before io.run()).
    notify::FeedCondition feed_condition = notify::FeedCondition::Down;
    std::string feed_detail = "starting";
    std::function<void(notify::Alert)> raise_alert;

    // WebSocket.
    asio::io_context io;
    FeedPublisher publisher(*writer, tokens);
    std::optional<kite::KiteWebSocket> socket;
    socket.emplace(
        io.get_executor(), *tls,
        [&kite_config]() -> std::optional<kite::Session> {
            auto current = kite::load_session(kite_config.session_file);
            return current ? std::optional(current->session) : std::nullopt;
        },
        kite::WsClientConfig{.mode = config.feed.mode,
                             .stale_after = config.feed.stale_after,
                             .reconnect_initial = config.feed.reconnect_initial,
                             .reconnect_max = config.feed.reconnect_max},
        kite::WsHandlers{
            .on_binary = [&](std::span<const std::byte> frame,
                             core::Timestamp received) { publisher.on_frame(frame, received); },
            .on_text =
                [](std::string_view text) {
                    spdlog::info("kite: message {}", text.substr(0, 300));
                },
            .on_state =
                [&](kite::WsState state, std::string_view detail) {
                    const auto reconnects = static_cast<std::uint32_t>(socket->reconnects());
                    publisher.on_state(state, static_cast<std::uint32_t>(socket->token_count()),
                                       reconnects);
                    feed_condition = state == kite::WsState::Connected     ? notify::FeedCondition::Up
                                     : state == kite::WsState::AuthExpired ? notify::FeedCondition::AuthRejected
                                                                           : notify::FeedCondition::Down;
                    feed_detail = std::string(detail);
                    if (state == kite::WsState::Connected || state == kite::WsState::Connecting) {
                        spdlog::info("kite ws: {} ({})", kite::to_string(state), detail);
                    } else {
                        spdlog::warn("kite ws: {} ({})", kite::to_string(state), detail);
                    }
                }});
    socket->set_tokens(subscriptions);
    asio::co_spawn(io, socket->run(), asio::detached);

    // Liveness + periodic counters.
    asio::co_spawn(io, every(std::chrono::milliseconds(100), [&] { writer->heartbeat(); }),
                   asio::detached);
    asio::co_spawn(io,
                   every(std::chrono::seconds(1),
                         [&] {
                             publisher.republish_state(
                                 static_cast<std::uint32_t>(socket->token_count()),
                                 static_cast<std::uint32_t>(socket->reconnects()));
                         }),
                   asio::detached);

    if (config.feed.circuit_refresh.count() > 0) {
        asio::co_spawn(io,
                       circuit_limits_loop(kite_config, *tls, registry, tokens, subscriptions,
                                           publisher, config.feed.circuit_refresh),
                       asio::detached);
    }

    // Automatic backfill (ADR-0025): bars.db on its own worker thread.
    std::optional<bars::BarStore> bar_store;  // declared first: the worker stops before it goes
    asio::thread_pool bars_worker(1);
    if (config.feed.auto_backfill.count() > 0) {
        if (auto opened = bars::BarStore::open(config.engine.bars_db)) {
            bar_store.emplace(std::move(*opened));
            asio::co_spawn(io,
                           auto_backfill_loop(kite_config, *tls, registry, tokens, subscriptions, *bar_store,
                                              bars_worker.get_executor(),
                                              static_cast<int>(config.feed.auto_backfill.count()),
                                              [&] {
                                                  if (auto alert = notify::FeedAlerts{}.session_rejected(
                                                          "mda-feedd backfill", unix_now())) {
                                                      raise_alert(std::move(*alert));
                                                  }
                                              }),
                           asio::detached);
        } else {
            spdlog::warn("backfill: cannot open {}: {}; automatic backfill off", config.engine.bars_db.string(),
                         opened.error().message);
        }
    }

    // Watchlist changes: read the DB on a worker (never block the tick path), apply the
    // difference on the I/O thread.
    asio::thread_pool db_worker(1);

    // System alerts go to the outbox on the DB worker; mda-notifier delivers them.
    raise_alert = [&](notify::Alert alert) {
        spdlog::warn("alert: {}", alert.type);
        asio::post(db_worker, [&, alert = std::move(alert)] {
            if (auto queued = appdb::enqueue(*db,
                                             {.user_id = 0, .type = alert.type, .payload = alert.payload,
                                              .dedup_key = alert.dedup_key},
                                             unix_now());
                !queued) {
                spdlog::error("alert: {} not queued: {}", alert.type, queued.error().message);
            }
        });
    };
    notify::FeedAlerts feed_alerts;
    asio::co_spawn(io,
                   every(std::chrono::seconds(1),
                         [&] {
                             for (auto& alert : feed_alerts.update(feed_condition, feed_detail, unix_now())) {
                                 raise_alert(std::move(alert));
                             }
                         }),
                   asio::detached);
    // One place applies subscription changes (static + watchlists + chain windows), on the
    // I/O thread, sending Kite only the difference.
    const auto resubscribe = [&](std::string_view reason) {
        auto wanted = merge_tokens(merge_tokens(static_tokens, on_watchlists), chain_now);
        if (wanted == subscriptions) {
            return;
        }
        std::vector<std::uint32_t> added;
        std::vector<std::uint32_t> removed;
        std::ranges::set_difference(wanted, subscriptions, std::back_inserter(added));
        std::ranges::set_difference(subscriptions, wanted, std::back_inserter(removed));
        spdlog::info("subscriptions: +{} -{} from {} ({} total)", added.size(), removed.size(),
                     reason, wanted.size());
        if (wanted.size() > kMaxTokensPerConnection * 8 / 10) {
            spdlog::warn("subscriptions: {} of Kite's {} per connection in use", wanted.size(),
                         kMaxTokensPerConnection);
        }
        subscriptions = std::move(wanted);
        socket->set_tokens(subscriptions);
    };

    publisher.on_snapshot = [&](const messages::MarketSnapshot& snapshot) {
        if ((snapshot.fields & messages::kFieldLtp) != 0 &&
            chains.is_spot(snapshot.header.instrument_id)) {
            (void)chains.on_spot(snapshot.header.instrument_id, snapshot.ltp);
        }
    };
    if (!chains.spots().empty()) {
        asio::co_spawn(io,
                       every(std::chrono::seconds(1),
                             [&] {
                                 log_recentres();
                                 chain_now = chain_tokens(unix_now());
                                 resubscribe("chains");
                             }),
                       asio::detached);
    }

    if (config.feed.watchlists_refresh.count() > 0) {
        asio::co_spawn(io,
                       every(config.feed.watchlists_refresh,
                             [&] {
                                 asio::post(db_worker, [&] {
                                     auto ids = lists.all_instrument_ids();
                                     if (!ids) {
                                         spdlog::warn("watchlists: cannot read: {}",
                                                      ids.error().message);
                                         return;
                                     }
                                     auto wanted = watchlist_tokens(*ids, tokens);
                                     asio::post(io, [&, wanted = std::move(wanted)]() mutable {
                                         on_watchlists = std::move(wanted);
                                         resubscribe("watchlists");
                                     });
                                 });
                             }),
                       asio::detached);
    }
    std::uint64_t last_packets = 0;
    std::uint64_t last_frames = 0;
    asio::co_spawn(
        io,
        every(config.feed.summary_interval,
              [&] {
                  const auto& d = publisher.decoder_stats();
                  const double seconds = static_cast<double>(config.feed.summary_interval.count());
                  spdlog::info(
                      "summary: {:.0f} frames/s, {:.0f} ticks/s, {} heartbeats, {} unknown, "
                      "{} malformed, {} reconnects, bus {} msgs",
                      static_cast<double>(d.frames - last_frames) / seconds,
                      static_cast<double>(d.packets - last_packets) / seconds, d.heartbeats,
                      d.unknown_tokens, d.malformed, socket->reconnects(),
                      writer->stats().messages);
                  last_frames = d.frames;
                  last_packets = d.packets;
              }),
        asio::detached);

    asio::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait([&](const boost::system::error_code&, int) {
        spdlog::info("shutting down");
        socket->stop();
        io.stop();
    });
    io.run();
    db_worker.stop();
    db_worker.join();
    bars_worker.stop();
    bars_worker.join();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const auto config_path = runtime::config_path_from_args(argc, argv);
    const auto config = runtime::load_config(config_path);
    if (!config) {
        std::fprintf(stderr, "error: %s\n", config.error().c_str());
        return 1;
    }
    runtime::setup_logging("mda-feedd", config->logging);
    spdlog::info("mda-feedd starting (config {}, log level {}, log dir {})",
                 config->source.string(), config->logging.level,
                 config->logging.directory.string());
    const int code = run(*config);
    spdlog::info("mda-feedd stopped (exit code {})", code);
    runtime::shutdown_logging();
    return code;
}

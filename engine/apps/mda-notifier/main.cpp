// mda-notifier — delivers the notification outbox by email and Telegram (ADR-0016,
// docs/design/notifications.md).
//
//   mda-notifier [--config config/mda.yaml]
//
// One thread, two loops on one io_context:
// - every notifier.interval: expire stale notifications, fan pending ones out into
//   deliveries, send the due ones, record outcomes (deliver_due);
// - Telegram: long-poll getUpdates and answer the bot's chats (linking, phone verification).
// - every minute: the morning Kite login reminder (system_alerts.hpp), from the session
//   file's modification time only; the token is never read here.
// Secrets come from notifier.secrets_file; a missing section turns that channel off. Logs
// carry ids, channels, counts and error codes only: never addresses, tokens or message text.

#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <spdlog/spdlog.h>
#include <sys/stat.h>

#include <mda/appdb/outbox.hpp>
#include <mda/appdb/schema.hpp>
#include <mda/kite/rest_client.hpp>
#include <mda/notify/email_channel.hpp>
#include <mda/notify/outbox.hpp>
#include <mda/notify/secrets.hpp>
#include <mda/notify/system_alerts.hpp>
#include <mda/notify/telegram.hpp>
#include <mda/runtime/config.hpp>
#include <mda/runtime/logging.hpp>
#include <mda/storage/database.hpp>

namespace {

using namespace mda;
namespace asio = boost::asio;

std::int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

asio::awaitable<void> delivery_loop(notify::Outbox& outbox, std::vector<notify::Channel*> channels,
                                    std::chrono::seconds interval) {
    asio::steady_timer timer(co_await asio::this_coro::executor);
    bool reported = false;
    while (true) {
        const auto round = co_await notify::deliver_due(
            outbox, channels, unix_now(),
            [](const notify::Delivery& d, const notify::SendResult& r) {
                spdlog::warn("delivery {} ({} {}): {} — {}", d.id, d.channel, d.type,
                             r.outcome == notify::SendOutcome::Fail ? "failed" : "will retry",
                             r.error);
            });
        if (!round) {
            if (!reported) {
                spdlog::error("outbox: {}", round.error().message);
                reported = true;
            }
        } else {
            reported = false;
            const auto& s = *round;
            if (s.expired > 0) {
                spdlog::info("outbox: {} notifications expired unsent (older than 24 h)",
                             s.expired);
            }
            if (s.sent + s.retried + s.failed > 0) {
                spdlog::info("outbox: {} sent, {} to retry, {} failed", s.sent, s.retried,
                             s.failed);
            }
        }
        timer.expires_after(interval);
        co_await timer.async_wait(asio::use_awaitable);
    }
}

asio::awaitable<void> telegram_loop(notify::TelegramApi& api, notify::TelegramLinker& linker,
                                    std::string configured_bot) {
    asio::steady_timer timer(co_await asio::this_coro::executor);
    // Check the token once, and the username the dashboard's t.me link uses.
    if (const auto me = co_await api.call("getMe", {}, api.options().timeout); !me) {
        spdlog::error("telegram: the bot token was rejected or Telegram is unreachable: {}",
                      me.error().what("getMe"));
    } else if (const auto* name =
                   me->is_object() ? me->as_object().if_contains("username") : nullptr;
               name != nullptr && name->is_string()) {
        const std::string username(name->as_string());
        spdlog::info("telegram: bot @{} ready", username);
        if (username != configured_bot) {
            spdlog::warn("telegram: set notifier.telegram_bot: {} in the config (now '{}')",
                         username, configured_bot);
        }
    }
    std::int64_t offset = 0;
    while (true) {
        const auto started = std::chrono::steady_clock::now();
        const std::int64_t next = co_await notify::poll_once(api, linker, offset, unix_now());
        // A long poll that came back at once without news failed: don't spin.
        if (next == offset &&
            std::chrono::steady_clock::now() - started < std::chrono::seconds(1)) {
            timer.expires_after(std::chrono::seconds(5));
            co_await timer.async_wait(asio::use_awaitable);
        }
        offset = next;
    }
}

asio::awaitable<void> reminder_loop(storage::Database& db, std::filesystem::path session_file) {
    asio::steady_timer timer(co_await asio::this_coro::executor);
    while (true) {
        std::optional<std::int64_t> written;
        struct stat info{};
        if (::stat(session_file.c_str(), &info) == 0) {
            written = static_cast<std::int64_t>(info.st_mtime);
        }
        if (const auto alert = notify::login_reminder(unix_now(), written)) {
            // The dedup key is per day: this is a no-op after the first minute.
            if (auto queued = appdb::enqueue(db,
                                             {.user_id = 0,
                                              .type = alert->type,
                                              .payload = alert->payload,
                                              .dedup_key = alert->dedup_key},
                                             unix_now());
                !queued) {
                spdlog::error("reminder: not queued: {}", queued.error().message);
            }
        }
        timer.expires_after(std::chrono::minutes(1));
        co_await timer.async_wait(asio::use_awaitable);
    }
}

int run(const runtime::Config& config) {
    auto secrets = notify::load_notifier_secrets(config.notifier.secrets_file);
    if (!secrets) {
        spdlog::error("secrets: {} (template: config/notifier.example.json)", secrets.error());
        return 1;
    }
    if (secrets->insecure_permissions) {
        spdlog::warn("secrets: {} is readable by others; run chmod 600 on it",
                     config.notifier.secrets_file.string());
    }
    if (!secrets->telegram && !secrets->smtp) {
        spdlog::error("secrets: neither telegram nor smtp is configured; nothing to deliver");
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

    auto db = storage::Database::open(config.database.path);
    if (!db) {
        spdlog::error("database: cannot open {}: {}", config.database.path.string(),
                      db.error().message);
        return 1;
    }
    if (auto migrated = db->migrate(appdb::migrations()); !migrated) {
        spdlog::error("database: migrating {}: {}", config.database.path.string(),
                      migrated.error().message);
        return 1;
    }
    spdlog::info("database: {} at schema {}", config.database.path.string(),
                 db->schema_version().value_or(-1));

    asio::io_context io;
    notify::Outbox outbox(*db);
    std::vector<notify::Channel*> channels;
    std::optional<notify::EmailChannel> email;
    if (secrets->smtp) {
        email.emplace(*secrets->smtp, *tls);
        channels.push_back(&*email);
        spdlog::info("email: via {}:{}", secrets->smtp->host, secrets->smtp->port);
    } else {
        spdlog::warn("email: off (no smtp section in the secrets)");
    }
    std::optional<notify::TelegramApi> telegram_api;
    std::optional<notify::TelegramChannel> telegram;
    notify::TelegramLinker linker(*db);
    if (secrets->telegram) {
        telegram_api.emplace(*secrets->telegram, *tls);
        telegram.emplace(*telegram_api);
        channels.push_back(&*telegram);
        asio::co_spawn(io, telegram_loop(*telegram_api, linker, config.notifier.telegram_bot),
                       asio::detached);
        spdlog::info("telegram: on; polling for messages");
    } else {
        spdlog::warn("telegram: off (no telegram section in the secrets)");
    }
    asio::co_spawn(io, delivery_loop(outbox, channels, config.notifier.interval), asio::detached);
    asio::co_spawn(io, reminder_loop(*db, config.kite.session_file), asio::detached);

    asio::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait([&](const boost::system::error_code&, int) { io.stop(); });
    io.run();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const auto config = runtime::load_config(runtime::config_path_from_args(argc, argv));
    if (!config) {
        std::fprintf(stderr, "error: %s\n", config.error().c_str());
        return 1;
    }
    runtime::setup_logging("mda-notifier", config->logging);
    spdlog::info("mda-notifier starting (config {}, log level {})", config->source.string(),
                 config->logging.level);
    const int code = run(*config);
    spdlog::info("mda-notifier stopped (exit code {})", code);
    runtime::shutdown_logging();
    return code;
}

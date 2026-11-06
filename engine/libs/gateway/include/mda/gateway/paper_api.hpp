#pragma once

// Paper trading in the gateway (ADR-0029, docs/design/paper-trading.md §12,
// docs/api/rest.md "Paper trading"):
// - OrderSender: the gateway is the single writer of bus "ord-gw"; it gives requests their
//   client order ids and sequence numbers;
// - OrderUpdates and ExecFeed: execution reports from mda-broker (bus "exec"), for REST
//   acknowledgements and the stream's "orders" mode;
// - routes: place, modify and cancel orders, the kill switch, and the user's orders, trades,
//   positions and funds from data/paper.db.
// A POST waits for mda-broker's answer and resends the same request (same client order id)
// if none comes, because a late reader misses earlier bus messages; a repeat is never a new
// order.

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/json/object.hpp>
#include <mda/auth/auth_service.hpp>
#include <mda/autotrade/decision_store.hpp>
#include <mda/broker/instrument_catalog.hpp>
#include <mda/broker/messages.hpp>
#include <mda/broker/paper_store.hpp>
#include <mda/bus/bus.hpp>
#include <mda/gateway/http.hpp>
#include <mda/gateway/quote_cache.hpp>
#include <mda/storage/database.hpp>

namespace mda::gateway {

/// The latest execution reports, in arrival order, for acknowledgements and the stream.
class OrderUpdates {
public:
    void add(const broker::ExecutionReport& report);

    /// `user`'s reports that arrived after `seen` (a version); updates `seen`.
    [[nodiscard]] std::vector<broker::ExecutionReport> since(std::int64_t user, std::uint64_t& seen) const;
    /// The newest report of `user` after version `after` that `match` accepts.
    [[nodiscard]] std::optional<broker::ExecutionReport> find(
        std::int64_t user, std::uint64_t after,
        const std::function<bool(const broker::ExecutionReport&)>& match) const;
    [[nodiscard]] std::uint64_t version() const;

private:
    static constexpr std::size_t kKept = 4096;

    mutable std::mutex mutex_;
    std::deque<std::pair<std::uint64_t, broker::ExecutionReport>> recent_;
    std::uint64_t version_ = 0;
};

/// Reads bus "exec" (mda-broker) into OrderUpdates; waits for the bus if the broker isn't up.
/// Joins on destruction.
class ExecFeed {
public:
    ExecFeed(OrderUpdates& updates, std::string bus_name);
    ~ExecFeed();
    ExecFeed(const ExecFeed&) = delete;
    ExecFeed& operator=(const ExecFeed&) = delete;

private:
    void run(const std::stop_token& stop);

    OrderUpdates* updates_;
    std::string bus_name_;
    std::jthread thread_;  // last: started after the members it uses
};

/// The gateway's writer on "ord-gw" (one per process).
class OrderSender {
public:
    /// Creates or takes over the bus; nullptr (logged) if it can't.
    [[nodiscard]] static std::unique_ptr<OrderSender> open(const std::string& bus_name, std::size_t capacity);

    explicit OrderSender(bus::Writer writer) : writer_(std::move(writer)) {}

    /// Publishes `request` with the next sequence number.
    void send(broker::OrderRequest request);
    /// A client order id no earlier one used (milliseconds and a counter).
    [[nodiscard]] std::uint64_t next_client_order_id();

private:
    std::mutex mutex_;
    bus::Writer writer_;
    std::uint64_t sequence_ = 0;
    std::uint64_t last_id_ = 0;
};

struct PaperSettings {
    oms::Paise opening_cash = 10'00'000 * 100;  ///< what a new account starts with (shown before the first order)
    oms::Ppb futures_margin = 120'000'000;        ///< index futures
    oms::Ppb stock_futures_margin = 200'000'000;  ///< stock futures
    std::map<std::string, std::int64_t> freeze_quantity;
    broker::McxMargins mcx_margins;               ///< MCX futures margin per commodity (ADR-0031)
    std::chrono::milliseconds resend_after{500};
    std::chrono::milliseconds give_up_after{2'000};  ///< then answer 202 with the client order id
};

/// An auto-trader run as configured (config autotrader.runs), for GET /paper/auto.
struct AutoRunInfo {
    std::string id;
    std::int64_t user = 0;
    std::string strategy;
    std::string signal;
    std::string underlying;
    bool enabled = true;
    boost::json::object settings;  ///< lots, product, stop and target points, limits
};

/// GET /api/v1/paper/auto?days=30 — the signed-in user's auto-trader runs: settings,
/// position, metrics over the last `days` and today's decisions (paper.db and autotrader.db,
/// read on `paper_executor`).
void register_auto_routes(Router& router, auth::AuthService& auth, asio::any_io_executor db_executor,
                          broker::PaperStore& store, autotrade::DecisionStore& decisions,
                          asio::any_io_executor paper_executor, std::vector<AutoRunInfo> runs);

/// One execution report as JSON (REST and the stream).
[[nodiscard]] boost::json::object report_json(const broker::ExecutionReport& report);

/// Reads of paper.db run on `paper_executor` (its thread); the session and the catalog on
/// `db_executor`.
void register_paper_routes(Router& router, auth::AuthService& auth, storage::Database& db,
                           asio::any_io_executor db_executor, broker::PaperStore& store,
                           asio::any_io_executor paper_executor, OrderSender& sender,
                           const OrderUpdates& updates, const QuoteCache& quotes, PaperSettings settings);

}  // namespace mda::gateway

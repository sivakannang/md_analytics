#include <mda/broker/broker.hpp>

#include <algorithm>
#include <cstdlib>
#include <format>

#include <mda/instruments/session.hpp>

namespace mda::broker {
namespace {

constexpr Millis kDayMs = 86'400'000;
constexpr std::int64_t kMaxLegs = 10;  ///< Kite's slicing of an order above the freeze quantity
constexpr Millis kIstOffsetMs = 19'800'000;

Millis ist_day_start(Millis now) {
    return (now + kIstOffsetMs) / kDayMs * kDayMs - kIstOffsetMs;
}

std::int32_t ist_day(Millis now) {
    return static_cast<std::int32_t>((now + kIstOffsetMs) / kDayMs);
}

Millis millis(core::Timestamp t) {
    return t.ns() / 1'000'000;
}

std::string describe(const OrderRequest& r) {
    return std::format("user={} client={} action={} order={} instrument={} side={} type={} product={} qty={} "
                       "price={} trigger={} source={}",
                       r.user, r.client_order_id, static_cast<int>(r.action), r.order_id,
                       r.header.instrument_id.value(), oms::name(r.side), oms::name(r.type), oms::name(r.product),
                       r.quantity, r.price.raw(), r.trigger.raw(), static_cast<int>(r.source));
}

std::string describe(const ExecutionReport& r) {
    return std::format("user={} order={} event={} status={} filled={}/{} last={}@{} charges={} reason={}", r.user,
                       r.order_id, static_cast<int>(r.event), oms::name(r.status), r.filled, r.quantity,
                       r.last_quantity, r.last_price.raw(), r.charges, text_of(r.reason));
}

}  // namespace

Broker::Broker(BrokerConfig config, PaperStore& store, InstrumentLookup lookup, FspLookup fsp,
               PreviousCloseLookup previous_close)
    : config_(std::move(config)), store_(&store), lookup_(std::move(lookup)), venue_(config_.venue),
      risk_(config_.risk), fsp_(std::move(fsp)), previous_close_(std::move(previous_close)) {}

std::expected<void, DbError> Broker::load(Millis now) {
    // A lost accounts row would make the next order open a second account over the old
    // ledger (and its cash): put the row back first.
    const auto lost = store_->lost_accounts();
    if (!lost) {
        return std::unexpected(lost.error());
    }
    for (const auto& row : *lost) {
        if (auto done = store_->upsert_account(row); !done) {
            return std::unexpected(done.error());
        }
    }
    restored_accounts_ = lost->size();
    const auto rows = store_->accounts();
    if (!rows) {
        return std::unexpected(rows.error());
    }
    for (const auto& row : *rows) {
        auto ledger = store_->ledger(row.id);
        auto positions = store_->positions(row.id);
        if (!ledger || !positions) {
            return std::unexpected(!ledger ? ledger.error() : positions.error());
        }
        const std::size_t saved = ledger->size();
        accounts_.emplace(row.id, AccountState{.row = row,
                                               .account = oms::Account::restore(row.id, std::move(*ledger),
                                                                                std::move(*positions)),
                                               .saved_ledger = saved});
        by_user_[row.user_id] = row.id;
        if (now > 0) {
            const auto today = store_->orders(row.id, ist_day_start(now), ist_day_start(now) + kDayMs);
            if (!today) {
                return std::unexpected(today.error());
            }
            auto_day_ = ist_day(now);
            for (const auto& order : *today) {
                const auto& spec = order.order.spec;
                if (spec.source == oms::Source::Auto && order.order.filled > 0) {
                    auto_net_[{row.id, {.instrument = spec.instrument, .product = spec.product}}] +=
                        (spec.side == oms::Side::Buy ? 1 : -1) * order.order.filled;
                }
            }
        }
    }
    const auto orders = store_->live_orders();
    if (!orders) {
        return std::unexpected(orders.error());
    }
    for (const auto& row : *orders) {
        const std::int64_t id = row.order.id;
        venue_.restore(id, row.venue);
        live_.emplace(id, LiveOrder{.order = oms::Order::restore(row.order), .user = row.user_id, .margin = row.margin});
    }
    const auto max_id = store_->max_order_id();
    if (!max_id) {
        return std::unexpected(max_id.error());
    }
    next_order_id_ = *max_id + 1;
    return {};
}

// ---------------------------------------------------------------- market data

void Broker::on_snapshot(const messages::MarketSnapshot& s) {
    oms::Book& book = books_[s.header.instrument_id.value()];
    const Millis at = millis(s.receive_ts);
    if ((s.fields & messages::kFieldLtp) != 0 && s.ltp.raw() > 0 && s.ltp != book.ltp) {
        book.ltp = s.ltp;
        book.ltp_at = at;
    }
    book.updated_at = std::max(book.updated_at, at);
    if ((s.fields & messages::kFieldOhlc) != 0 && (s.fields & messages::kFieldExchangeTime) != 0 && s.close.raw() > 0) {
        kite_close_[s.header.instrument_id.value()] = {.close = s.close, .exchange_at = millis(s.exchange_ts)};
    }
}

void Broker::on_circuit(const messages::CircuitLimits& c) {
    circuits_[c.header.instrument_id.value()] = {c.lower, c.upper};
}

void Broker::on_depth(const messages::DepthSnapshot& d) {
    oms::Book& book = books_[d.header.instrument_id.value()];
    for (std::size_t i = 0; i < book.bids.size(); ++i) {
        book.bids[i] = {.price = d.bids[i].price, .quantity = d.bids[i].quantity};
        book.asks[i] = {.price = d.asks[i].price, .quantity = d.asks[i].quantity};
    }
    book.updated_at = std::max(book.updated_at, millis(d.receive_ts));
}

void Broker::on_feed(const messages::FeedStatus& status) {
    feed_ok_ = status.state == messages::FeedState::Connected;
}

oms::Book Broker::book_of(core::InstrumentId instrument, const rms::InstrumentInfo& info) const {
    const auto found = books_.find(instrument.value());
    oms::Book book = found == books_.end() ? oms::Book{} : found->second;
    if (info.contract.exchange == oms::Exchange::Mcx) {
        // Kite's MCX depth is in lots; our quantities are in units (ADR-0031).
        for (auto* side : {&book.bids, &book.asks}) {
            for (auto& level : *side) {
                level.quantity *= info.contract.lot_size;
            }
        }
    }
    book.tick = info.tick;
    book.feed_ok = feed_ok_;
    return book;
}

rms::Quote Broker::quote_of(core::InstrumentId instrument) const {
    const auto found = books_.find(instrument.value());
    if (found == books_.end()) {
        return {};
    }
    const oms::Book& b = found->second;
    const auto circuit = circuits_.find(instrument.value());
    const bool known = circuit != circuits_.end();
    return {.ltp = b.ltp,
            .best_ask = b.asks[0].quantity > 0 ? b.asks[0].price : core::Price{},
            .best_bid = b.bids[0].quantity > 0 ? b.bids[0].price : core::Price{},
            .lower_circuit = known ? circuit->second.first : core::Price{},
            .upper_circuit = known ? circuit->second.second : core::Price{}};
}

// ------------------------------------------------------------------- accounts

Broker::AccountState& Broker::ensure_account(std::int64_t user, Millis now, Changes& changes) {
    if (const auto found = by_user_.find(user); found != by_user_.end()) {
        return accounts_.at(found->second);
    }
    const std::int64_t id = accounts_.empty() ? 1 : accounts_.rbegin()->first + 1;
    AccountState state{.row = {.id = id,
                               .user_id = user,
                               .created_at = now,
                               .opening_cash = config_.opening_cash,
                               .active = true,
                               .kill_switch = false},
                       .account = oms::Account(id, config_.opening_cash, now),
                       .saved_ledger = 0};
    by_user_[user] = id;
    changes.accounts.insert(id);
    return accounts_.emplace(id, std::move(state)).first->second;
}

rms::AccountView Broker::view_of(const AccountState& state, core::InstrumentId instrument, oms::Side side,
                                 std::int64_t except_order, Millis now) const {
    const oms::Account& account = state.account;
    rms::AccountView view{.account = &account, .kill_switch = state.row.kill_switch};
    for (const auto& [key, position] : account.positions()) {
        if (position.quantity() == 0) {
            continue;
        }
        const auto info = lookup_(key.instrument);
        const auto quote = quote_of(key.instrument);
        const core::Price mark = quote.ltp.raw() > 0 ? quote.ltp : position.average_price();
        if (info) {
            view.blocked += risk_.margin(*info, position.quantity() > 0 ? oms::Side::Buy : oms::Side::Sell,
                                         std::abs(position.quantity()), position.average_price());
        }
        if (quote.ltp.raw() > 0) {
            view.unrealised += position.unrealised(quote.ltp);
        }
        const Paise notional = position.notional(mark);
        view.gross_notional += notional;
        // The auto trader's share: its net quantity, at most the position (a square-off or
        // the user may have closed some of it).
        if (const auto found = auto_net_.find({account.id(), key});
            auto_day_ == ist_day(now) && found != auto_net_.end() && (found->second > 0) == (position.quantity() > 0)) {
            const std::int64_t held = std::abs(position.quantity());
            const std::int64_t share = std::min(std::abs(found->second), held);
            view.auto_notional += notional * share / held;
            if (key.instrument == instrument) {
                view.auto_position += position.quantity() > 0 ? share : -share;
            }
        }
    }
    for (const auto& [id, live] : live_) {
        if (id == except_order || live.order.spec().account != account.id()) {
            continue;
        }
        const auto& spec = live.order.spec();
        view.blocked += spec.quantity > 0 ? live.margin * live.order.remaining() / spec.quantity : 0;
        if (spec.instrument == instrument && spec.side == side) {
            view.open_order_quantity += live.order.remaining();
        }
    }
    const Millis today = ist_day_start(now);
    Paise today_pnl = 0;
    Paise before_today = 0;
    for (const auto& entry : account.ledger()) {
        if (entry.at < today || entry.kind == oms::LedgerKind::Opening) {
            before_today += entry.amount;
        } else {
            today_pnl += entry.amount;
        }
    }
    view.opening_cash = before_today;
    view.day_pnl = today_pnl + view.unrealised;
    return view;
}

bool Broker::kill_switch(std::int64_t user) const {
    const auto found = by_user_.find(user);
    return found != by_user_.end() && accounts_.at(found->second).row.kill_switch;
}

const oms::Account* Broker::account_of_user(std::int64_t user) const {
    const auto found = by_user_.find(user);
    return found == by_user_.end() ? nullptr : &accounts_.at(found->second).account;
}

const oms::Order* Broker::live_order(std::int64_t order_id) const {
    const auto found = live_.find(order_id);
    return found == live_.end() ? nullptr : &found->second.order;
}

// -------------------------------------------------------------------- reports

ExecutionReport Broker::report(const oms::Order& order, std::int64_t user, ReportEvent event, Millis now) const {
    const auto& spec = order.spec();
    ExecutionReport r{};
    r.header = messages::make_header<ExecutionReport>(spec.instrument, 0);
    r.at = now;
    r.user = user;
    r.client_order_id = spec.client_order_id;
    r.order_id = order.id();
    r.quantity = spec.quantity;
    r.filled = order.filled();
    r.average_price = order.average_price();
    r.price = spec.price;
    r.trigger = spec.trigger;
    r.event = event;
    r.status = order.status();
    r.side = spec.side;
    r.type = order.type();
    r.product = spec.product;
    r.source = spec.source;
    set_text(r.reason, order.reason());
    return r;
}

OrderRow Broker::row_of(const oms::Order& order, std::int64_t user, Paise margin) const {
    return {.order = order.save(),
            .user_id = user,
            .margin = margin,
            .venue = venue_.state(order.id()).value_or(oms::PaperVenue::State{})};
}

void Broker::finish(std::int64_t order_id, Changes& changes) {
    const auto found = live_.find(order_id);
    if (found == live_.end() || !oms::is_final(found->second.order.status())) {
        return;
    }
    changes.final_orders.push_back(row_of(found->second.order, found->second.user, found->second.margin));
    changes.orders.erase(order_id);
    venue_.forget(order_id);
    live_.erase(found);
}

// ------------------------------------------------------------------- requests

std::expected<Reports, DbError> Broker::on_request(const OrderRequest& request, Millis now) {
    Changes changes;
    Reports reports;
    changes.events.emplace_back("request", describe(request));
    const auto reject_request = [&](rms::RejectCode code, std::string_view reason) {
        ExecutionReport r{};
        r.header = messages::make_header<ExecutionReport>(request.header.instrument_id, 0);
        r.at = now;
        r.user = request.user;
        r.client_order_id = request.client_order_id;
        r.order_id = request.order_id;
        r.quantity = request.quantity;
        r.event = ReportEvent::Rejected;
        r.status = oms::OrderStatus::Rejected;
        r.side = request.side;
        r.type = request.type;
        r.product = request.product;
        r.source = request.source;
        r.reject_code = static_cast<std::uint8_t>(code);
        set_text(r.reason, reason);
        reports.push_back(r);
    };
    if (request.user <= 0) {
        reject_request(rms::RejectCode::NotTradable, "no user");
        for (auto& r : reports) {
            changes.events.emplace_back("report", describe(r));
        }
        if (auto saved = save(changes, now); !saved) {
            return std::unexpected(saved.error());
        }
        return reports;
    }
    AccountState& state = ensure_account(request.user, now, changes);
    const std::int64_t account_id = state.row.id;

    switch (request.action) {
    case Action::New: {
        // A repeat of a known client order id gets that order's state, nothing else.
        std::optional<OrderRow> known;
        for (const auto& [id, live] : live_) {
            if (live.order.spec().account == account_id &&
                live.order.spec().client_order_id == request.client_order_id) {
                known = row_of(live.order, live.user, live.margin);
            }
        }
        if (!known) {
            auto stored = store_->order_by_client(account_id, request.client_order_id);
            if (!stored) {
                return std::unexpected(stored.error());
            }
            known = std::move(*stored);
        }
        if (known) {
            reports.push_back(report(oms::Order::restore(known->order), request.user, ReportEvent::Status, now));
            break;
        }
        oms::OrderSpec spec{.account = account_id,
                            .client_order_id = request.client_order_id,
                            .source = request.source,
                            .instrument = request.header.instrument_id,
                            .side = request.side,
                            .type = request.type,
                            .product = request.product,
                            .quantity = request.quantity,
                            .price = request.price,
                            .trigger = request.trigger,
                            .tag = std::string(text_of(request.tag))};
        const auto info = lookup_(spec.instrument);
        // One order (a leg) through risk: accepted, or rejected with `refuse` (when not
        // empty) or risk's reason; reported either way.
        const auto place = [&](oms::OrderSpec leg, const std::string& refuse, rms::RejectCode refuse_code) {
            const std::int64_t id = next_order_id_++;
            std::expected<rms::Approval, rms::Rejection> approval =
                std::unexpected(rms::Rejection{.code = refuse_code, .message = refuse});
            if (info && refuse.empty()) {
                approval = risk_.check(leg, *info, quote_of(leg.instrument),
                                       view_of(state, leg.instrument, leg.side, 0, now), now);
            }
            if (!approval) {
                const auto order = oms::Order::rejected(id, std::move(leg), approval.error().message, now);
                changes.final_orders.push_back(row_of(order, request.user, 0));
                auto r = report(order, request.user, ReportEvent::Rejected, now);
                r.reject_code = static_cast<std::uint8_t>(approval.error().code);
                reports.push_back(r);
                return;
            }
            auto order = oms::Order::accepted(id, std::move(leg), now);
            venue_.accepted(order, now);
            reports.push_back(report(order, request.user, ReportEvent::Accepted, now));
            live_.emplace(id, LiveOrder{.order = std::move(order), .user = request.user, .margin = approval->margin});
            changes.orders.insert(id);
        };
        if (!info) {
            place(std::move(spec), "unknown instrument", rms::RejectCode::NotTradable);
            break;
        }
        // A person's order above the freeze quantity is sliced as Kite does: legs of at most
        // the freeze quantity (whole lots), up to kMaxLegs, placed together. Leg k > 0 gets
        // the client order id + k << 52 (the gateway's ids stay below 2^52).
        const std::int64_t lot = std::max<std::int64_t>(1, info->contract.lot_size);
        const std::int64_t freeze = info->freeze_quantity / lot * lot;
        if (request.source == oms::Source::Ui && freeze > 0 && spec.quantity > freeze && spec.quantity % lot == 0) {
            const std::int64_t legs = (spec.quantity + freeze - 1) / freeze;
            if (legs > kMaxLegs) {
                place(std::move(spec),
                      std::format("quantity above {} legs of the freeze limit ({} × {})", kMaxLegs, kMaxLegs, freeze),
                      rms::RejectCode::FreezeQuantity);
                break;
            }
            for (std::int64_t k = 0; k < legs; ++k) {
                oms::OrderSpec leg = spec;
                leg.quantity = std::min(freeze, spec.quantity - k * freeze);
                leg.client_order_id = spec.client_order_id + (static_cast<std::uint64_t>(k) << 52);
                if (leg.tag.empty()) {
                    leg.tag = std::format("slice {}/{}", k + 1, legs);
                }
                place(std::move(leg), {}, rms::RejectCode::NotTradable);
            }
            break;
        }
        place(std::move(spec), {}, rms::RejectCode::NotTradable);
        break;
    }
    case Action::Modify:
    case Action::Cancel: {
        const auto found = live_.find(request.order_id);
        if (found == live_.end() || found->second.order.spec().account != account_id) {
            reject_request(rms::RejectCode::NotTradable, "no such open order");
            break;
        }
        LiveOrder& live = found->second;
        if (request.action == Action::Cancel) {
            (void)live.order.cancel(request.source == oms::Source::Auto ? "cancelled by the auto trader" : "cancelled by the user", now);
            reports.push_back(report(live.order, live.user, ReportEvent::Cancelled, now));
            changes.orders.insert(live.order.id());
            finish(live.order.id(), changes);
            break;
        }
        oms::OrderSpec spec = live.order.spec();
        spec.quantity = request.quantity;
        spec.price = request.price;
        spec.trigger = request.trigger;
        const auto info = lookup_(spec.instrument);
        std::expected<rms::Approval, rms::Rejection> approval =
            std::unexpected(rms::Rejection{.code = rms::RejectCode::NotTradable, .message = "unknown instrument"});
        if (info) {
            approval = risk_.check(spec, *info, quote_of(spec.instrument),
                                   view_of(state, spec.instrument, spec.side, live.order.id(), now), now);
        }
        if (!approval || !live.order.modify(spec.quantity, spec.price, spec.trigger, now)) {
            auto r = report(live.order, live.user, ReportEvent::Rejected, now);
            set_text(r.reason, "modify rejected: " + (approval ? std::string("invalid quantity") : approval.error().message));
            r.reject_code = static_cast<std::uint8_t>(approval ? rms::RejectCode::BadQuantity : approval.error().code);
            reports.push_back(r);
            break;
        }
        live.margin = approval->margin;
        venue_.accepted(live.order, now);  // a modified order looks at the book afresh
        reports.push_back(report(live.order, live.user, ReportEvent::Modified, now));
        changes.orders.insert(live.order.id());
        finish(live.order.id(), changes);
        break;
    }
    case Action::KillSwitchOn:
    case Action::KillSwitchOff: {
        const bool on = request.action == Action::KillSwitchOn;
        state.row.kill_switch = on;
        changes.accounts.insert(account_id);
        if (on) {
            std::vector<std::int64_t> cancel;
            for (const auto& [id, live] : live_) {
                if (live.order.spec().account == account_id) {
                    cancel.push_back(id);
                }
            }
            for (const auto id : cancel) {
                LiveOrder& live = live_.at(id);
                (void)live.order.cancel("kill switch", now);
                reports.push_back(report(live.order, live.user, ReportEvent::Cancelled, now));
                changes.orders.insert(id);
                finish(id, changes);
            }
        }
        ExecutionReport r{};
        r.header = messages::make_header<ExecutionReport>({}, 0);
        r.at = now;
        r.user = request.user;
        r.client_order_id = request.client_order_id;
        r.event = on ? ReportEvent::KillSwitchOn : ReportEvent::KillSwitchOff;
        r.source = request.source;
        reports.push_back(r);
        break;
    }
    case Action::Convert: {
        // A resend of a conversion already done gets its report again, never a second move.
        if (const auto done = converted_.find({request.user, request.client_order_id}); done != converted_.end()) {
            reports.push_back(done->second);
            break;
        }
        const core::InstrumentId instrument = request.header.instrument_id;
        const oms::Product to = request.product;
        const oms::Product from = to == oms::Product::Mis ? oms::Product::Nrml : oms::Product::Mis;
        const auto info = lookup_(instrument);
        if (!info) {
            reject_request(rms::RejectCode::NotTradable, "unknown instrument");
            break;
        }
        if (const auto allowed = risk_.check_convert(*info, to, request.quantity, now); !allowed) {
            reject_request(allowed.error().code, allowed.error().message);
            break;
        }
        const oms::Position* held = state.account.position(instrument, from);
        if (held == nullptr || std::abs(held->quantity()) < request.quantity) {
            reject_request(rms::RejectCode::BadQuantity,
                           std::format("the {} position is {} units", oms::name(from),
                                       held == nullptr ? 0 : std::abs(held->quantity())));
            break;
        }
        const oms::Side side = held->quantity() > 0 ? oms::Side::Buy : oms::Side::Sell;
        const core::Price average = held->average_price();
        (void)state.account.convert(instrument, from, to, request.quantity, now);
        changes.accounts.insert(account_id);
        changes.positions.insert({account_id, {.instrument = instrument, .product = from}});
        changes.positions.insert({account_id, {.instrument = instrument, .product = to}});
        ExecutionReport r{};
        r.header = messages::make_header<ExecutionReport>(instrument, 0);
        r.at = now;
        r.user = request.user;
        r.client_order_id = request.client_order_id;
        r.quantity = request.quantity;
        r.last_quantity = request.quantity;
        r.last_price = average;
        r.average_price = average;
        r.event = ReportEvent::Converted;
        r.status = oms::OrderStatus::Complete;
        r.side = side;
        r.product = to;
        r.source = request.source;
        set_text(r.reason, std::format("converted {} → {}", oms::name(from), oms::name(to)));
        if (converted_.size() >= 1'000) {
            converted_.clear();  // only resends within seconds need it
        }
        converted_[{request.user, request.client_order_id}] = r;
        reports.push_back(r);
        break;
    }
    }

    for (auto& r : reports) {
        changes.events.emplace_back("report", describe(r));
    }
    if (auto saved = save(changes, now); !saved) {
        return std::unexpected(saved.error());
    }
    for (auto& r : reports) {
        r.header.sequence = ++sequence_;
    }
    return reports;
}

// ----------------------------------------------------------------------- fills

void Broker::apply_fill(LiveOrder& live, const rms::InstrumentInfo& info, const oms::Execution& fill, Millis now,
                        Changes& changes, Reports& reports) {
    oms::Order& order = live.order;
    const auto& spec = order.spec();
    AccountState& state = accounts_.at(spec.account);
    if (spec.source == oms::Source::Auto) {
        if (const std::int32_t day = ist_day(now); day != auto_day_) {
            auto_net_.clear();  // MIS: yesterday's auto positions are squared off
            auto_day_ = day;
        }
        auto_net_[{spec.account, {.instrument = spec.instrument, .product = spec.product}}] += (spec.side == oms::Side::Buy ? 1 : -1) * fill.quantity;
    }
    const auto result = state.account.apply_fill(config_.rates, info.contract, spec.product, spec.side, fill.quantity,
                                                 fill.price, order.brokerage(), order.id(), now);
    (void)order.fill(fill.quantity, fill.price, now);
    order.add_charges(result.charges.brokerage, result.charges.total());
    changes.trades.push_back({.order_id = order.id(),
                              .account_id = spec.account,
                              .instrument = spec.instrument,
                              .side = spec.side,
                              .product = spec.product,
                              .quantity = fill.quantity,
                              .price = fill.price,
                              .charges = result.charges,
                              .realised = result.realised,
                              .at = now,
                              .entry_realised = result.entry_realised});
    changes.positions.insert({spec.account, {.instrument = spec.instrument, .product = spec.product}});
    changes.accounts.insert(spec.account);
    auto r = report(order, live.user, ReportEvent::Fill, now);
    r.last_quantity = fill.quantity;
    r.last_price = fill.price;
    r.charges = result.charges.total();
    reports.push_back(r);
}

std::expected<Reports, DbError> Broker::on_tick(Millis now) {
    Changes changes;
    Reports reports;
    close_of_day(now, changes, reports);
    std::vector<std::int64_t> done;
    for (auto& [id, live] : live_) {
        const auto info = lookup_(live.order.spec().instrument);
        if (!info) {
            continue;
        }
        const auto before = venue_.state(id);
        const auto result = venue_.evaluate(live.order, book_of(live.order.spec().instrument, *info), now);
        const auto after = venue_.state(id);
        bool changed = !before || !after || before->eligible_at != after->eligible_at ||
                       before->resting != after->resting;
        if (result.triggered) {
            (void)live.order.trigger(now);
            reports.push_back(report(live.order, live.user, ReportEvent::Triggered, now));
            changed = true;
        }
        for (const auto& fill : result.fills) {
            apply_fill(live, *info, fill, now, changes, reports);
            changed = true;
        }
        if (result.reject) {
            (void)live.order.cancel(*result.reject, now);
            reports.push_back(report(live.order, live.user, ReportEvent::Cancelled, now));
            changed = true;
        }
        if (changed) {
            changes.orders.insert(id);
        }
        if (oms::is_final(live.order.status())) {
            done.push_back(id);
        }
    }
    for (const auto id : done) {
        finish(id, changes);
    }
    if (changes.orders.empty() && changes.final_orders.empty() && changes.accounts.empty()) {
        return reports;
    }
    for (auto& r : reports) {
        changes.events.emplace_back("report", describe(r));
    }
    if (auto saved = save(changes, now); !saved) {
        return std::unexpected(saved.error());
    }
    for (auto& r : reports) {
        r.header.sequence = ++sequence_;
    }
    return reports;
}

// ---------------------------------------------------------------- close of day

void Broker::close_of_day(Millis now, Changes& changes, Reports& reports) {
    const std::int32_t today = ist_day(now);
    const std::int64_t time_of_day = (now - ist_day_start(now)) / 1000;
    const CloseOfDay& c = config_.close_of_day;
    // [0] NSE/BSE at the configured times; [1] MCX at the same distance from its close.
    const int mcx_close =
        instruments::trading_session(instruments::Exchange::Mcx, instruments::Market::Fo,
                                     instruments::ist_day(now / 1000))
            .close *
        60;
    const std::array<CloseOfDay, 2> times = {
        c, CloseOfDay{.mis_square_off = mcx_close - (c.close - c.mis_square_off),
                      .close = mcx_close,
                      .settle = mcx_close + (c.settle - c.close),
                      .mark_to_market = mcx_close + 60}};
    for (const bool mcx : {false, true}) {
        const CloseOfDay& t = times[mcx ? 1 : 0];
        // MIS: once a day between the square-off time and the close (a broker started after
        // the close leaves them for the next session's square-off: the market is gone).
        if (time_of_day >= t.mis_square_off && time_of_day < t.close && squared_off_day_[mcx] != today) {
            squared_off_day_[mcx] = today;
            square_off_mis(today, mcx, now, changes, reports);
        }
        if (time_of_day >= t.close && closed_day_[mcx] != today) {
            closed_day_[mcx] = today;
            expire_orders(ist_day_start(now) + kDayMs, mcx, "expired at the close", now, changes, reports);
        }
    }
    // Once a minute: orders left from an earlier day, and positions due to settle.
    if (now >= next_settle_check_) {
        next_settle_check_ = now + 60'000;
        catch_up(now, changes, reports);
        mark_to_previous_close(today, now, changes);
        expire_orders(ist_day_start(now), std::nullopt, "expired: placed on an earlier day", now, changes, reports);
        settle_expired(today, {time_of_day >= times[0].settle, time_of_day >= times[1].settle}, now, changes,
                       reports);
        mark_to_market(today, {time_of_day >= times[0].mark_to_market, time_of_day >= times[1].mark_to_market},
                       now, changes);
    }
}

void Broker::catch_up(Millis now, Changes& changes, Reports& reports) {
    if (!previous_close_) {
        return;
    }
    const std::int32_t today = ist_day(now);
    const Millis today_start = ist_day_start(now);
    for (auto& [account_id, state] : accounts_) {
        // Decided per instrument before acting: closing its MIS books activity of its own.
        std::vector<std::pair<oms::PositionKey, PreviousClose>> stale;
        for (const auto& [key, position] : state.account.positions()) {
            if (position.quantity() == 0 || key.product != oms::Product::Mis) {
                continue;
            }
            const auto info = lookup_(key.instrument);
            if (!info || info->expiry_days < today) {
                continue;  // expired: settle_expired settles it
            }
            const auto previous = previous_close_(key.instrument, today_start);
            if (previous && state.account.last_activity(key.instrument) < previous->ended) {
                stale.emplace_back(key, *previous);  // that day's close of day never ran for it
            }
        }
        for (const auto& [key, previous] : stale) {
            const std::int64_t quantity = state.account.position(key.instrument, key.product)->quantity();
            (void)state.account.close_at(key.instrument, key.product, previous.close, now);
            ExecutionReport r{};
            r.header = messages::make_header<ExecutionReport>(key.instrument, 0);
            r.at = now;
            r.user = state.row.user_id;
            r.event = ReportEvent::Settled;
            r.last_quantity = std::abs(quantity);
            r.last_price = previous.close;
            r.side = quantity > 0 ? oms::Side::Sell : oms::Side::Buy;
            r.product = oms::Product::Mis;
            r.source = oms::Source::System;
            set_text(r.reason, "MIS squared off at the last close (missed while down)");
            reports.push_back(r);
            changes.accounts.insert(account_id);
            changes.positions.insert({account_id, key});
        }
    }
}

void Broker::mark_to_previous_close(std::int32_t today, Millis now, Changes& changes) {
    const Millis today_start = ist_day_start(now);
    const std::int64_t time_of_day = (now - today_start) / 1000;
    for (auto& [account_id, state] : accounts_) {
        std::set<std::uint32_t> open;
        for (const auto& [key, position] : state.account.positions()) {
            if (position.quantity() != 0) {
                open.insert(key.instrument.value());
            }
        }
        for (const std::uint32_t raw : open) {
            const core::InstrumentId instrument(raw);
            const auto info = lookup_(instrument);
            if (!info || info->contract.segment != oms::Segment::Future || info->expiry_days < today) {
                continue;
            }
            const int market_open = info->market_open != 0 ? info->market_open : config_.risk.market_open;
            const int market_close = info->market_close != 0 ? info->market_close : config_.risk.market_close;
            const auto kite = kite_close_.find(raw);
            // Only in the session: Kite's close is the previous day's from the open until
            // the market shuts, and after that tonight's own mark takes over.
            if (kite == kite_close_.end() || time_of_day < market_open || time_of_day >= market_close ||
                kite->second.exchange_at < today_start + market_open * Millis{1000} ||
                state.account.last_fill(instrument) >= today_start) {
                continue;  // a fill today: its price is in the average, so tonight's mark settles it
            }
            mark_instrument(account_id, state, instrument, kite->second.close, now, changes);
        }
    }
}

void Broker::mark_instrument(std::int64_t account_id, AccountState& state, core::InstrumentId instrument,
                             core::Price close, Millis now, Changes& changes) {
    bool changed = false;
    for (const auto product : {oms::Product::Mis, oms::Product::Nrml}) {
        const auto* p = state.account.position(instrument, product);
        if (p != nullptr && p->quantity() != 0 && p->average_price() != close) {
            changed = true;
            changes.positions.insert({account_id, {.instrument = instrument, .product = product}});
        }
    }
    if (changed) {
        (void)state.account.mark_to_market(instrument, close, now);
        changes.accounts.insert(account_id);
    }
}

void Broker::mark_to_market(std::int32_t today, std::array<bool, 2> due, Millis now, Changes& changes) {
    if (!fsp_ || (!due[0] && !due[1])) {
        return;
    }
    for (auto& [account_id, state] : accounts_) {
        std::set<std::uint32_t> open;
        for (const auto& [key, position] : state.account.positions()) {
            if (position.quantity() != 0) {
                open.insert(key.instrument.value());
            }
        }
        for (const std::uint32_t raw : open) {
            const core::InstrumentId instrument(raw);
            const auto info = lookup_(instrument);
            if (!info || info->contract.segment != oms::Segment::Future || info->expiry_days <= today ||
                !due[info->contract.exchange == oms::Exchange::Mcx ? 1 : 0]) {
                continue;  // options aren't marked (the premium is paid); expiring ones settle
            }
            const auto close = fsp_(instrument, today);  // its own last close today
            if (!close) {
                continue;
            }
            mark_instrument(account_id, state, instrument, *close, now, changes);
        }
    }
}

bool Broker::on_mcx(core::InstrumentId instrument) const {
    const auto info = lookup_(instrument);
    return info && info->contract.exchange == oms::Exchange::Mcx;
}

void Broker::square_off_mis(std::int32_t today, bool mcx, Millis now, Changes& changes, Reports& reports) {
    std::vector<std::int64_t> cancel;
    for (const auto& [id, live] : live_) {
        // The traders' MIS orders; never a closing order of ours (a restart runs this again).
        if (live.order.spec().product == oms::Product::Mis && live.order.spec().source != oms::Source::System &&
            on_mcx(live.order.spec().instrument) == mcx) {
            cancel.push_back(id);
        }
    }
    for (const auto id : cancel) {
        LiveOrder& live = live_.at(id);
        (void)live.order.cancel("auto square-off", now);
        reports.push_back(report(live.order, live.user, ReportEvent::Cancelled, now));
        changes.orders.insert(id);
        finish(id, changes);
    }
    for (auto& [account_id, state] : accounts_) {
        for (const auto& [key, position] : state.account.positions()) {
            if (key.product != oms::Product::Mis || position.quantity() == 0 || on_mcx(key.instrument) != mcx) {
                continue;
            }
            // One closing order per account, day and instrument: a restart finds it in
            // paper.db (the same client order id) instead of placing another.
            const std::uint64_t client = (std::uint64_t{1} << 63) | (static_cast<std::uint64_t>(today) << 24) |
                                         key.instrument.value();
            auto known = store_->order_by_client(account_id, client);
            if (!known || *known) {
                continue;
            }
            oms::OrderSpec spec{.account = account_id,
                                .client_order_id = client,
                                .source = oms::Source::System,
                                .instrument = key.instrument,
                                .side = position.quantity() > 0 ? oms::Side::Sell : oms::Side::Buy,
                                .type = oms::OrderType::Market,
                                .product = oms::Product::Mis,
                                .quantity = std::abs(position.quantity()),
                                .price = {},
                                .trigger = {},
                                .tag = "auto square-off"};
            const std::int64_t id = next_order_id_++;
            auto order = oms::Order::accepted(id, std::move(spec), now);
            venue_.accepted(order, now);
            reports.push_back(report(order, state.row.user_id, ReportEvent::Accepted, now));
            live_.emplace(id, LiveOrder{.order = std::move(order), .user = state.row.user_id, .margin = 0});
            changes.orders.insert(id);
        }
    }
}

void Broker::expire_orders(Millis before, std::optional<bool> mcx, std::string_view reason, Millis now,
                           Changes& changes, Reports& reports) {
    std::vector<std::int64_t> expired;
    for (const auto& [id, live] : live_) {
        if (live.order.created_at() < before && (!mcx || on_mcx(live.order.spec().instrument) == *mcx)) {
            expired.push_back(id);
        }
    }
    for (const auto id : expired) {
        LiveOrder& live = live_.at(id);
        (void)live.order.cancel(std::string(reason), now);
        reports.push_back(report(live.order, live.user, ReportEvent::Cancelled, now));
        changes.orders.insert(id);
        finish(id, changes);
    }
}

void Broker::settle_expired(std::int32_t today, std::array<bool, 2> after_close, Millis now, Changes& changes,
                            Reports& reports) {
    if (!fsp_) {
        return;
    }
    for (auto& [account_id, state] : accounts_) {
        std::set<std::uint32_t> due;
        for (const auto& [key, position] : state.account.positions()) {
            if (position.quantity() != 0) {
                due.insert(key.instrument.value());
            }
        }
        for (const std::uint32_t raw : due) {
            const core::InstrumentId instrument(raw);
            const auto info = lookup_(instrument);
            const bool mcx = info && info->contract.exchange == oms::Exchange::Mcx;
            if (!info || info->expiry_days > today || (info->expiry_days == today && !after_close[mcx ? 1 : 0])) {
                continue;
            }
            // An MCX future settles on its own close (it has no underlying); an option on its
            // future's (cash at intrinsic value: ADR-0031, no devolvement into the future).
            const core::InstrumentId settle_on =
                mcx && !info->underlying.valid() ? info->contract.id : info->underlying;
            const auto fsp = fsp_(settle_on, info->expiry_days);
            if (!fsp) {
                continue;  // not known yet: the next check tries again
            }
            std::int64_t quantity = 0;
            for (const auto product : {oms::Product::Mis, oms::Product::Nrml}) {
                if (const auto* p = state.account.position(instrument, product); p != nullptr) {
                    quantity += p->quantity();
                    changes.positions.insert({account_id, {.instrument = instrument, .product = product}});
                }
            }
            (void)state.account.settle_expiry(config_.rates, info->contract, *fsp, now);
            changes.accounts.insert(account_id);
            ExecutionReport r{};
            r.header = messages::make_header<ExecutionReport>(instrument, 0);
            r.at = now;
            r.user = state.row.user_id;
            r.event = ReportEvent::Settled;
            r.last_quantity = quantity;
            r.last_price = *fsp;
            r.source = oms::Source::System;
            set_text(r.reason, "settled at expiry");
            reports.push_back(r);
        }
    }
}

std::vector<DailySummary> Broker::daily_summaries(Millis now) const {
    const Millis from = ist_day_start(now);
    std::vector<DailySummary> out;
    for (const auto& [account_id, state] : accounts_) {
        DailySummary s{.user = state.row.user_id, .cash = state.account.cash()};
        for (const auto& entry : state.account.ledger()) {
            if (entry.at < from || entry.at >= from + kDayMs) {
                continue;
            }
            if (entry.kind == oms::LedgerKind::Charges) {
                s.charges -= entry.amount;
                ++s.fills;
            } else if (entry.kind == oms::LedgerKind::Pnl || entry.kind == oms::LedgerKind::Expiry ||
                       entry.kind == oms::LedgerKind::Mtm) {
                s.realised += entry.amount;
            }
        }
        for (const auto& [key, position] : state.account.positions()) {
            s.open_positions += position.quantity() != 0 ? 1 : 0;
        }
        if (s.fills > 0 || s.realised != 0 || s.open_positions > 0) {
            out.push_back(s);
        }
    }
    return out;
}

// ------------------------------------------------------------------ persisting

std::expected<void, DbError> Broker::save(Changes& changes, Millis now) {
    return store_->in_transaction([&]() -> std::expected<void, DbError> {
        for (const auto account_id : changes.accounts) {
            AccountState& state = accounts_.at(account_id);
            if (auto done = store_->upsert_account(state.row); !done) {
                return done;
            }
            const auto& ledger = state.account.ledger();
            for (std::size_t i = state.saved_ledger; i < ledger.size(); ++i) {
                if (auto done = store_->insert_ledger(account_id, ledger[i]); !done) {
                    return done;
                }
            }
        }
        for (const auto id : changes.orders) {
            const LiveOrder& live = live_.at(id);
            if (auto done = store_->upsert_order(row_of(live.order, live.user, live.margin)); !done) {
                return done;
            }
        }
        for (const auto& row : changes.final_orders) {
            if (auto done = store_->upsert_order(row); !done) {
                return done;
            }
        }
        for (const auto& trade : changes.trades) {
            if (auto done = store_->insert_trade(trade); !done) {
                return done;
            }
        }
        for (const auto& [account_id, key] : changes.positions) {
            const auto* position = accounts_.at(account_id).account.position(key.instrument, key.product);
            if (position == nullptr) {
                continue;
            }
            if (auto done = store_->upsert_position(account_id, key, *position, now); !done) {
                return done;
            }
        }
        for (const auto& [kind, payload] : changes.events) {
            if (auto done = store_->insert_event(now, kind, payload); !done) {
                return done;
            }
        }
        return {};
    }).and_then([&]() -> std::expected<void, DbError> {
        for (const auto account_id : changes.accounts) {
            AccountState& state = accounts_.at(account_id);
            state.saved_ledger = state.account.ledger().size();
        }
        return {};
    });
}

}  // namespace mda::broker

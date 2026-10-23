#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

#include <mda/options/black76.hpp>
#include <mda/options/chain_analytics.hpp>

namespace mda::options {
namespace {

constexpr std::int64_t kIst = 5 * 3600 + 30 * 60;
constexpr double kYear = 365.0 * 86'400;

double rupees(core::Price price) {
    return price.to_double();
}

}  // namespace

double years_to_expiry(std::int64_t now, std::chrono::year_month_day expiry) {
    const auto day = std::chrono::sys_days{expiry}.time_since_epoch();
    const std::int64_t close =
        std::chrono::duration_cast<std::chrono::seconds>(day).count() + 15 * 3600 + 30 * 60 - kIst;
    return static_cast<double>(std::max<std::int64_t>(close - now, 3600)) / kYear;
}

double max_pain(const std::vector<double>& strikes, const std::vector<std::int64_t>& call_oi,
                const std::vector<std::int64_t>& put_oi) {
    double best = 0;
    double best_payout = -1;
    for (const double settle : strikes) {
        double payout = 0;
        for (std::size_t i = 0; i < strikes.size(); ++i) {
            payout += static_cast<double>(call_oi[i]) * std::max(settle - strikes[i], 0.0);
            payout += static_cast<double>(put_oi[i]) * std::max(strikes[i] - settle, 0.0);
        }
        if (best_payout < 0 || payout < best_payout) {
            best_payout = payout;
            best = settle;
        }
    }
    return best;
}

Buildup classify(double price_change, std::int64_t oi_change) {
    if (oi_change == 0 || price_change == 0) {
        return Buildup::None;
    }
    if (oi_change > 0) {
        return price_change > 0 ? Buildup::LongBuildup : Buildup::ShortBuildup;
    }
    return price_change > 0 ? Buildup::ShortCovering : Buildup::LongUnwinding;
}

ChainAnalytics::ChainAnalytics(std::vector<ChainDefinition> chains) : chains_(std::move(chains)) {
    for (const auto& chain : chains_) {
        quotes_[chain.spot.value()];
        for (const auto& expiry : chain.expiries) {
            for (const auto& option : expiry.options) {
                quotes_[option.id.value()];
            }
        }
    }
}

void ChainAnalytics::on_snapshot(const messages::MarketSnapshot& s) {
    const auto found = quotes_.find(s.header.instrument_id.value());
    if (found == quotes_.end()) {
        return;  // not part of any chain
    }
    Quote& quote = found->second;
    if ((s.fields & messages::kFieldLtp) != 0 && s.ltp.raw() > 0) {
        quote.ltp = rupees(s.ltp);
    }
    if ((s.fields & messages::kFieldOhlc) != 0 && s.close.raw() > 0) {
        quote.prev_close = rupees(s.close);
    }
    if ((s.fields & messages::kFieldOpenInterest) != 0) {
        quote.oi = s.oi.value();
    }
    if ((s.fields & messages::kFieldVolume) != 0) {
        quote.volume = s.volume.value();
    }
}

void ChainAnalytics::set_prev_oi(core::InstrumentId id, std::int64_t oi) {
    prev_oi_[id.value()] = oi;
}

std::vector<core::InstrumentId> ChainAnalytics::option_ids() const {
    std::vector<core::InstrumentId> ids;
    for (const auto& chain : chains_) {
        for (const auto& expiry : chain.expiries) {
            for (const auto& option : expiry.options) {
                ids.push_back(option.id);
            }
        }
    }
    return ids;
}

void ChainAnalytics::compute(std::int64_t now, std::vector<OptionResult>& results,
                             std::vector<ChainSummary>& summaries) const {
    const auto quote_of = [&](core::InstrumentId id) -> const Quote* {
        const auto found = quotes_.find(id.value());
        return found == quotes_.end() ? nullptr : &found->second;
    };
    for (const auto& chain : chains_) {
        const Quote* spot = quote_of(chain.spot);
        const double spot_price = spot != nullptr ? spot->ltp : 0;
        for (const auto& expiry : chain.expiries) {
            // Strike rows of what we have quotes for: strike -> (call, put)
            std::map<double, std::pair<const OptionContract*, const OptionContract*>> rows;
            bool any = false;
            for (const auto& option : expiry.options) {
                const Quote* q = quote_of(option.id);
                if (q == nullptr || q->ltp <= 0) {
                    continue;
                }
                any = true;
                auto& row = rows[rupees(option.strike)];
                (option.call ? row.first : row.second) = &option;
            }
            if (!any) {
                continue;
            }
            const double years = years_to_expiry(now, expiry.expiry);

            // Forward from put-call parity at the 3 strikes nearest spot where both sides traded
            // today.
            std::vector<std::pair<double, double>> parity;  // (distance, F)
            for (const auto& [strike, pair] : rows) {
                if (pair.first == nullptr || pair.second == nullptr) {
                    continue;
                }
                const Quote* c = quote_of(pair.first->id);
                const Quote* p = quote_of(pair.second->id);
                if (c->volume > 0 && p->volume > 0) {
                    parity.emplace_back(std::abs(strike - spot_price), strike + c->ltp - p->ltp);
                }
            }
            double forward = spot_price;
            if (!parity.empty()) {
                std::ranges::sort(parity);
                std::vector<double> nearest;
                for (std::size_t i = 0; i < std::min<std::size_t>(3, parity.size()); ++i) {
                    nearest.push_back(parity[i].second);
                }
                std::ranges::sort(nearest);
                forward = nearest[nearest.size() / 2];
            }

            ChainSummary summary{.underlying = chain.underlying,
                                 .spot = chain.spot,
                                 .expiry = expiry.expiry,
                                 .forward = forward,
                                 .years = years};
            std::vector<double> strikes;
            std::vector<std::int64_t> call_oi;
            std::vector<std::int64_t> put_oi;
            double atm_gap = -1;
            for (const auto& [strike, pair] : rows) {
                strikes.push_back(strike);
                std::int64_t row_call_oi = 0;
                std::int64_t row_put_oi = 0;
                double row_iv_sum = 0;
                int row_iv_count = 0;
                for (const OptionContract* option : {pair.first, pair.second}) {
                    if (option == nullptr) {
                        continue;
                    }
                    const Quote* q = quote_of(option->id);
                    OptionResult r{.id = option->id};
                    if (q->volume > 0 && forward > 0) {  // traded today: the price is meaningful
                        if (const auto iv =
                                implied_vol(q->ltp, forward, strike, years, option->call)) {
                            const Greeks g =
                                black76_greeks(forward, strike, years, *iv, option->call);
                            r.has_iv = true;
                            r.iv = *iv;
                            r.delta = g.delta;
                            r.gamma = g.gamma;
                            r.theta = g.theta;
                            r.vega = g.vega;
                            row_iv_sum += *iv;
                            ++row_iv_count;
                        }
                    }
                    if (const auto prev = prev_oi_.find(option->id.value());
                        prev != prev_oi_.end()) {
                        r.has_prev_oi = true;
                        r.prev_oi = prev->second;
                        if (q->prev_close > 0) {
                            r.buildup = classify(q->ltp - q->prev_close, q->oi - prev->second);
                        }
                    }
                    (option->call ? row_call_oi : row_put_oi) = q->oi;
                    results.push_back(r);
                }
                call_oi.push_back(row_call_oi);
                put_oi.push_back(row_put_oi);
                summary.call_oi += row_call_oi;
                summary.put_oi += row_put_oi;
                const double gap = std::abs(strike - (forward > 0 ? forward : spot_price));
                if (atm_gap < 0 || gap < atm_gap) {
                    atm_gap = gap;
                    summary.atm_strike = strike;
                    summary.atm_iv = row_iv_count > 0 ? row_iv_sum / row_iv_count : 0;
                }
            }
            summary.pcr = summary.call_oi > 0 ? static_cast<double>(summary.put_oi) /
                                                    static_cast<double>(summary.call_oi)
                                              : 0;
            summary.max_pain = max_pain(strikes, call_oi, put_oi);
            summaries.push_back(summary);
        }
    }
}

OptionAnalyticsMessage to_message(const OptionResult& r) {
    OptionAnalyticsMessage m{};
    m.header = messages::make_header<OptionAnalyticsMessage>(r.id, 0);
    m.iv = r.iv;
    m.delta = r.delta;
    m.gamma = r.gamma;
    m.theta = r.theta;
    m.vega = r.vega;
    m.prev_oi = r.prev_oi;
    m.buildup = r.buildup;
    m.flags = static_cast<std::uint8_t>((r.has_iv ? kHasIv : 0) | (r.has_prev_oi ? kHasPrevOi : 0));
    return m;
}

ChainSummaryMessage to_message(const ChainSummary& s) {
    ChainSummaryMessage m{};
    m.header = messages::make_header<ChainSummaryMessage>(s.spot, 0);
    m.expiry_days =
        static_cast<std::int32_t>(std::chrono::sys_days{s.expiry}.time_since_epoch().count());
    std::strncpy(m.underlying, s.underlying.c_str(), sizeof(m.underlying) - 1);
    m.forward = s.forward;
    m.atm_strike = s.atm_strike;
    m.atm_iv = s.atm_iv;
    m.pcr = s.pcr;
    m.max_pain = s.max_pain;
    m.years = s.years;
    m.call_oi = s.call_oi;
    m.put_oi = s.put_oi;
    return m;
}

ChainSummary from_message(const ChainSummaryMessage& m) {
    const std::size_t length = strnlen(m.underlying, sizeof(m.underlying));
    return {
        .underlying = std::string(m.underlying, length),
        .spot = m.header.instrument_id,
        .expiry =
            std::chrono::year_month_day{std::chrono::sys_days{std::chrono::days{m.expiry_days}}},
        .forward = m.forward,
        .atm_strike = m.atm_strike,
        .atm_iv = m.atm_iv,
        .pcr = m.pcr,
        .max_pain = m.max_pain,
        .years = m.years,
        .call_oi = m.call_oi,
        .put_oi = m.put_oi};
}

}  // namespace mda::options

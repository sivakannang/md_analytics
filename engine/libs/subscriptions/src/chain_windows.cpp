#include <algorithm>
#include <cstdio>
#include <map>

#include <mda/subscriptions/chain_windows.hpp>

namespace mda::subscriptions {
namespace {

using instruments::Kind;
using instruments::OptionType;

std::string date_text(std::chrono::year_month_day date) {
    char text[16];
    std::snprintf(text, sizeof text, "%04d-%02u-%02u", static_cast<int>(date.year()),
                  static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()));
    return text;
}

}  // namespace

ChainWindows::ChainWindows(const instruments::InstrumentRegistry& registry,
                           std::vector<ChainSpec> specs, ChainOptions options)
    : options_(options) {
    for (ChainSpec& spec : specs) {
        Chain chain{.spec = std::move(spec)};
        // expiry -> strike -> (call, put)
        std::map<std::chrono::year_month_day,
                 std::map<std::int64_t, std::pair<core::InstrumentId, core::InstrumentId>>>
            options_by_expiry;
        for (const auto& instrument : registry.all()) {
            const auto& key = instrument.key;
            if (!instrument.listed_today || key.underlying != chain.spec.underlying) {
                continue;
            }
            if (key.kind == Kind::Future) {
                chain.futures.push_back(instrument.id);
            } else if (key.kind == Kind::Option) {
                auto& pair = options_by_expiry[key.expiry][key.strike.raw()];
                (key.option_type == OptionType::Call ? pair.first : pair.second) = instrument.id;
            }
        }
        for (const auto& [date, strikes] : options_by_expiry) {
            if (static_cast<int>(chain.expiries.size()) >= chain.spec.expiries) {
                break;  // map order: nearest expiries first
            }
            Expiry expiry{.date = date};
            for (const auto& [strike, pair] : strikes) {
                expiry.strikes.push_back(core::Price::from_raw(strike));
                expiry.calls.push_back(pair.first);
                expiry.puts.push_back(pair.second);
            }
            chain.expiries.push_back(std::move(expiry));
        }
        chains_.push_back(std::move(chain));
    }
}

bool ChainWindows::on_spot(core::InstrumentId spot, core::Price price) {
    if (price.raw() <= 0) {
        return false;
    }
    bool moved = false;
    for (Chain& chain : chains_) {
        if (chain.spec.spot != spot) {
            continue;
        }
        for (Expiry& expiry : chain.expiries) {
            if (expiry.strikes.empty()) {
                continue;
            }
            // ATM: the listed strike nearest the spot.
            const auto upper = std::ranges::lower_bound(expiry.strikes, price);
            int atm = static_cast<int>(upper - expiry.strikes.begin());
            if (atm == static_cast<int>(expiry.strikes.size()) ||
                (atm > 0 && price.raw() - expiry.strikes[static_cast<std::size_t>(atm - 1)].raw() <=
                                upper->raw() - price.raw())) {
                --atm;
            }
            if (expiry.centre < 0 || std::abs(atm - expiry.centre) > options_.buffer) {
                recentres_.push_back(
                    {.underlying = chain.spec.underlying,
                     .expiry = expiry.date,
                     .from = expiry.centre < 0
                                 ? core::Price{}
                                 : expiry.strikes[static_cast<std::size_t>(expiry.centre)],
                     .to = expiry.strikes[static_cast<std::size_t>(atm)]});
                expiry.centre = atm;
                moved = true;
            }
        }
    }
    return moved;
}

std::vector<core::InstrumentId> ChainWindows::wanted(std::int64_t now) {
    std::set<std::uint32_t> window;
    for (const Chain& chain : chains_) {
        if (chain.spec.spot.valid()) {
            window.insert(chain.spec.spot.value());
        }
        for (const auto id : chain.futures) {
            window.insert(id.value());
        }
        for (const Expiry& expiry : chain.expiries) {
            if (expiry.centre < 0) {
                continue;
            }
            const int reach = chain.spec.strikes + options_.buffer;
            const int last = static_cast<int>(expiry.strikes.size()) - 1;
            for (int i = std::max(0, expiry.centre - reach);
                 i <= std::min(last, expiry.centre + reach); ++i) {
                const auto at = static_cast<std::size_t>(i);
                for (const auto id : {expiry.calls[at], expiry.puts[at]}) {
                    if (id.valid()) {
                        window.insert(id.value());
                    }
                }
            }
        }
    }
    // Strikes that just left a window stay for the grace period; ones back in leave it.
    for (const std::uint32_t id : current_) {
        if (!window.contains(id) && !held_.contains(id)) {
            held_[id] = now + options_.grace.count();
        }
    }
    std::erase_if(held_, [&](const auto& entry) {
        return window.contains(entry.first) || entry.second <= now;
    });
    current_ = window;

    std::set<std::uint32_t> all = window;
    for (const auto& [id, until] : held_) {
        all.insert(id);
    }
    std::vector<core::InstrumentId> out;
    out.reserve(all.size());
    for (const std::uint32_t id : all) {
        out.emplace_back(id);
    }
    return out;
}

bool ChainWindows::is_spot(core::InstrumentId id) const {
    return std::ranges::any_of(chains_, [&](const Chain& chain) { return chain.spec.spot == id; });
}

std::vector<core::InstrumentId> ChainWindows::spots() const {
    std::vector<core::InstrumentId> out;
    for (const Chain& chain : chains_) {
        if (chain.spec.spot.valid()) {
            out.push_back(chain.spec.spot);
        }
    }
    return out;
}

std::vector<ChainWindows::Recentre> ChainWindows::take_recentres() {
    std::vector<Recentre> out;
    out.swap(recentres_);
    return out;
}

std::vector<std::string> ChainWindows::describe() const {
    std::vector<std::string> lines;
    for (const Chain& chain : chains_) {
        std::string line = chain.spec.underlying + ": ";
        if (chain.expiries.empty()) {
            line += "no listed options";
        } else {
            line += std::to_string(chain.expiries.size()) + " expiries (";
            for (std::size_t i = 0; i < chain.expiries.size(); ++i) {
                line += (i == 0 ? "" : ", ") + date_text(chain.expiries[i].date) + " " +
                        std::to_string(chain.expiries[i].strikes.size()) + " strikes";
            }
            line += ")";
        }
        line += ", " + std::to_string(chain.futures.size()) + " futures, ATM ± " +
                std::to_string(chain.spec.strikes) + " (+" + std::to_string(options_.buffer) +
                " buffer)";
        lines.push_back(std::move(line));
    }
    return lines;
}

}  // namespace mda::subscriptions

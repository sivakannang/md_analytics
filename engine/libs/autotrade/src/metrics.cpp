#include <mda/autotrade/metrics.hpp>

#include <algorithm>
#include <cstdlib>

namespace mda::autotrade {

std::vector<RoundTrip> round_trips(std::span<const broker::TradeRow> trades) {
    std::vector<RoundTrip> out;
    RoundTrip current;
    std::int64_t position = 0;
    for (const auto& t : trades) {
        const std::int64_t signed_qty = (t.side == oms::Side::Buy ? 1 : -1) * t.quantity;
        if (position == 0) {
            current = {.opened = t.at, .quantity = signed_qty};
        }
        current.gross += t.realised;
        current.charges += t.charges.total();
        const std::int64_t before = position;
        position += signed_qty;
        if (position == 0 || (before != 0 && (before > 0) != (position > 0))) {
            current.closed = t.at;
            out.push_back(current);
            // Through zero: what's left opens the next trip at this fill.
            current = {.opened = t.at, .quantity = position};
        }
    }
    return out;
}

std::string exit_category(const Decision& d) {
    if (d.action == "stopped out") {
        return "stop";
    }
    if (d.action != "exit") {
        return {};
    }
    for (const char* category : {"signal", "target", "index stop", "max hold", "end of day", "stop lost", "flatten"}) {
        if (d.reason.starts_with(category == std::string("signal") ? "signal changed" : category)) {
            return category;
        }
    }
    return "other";
}

Metrics metrics(std::span<const broker::TradeRow> trades, std::span<const Decision> decisions) {
    Metrics m;
    for (const auto& t : trades) {
        m.gross += t.realised;
        m.charges += t.charges.total();
        m.open_position += (t.side == oms::Side::Buy ? 1 : -1) * t.quantity;
    }
    m.net = m.gross - m.charges;
    Paise cumulative = 0;
    Paise peak = 0;
    Paise won = 0;
    Paise lost = 0;
    for (const auto& trip : round_trips(trades)) {
        ++m.round_trips;
        const Paise net = trip.net();
        if (net > 0) {
            ++m.wins;
            won += net;
        } else {
            ++m.losses;
            lost += net;
            m.largest_loss = std::min(m.largest_loss, net);
        }
        cumulative += net;
        peak = std::max(peak, cumulative);
        m.max_drawdown = std::max(m.max_drawdown, peak - cumulative);
    }
    m.average_win = m.wins > 0 ? won / m.wins : 0;
    m.average_loss = m.losses > 0 ? lost / m.losses : 0;
    for (const auto& d : decisions) {
        if (const auto category = exit_category(d); !category.empty()) {
            ++m.exits[category];
        }
    }
    return m;
}

}  // namespace mda::autotrade

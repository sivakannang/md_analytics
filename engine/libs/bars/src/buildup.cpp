#include <algorithm>
#include <cmath>
#include <cstdlib>

#include <mda/bars/buildup.hpp>

namespace mda::bars {

void mark_backfill_oi_updates(std::span<Bar> minutes, core::Quantity previous_oi) {
    for (Bar& bar : minutes) {
        if (bar.source == BarSource::Kite) {
            // An unknown previous OI can't show a change; the next minute will.
            const bool changed =
                previous_oi.value() > 0 && bar.oi_close.value() > 0 && bar.oi_close != previous_oi;
            bar.oi_updates = changed ? 1 : 0;
        }
        if (bar.oi_close.value() > 0) {
            previous_oi = bar.oi_close;
        }
    }
}

BuildupClassifier::BuildupClassifier(core::Quantity lot_size, BuildupThreshold threshold)
    : lot_size_(lot_size), threshold_(threshold) {}

BuildupPoint BuildupClassifier::point(const Bar& bar) const {
    if (bar.oi_close.value() == 0) {
        return {};  // no OI for this instrument (or not seen yet)
    }
    if (bar.oi_updates == 0) {
        return {.state = Buildup::NoOiUpdate};
    }
    if (!ref_) {
        return {};
    }
    const std::int64_t d_oi = bar.oi_close.value() - ref_->oi.value();
    const auto by_fraction =
        static_cast<std::int64_t>(std::llround(threshold_.oi_fraction * static_cast<double>(ref_->oi.value())));
    const std::int64_t threshold = std::max(by_fraction, threshold_.lots * lot_size_.value());
    if (d_oi == 0 || std::abs(d_oi) < threshold) {
        return {.state = Buildup::Neutral, .oi_change = d_oi};
    }
    const bool price_up = bar.close >= ref_->close;  // unchanged price counts as up (§2.2)
    if (d_oi > 0) {
        return {.state = price_up ? Buildup::LongBuildup : Buildup::ShortBuildup, .oi_change = d_oi};
    }
    return {.state = price_up ? Buildup::ShortCovering : Buildup::LongUnwinding, .oi_change = d_oi};
}

BuildupPoint BuildupClassifier::close_point(const Bar& bar) {
    const BuildupPoint result = point(bar);
    if (bar.oi_close.value() > 0 && bar.oi_updates > 0) {
        ref_ = Reference{.close = bar.close, .oi = bar.oi_close};
    }
    return result;
}

Buildup BuildupClassifier::on_close(const Bar& bar) {
    return close_point(bar).state;
}

std::vector<BuildupPoint> classify_buildup_points(std::span<const Bar> bars, core::Quantity lot_size,
                                                  BuildupThreshold threshold) {
    BuildupClassifier classifier(lot_size, threshold);
    std::vector<BuildupPoint> points;
    points.reserve(bars.size());
    for (const Bar& bar : bars) {
        const bool forming = (bar.header.flags & kBarForming) != 0;
        points.push_back(forming ? classifier.point(bar) : classifier.close_point(bar));
    }
    return points;
}

std::vector<Buildup> classify_buildup(std::span<const Bar> bars, core::Quantity lot_size,
                                      BuildupThreshold threshold) {
    std::vector<Buildup> states;
    states.reserve(bars.size());
    for (const BuildupPoint& point : classify_buildup_points(bars, lot_size, threshold)) {
        states.push_back(point.state);
    }
    return states;
}

}  // namespace mda::bars

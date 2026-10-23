#pragma once

// OI buildup state of each bar (docs/design/oi-buildup-chart.md §2, §5, §6): price and OI
// change measured close-to-close from the last bar that carried an OI update. Works on
// 1-minute or aggregated bars alike; the gateway runs it after aggregating.

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <mda/bars/bar.hpp>
#include <mda/core/buildup.hpp>
#include <mda/core/price.hpp>
#include <mda/core/quantity.hpp>

namespace mda::bars {

using core::Buildup;

/// An OI change smaller than the larger of these is Neutral (§2.4).
struct BuildupThreshold {
    double oi_fraction = 0.001;  ///< of the reference bar's OI
    std::int64_t lots = 1;
};

/// Kite candles carry OI but no update count (§5): a backfilled minute counts as one OI
/// update when its OI differs from the previous minute's, otherwise none. `previous_oi`
/// is the OI just before the first minute (0 when unknown). Run before aggregate().
void mark_backfill_oi_updates(std::span<Bar> minutes, core::Quantity previous_oi = {});

/// A bar's state and the OI change it was measured on (from the reference bar); no change
/// when there is no OI update or nothing to compare with yet.
struct BuildupPoint {
    Buildup state = Buildup::None;
    std::optional<std::int64_t> oi_change;
};

/// Classifies one instrument's bars in order, keeping the reference bar between calls,
/// so a live stream and a history query give the same states.
class BuildupClassifier {
public:
    explicit BuildupClassifier(core::Quantity lot_size, BuildupThreshold threshold = {});

    /// State of a closed bar; a bar with an OI update becomes the next reference.
    Buildup on_close(const Bar& bar);

    /// State the bar would get if it closed now (the forming bar), reference unchanged.
    [[nodiscard]] Buildup peek(const Bar& bar) const { return point(bar).state; }

    /// The same with its OI change.
    [[nodiscard]] BuildupPoint point(const Bar& bar) const;

    /// on_close() with the OI change.
    BuildupPoint close_point(const Bar& bar);

private:
    struct Reference {
        core::Price close;
        core::Quantity oi;
    };

    core::Quantity lot_size_;
    BuildupThreshold threshold_;
    std::optional<Reference> ref_;
};

/// States of `bars` (one instrument, oldest first); bars flagged kBarForming are peeked.
[[nodiscard]] std::vector<Buildup> classify_buildup(std::span<const Bar> bars, core::Quantity lot_size,
                                                    BuildupThreshold threshold = {});

/// The same with each bar's OI change (the ΔOI bars of docs/design/instrument-page.md).
[[nodiscard]] std::vector<BuildupPoint> classify_buildup_points(std::span<const Bar> bars, core::Quantity lot_size,
                                                                BuildupThreshold threshold = {});

}  // namespace mda::bars

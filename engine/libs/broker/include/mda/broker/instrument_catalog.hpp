#pragma once

// Instruments as risk and the paper venue need them (rms::InstrumentInfo), read from the
// catalog in app.db and cached (ADR-0029). F&O only in v1: other kinds are unknown. The
// cache is cleared periodically so the day's listing and lot sizes apply.
// MCX (ADR-0031): a commodity needs a units-per-lot entry (instruments/mcx.hpp) to be
// tradable; it gets the commodity's futures margin and MCX's hours for the day.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>

#include <mda/rms/risk.hpp>
#include <mda/storage/database.hpp>

namespace mda::broker {

/// MCX futures margins (config paper.mcx_margin_percent).
struct McxMargins {
    std::map<std::string, oms::Ppb> by_commodity;
    oms::Ppb fallback = 150'000'000;  ///< 15%: a commodity not listed

    [[nodiscard]] static McxMargins from_percent(const std::map<std::string, double>& percent,
                                                 double fallback) {
        const auto ppb = [](double p) { return static_cast<oms::Ppb>(p * 10'000'000.0); };
        McxMargins out{.by_commodity = {}, .fallback = ppb(fallback)};
        for (const auto& [name, p] : percent) {
            out.by_commodity[name] = ppb(p);
        }
        return out;
    }
};

class InstrumentCatalog {
public:
    /// `freeze`: the exchange freeze quantity per underlying (config paper.freeze_quantity).
    InstrumentCatalog(storage::Database& db, std::map<std::string, std::int64_t> freeze,
                      McxMargins mcx = {});

    [[nodiscard]] std::optional<rms::InstrumentInfo> get(core::InstrumentId id);
    void clear() { cache_.clear(); }

private:
    [[nodiscard]] std::optional<rms::InstrumentInfo> read(core::InstrumentId id);

    storage::Database* db_;
    std::map<std::string, std::int64_t> freeze_;
    McxMargins mcx_;
    std::unordered_map<std::uint32_t, std::optional<rms::InstrumentInfo>> cache_;
};

}  // namespace mda::broker

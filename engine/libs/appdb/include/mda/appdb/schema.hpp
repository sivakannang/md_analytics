#pragma once

// The app database schema (ADR-0012): every table of data/app.db, as one ordered list of
// migrations. Every process that opens the app DB applies them on start, so all agree on
// the schema. Never edit an existing migration; append a new one.

#include <span>
#include <string_view>

namespace mda::appdb {

/// Migrations in order; index i upgrades schema version i -> i + 1.
[[nodiscard]] std::span<const std::string_view> migrations();

}  // namespace mda::appdb

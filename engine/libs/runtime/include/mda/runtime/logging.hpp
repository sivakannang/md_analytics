#pragma once

// Process logging (ADR-0017): spdlog, console + rotating file data/logs/<process>.log,
// asynchronous with a bounded queue that drops rather than blocks.
//
// Never log secrets, tokens, passwords, email addresses or phone numbers.

#include <string_view>

#include <spdlog/spdlog.h>

#include <mda/runtime/config.hpp>

namespace mda::runtime {

/// Installs the process-wide default logger; use spdlog::info(...) etc. afterwards.
void setup_logging(std::string_view process, const LoggingConfig& config);

/// Flushes and stops the async logger (call before exit).
void shutdown_logging();

}  // namespace mda::runtime

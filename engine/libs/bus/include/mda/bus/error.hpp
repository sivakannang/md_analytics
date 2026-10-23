#pragma once

#include <string>
#include <string_view>

namespace mda::bus {

enum class BusErrc {
    InvalidName,       ///< bus name empty, too long, or has characters outside [a-z0-9_-]
    InvalidCapacity,   ///< not a power of two or below the minimum
    NotFound,          ///< no segment with this name (writer not started yet?)
    AlreadyExists,     ///< segment name already taken (create only)
    NotReady,          ///< segment exists but is not initialized yet (or creator crashed mid-way)
    LayoutMismatch,    ///< segment was created by an incompatible build; remove it
    CapacityMismatch,  ///< existing segment has a different capacity than configured
    WriterAlreadyActive,  ///< another live process is the writer of this bus
    SystemError,          ///< an OS call failed; see BusError::system_errno
};

struct BusError {
    BusErrc code;
    int system_errno = 0;

    friend bool operator==(const BusError&, const BusError&) = default;
};

[[nodiscard]] std::string_view to_string(BusErrc code) noexcept;
/// Human-readable description including the OS error text, for logs.
[[nodiscard]] std::string to_string(const BusError& error);

}  // namespace mda::bus

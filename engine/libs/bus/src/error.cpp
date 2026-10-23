#include <cstring>

#include <mda/bus/error.hpp>

namespace mda::bus {

std::string_view to_string(BusErrc code) noexcept {
    switch (code) {
        case BusErrc::InvalidName:
            return "invalid bus name";
        case BusErrc::InvalidCapacity:
            return "invalid ring capacity";
        case BusErrc::NotFound:
            return "bus not found";
        case BusErrc::AlreadyExists:
            return "bus already exists";
        case BusErrc::NotReady:
            return "bus segment not initialized";
        case BusErrc::LayoutMismatch:
            return "bus segment layout mismatch";
        case BusErrc::CapacityMismatch:
            return "bus capacity mismatch";
        case BusErrc::WriterAlreadyActive:
            return "bus already has an active writer";
        case BusErrc::SystemError:
            return "system error";
    }
    return "unknown bus error";
}

std::string to_string(const BusError& error) {
    std::string text(to_string(error.code));
    if (error.system_errno != 0) {
        text += ": ";
        text += std::strerror(error.system_errno);
    }
    return text;
}

}  // namespace mda::bus

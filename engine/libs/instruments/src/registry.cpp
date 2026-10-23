#include <charconv>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

#include <mda/instruments/registry.hpp>

namespace mda::instruments {
namespace {

constexpr std::string_view kFileHeader = "# md_analytics instrument registry v1: id,contract_key\n";

}  // namespace

core::InstrumentId InstrumentRegistry::find(const ContractKey& key) const {
    const auto it = ids_.find(key);
    return it == ids_.end() ? core::InstrumentId{} : it->second;
}

core::InstrumentId InstrumentRegistry::assign(const ContractKey& key) {
    if (const core::InstrumentId existing = find(key); existing.valid()) {
        return existing;
    }
    const core::InstrumentId id(static_cast<std::uint32_t>(instruments_.size() + 1));
    instruments_.push_back(Instrument{.id = id, .key = key});
    ids_.emplace(key, id);
    return id;
}

const Instrument* InstrumentRegistry::get(core::InstrumentId id) const noexcept {
    if (!id.valid() || id.value() > instruments_.size()) {
        return nullptr;
    }
    return &instruments_[id.value() - 1];
}

Instrument* InstrumentRegistry::get(core::InstrumentId id) noexcept {
    return const_cast<Instrument*>(std::as_const(*this).get(id));
}

void InstrumentRegistry::begin_day() {
    for (Instrument& instrument : instruments_) {
        instrument.trading_symbol.clear();
        instrument.name.clear();
        instrument.lot_size = core::Quantity{};
        instrument.tick_size = core::Price{};
        instrument.underlying = core::InstrumentId{};
        instrument.listed_today = false;
    }
}

std::expected<RegistryLoadResult, RegistryError> InstrumentRegistry::load(
    const std::filesystem::path& path) {
    RegistryLoadResult result;
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (!std::filesystem::exists(path)) {
            return result;  // first run: empty registry
        }
        return std::unexpected(RegistryError::Io);
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string content = buffer.str();

    InstrumentRegistry& registry = result.registry;
    std::size_t start = 0;
    while (start < content.size()) {
        const std::size_t newline = content.find('\n', start);
        const bool complete = newline != std::string::npos;
        const std::string_view line(content.data() + start,
                                    (complete ? newline : content.size()) - start);
        start = complete ? newline + 1 : content.size();
        if (line.empty() || line.front() == '#') {
            continue;
        }

        const std::size_t comma = line.find(',');
        std::uint32_t id = 0;
        std::optional<ContractKey> key;
        if (comma != std::string_view::npos) {
            const auto [end, error] = std::from_chars(line.data(), line.data() + comma, id);
            if (error == std::errc{} && end == line.data() + comma) {
                key = parse_contract_key(line.substr(comma + 1));
            }
        }
        if (!key || id == 0) {
            if (!complete) {
                result.ignored_partial_line = true;  // interrupted append
                break;
            }
            return std::unexpected(RegistryError::MalformedLine);
        }
        // Lines are appended in id order, so the next id must be exactly size + 1.
        if (id != registry.instruments_.size() + 1) {
            return std::unexpected(RegistryError::DuplicateId);
        }
        if (registry.find(*key).valid()) {
            return std::unexpected(RegistryError::DuplicateKey);
        }
        registry.assign(*key);
    }
    registry.saved_count_ = registry.instruments_.size();
    return result;
}

std::expected<std::size_t, RegistryError> InstrumentRegistry::save_new(
    const std::filesystem::path& path) {
    const std::size_t pending = unsaved();
    if (pending == 0) {
        return 0;
    }
    std::string text;
    if (!std::filesystem::exists(path) || std::filesystem::file_size(path) == 0) {
        text += kFileHeader;
    }
    for (std::size_t i = saved_count_; i < instruments_.size(); ++i) {
        text += std::to_string(instruments_[i].id.value()) + "," + to_string(instruments_[i].key) +
                "\n";
    }

    if (path.has_parent_path()) {
        std::error_code ignored;
        std::filesystem::create_directories(path.parent_path(), ignored);
    }
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        return std::unexpected(RegistryError::Io);
    }
    const char* data = text.data();
    std::size_t remaining = text.size();
    bool ok = true;
    while (remaining > 0) {
        const ssize_t written = ::write(fd, data, remaining);
        if (written <= 0) {
            ok = false;
            break;
        }
        data += written;
        remaining -= static_cast<std::size_t>(written);
    }
    // IDs must survive a crash or power loss: once handed out they're used in history.
    ok = ok && ::fsync(fd) == 0;
    ::close(fd);
    if (!ok) {
        return std::unexpected(RegistryError::Io);
    }
    saved_count_ = instruments_.size();
    return pending;
}

}  // namespace mda::instruments

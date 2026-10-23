#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>

#include <mda/journal/journal.hpp>

namespace mda::journal {
namespace {

constexpr std::array<char, 8> kMagic = {'M', 'D', 'A', 'J', 'R', 'N', 'L', '1'};
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::size_t kHeaderSize = 16;
constexpr std::size_t kRecordHeaderSize = 8;
constexpr std::int64_t kIst = 5 * 3600 + 30 * 60;

std::string errno_text(std::string_view what, const std::filesystem::path& path) {
    return std::string(what) + " " + path.string() + ": " + std::strerror(errno);
}

std::array<std::byte, kHeaderSize> file_header() {
    std::array<std::byte, kHeaderSize> header{};
    std::memcpy(header.data(), kMagic.data(), kMagic.size());
    std::memcpy(header.data() + 8, &kFormatVersion, sizeof kFormatVersion);
    return header;
}

/// Offset just past the last complete record (the file must start with a valid header).
std::expected<std::uint64_t, std::string> valid_end(std::FILE* file,
                                                    const std::filesystem::path& path) {
    std::array<std::byte, kHeaderSize> header{};
    if (std::fseek(file, 0, SEEK_END) != 0) {
        return std::unexpected(errno_text("seek", path));
    }
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size < static_cast<long>(kHeaderSize) ||
        std::fread(header.data(), 1, header.size(), file) != header.size() ||
        std::memcmp(header.data(), kMagic.data(), kMagic.size()) != 0) {
        return std::unexpected(path.string() + ": not a journal file");
    }
    const auto file_size = static_cast<std::uint64_t>(size);
    std::uint64_t end = kHeaderSize;
    while (end + kRecordHeaderSize <= file_size) {
        std::uint32_t head[2];
        std::fseek(file, static_cast<long>(end), SEEK_SET);
        if (std::fread(head, 1, sizeof head, file) != sizeof head || head[0] > kMaxRecord ||
            end + kRecordHeaderSize + head[0] > file_size) {
            break;
        }
        end += kRecordHeaderSize + head[0];
    }
    return end;
}

std::string two_digits(long long value) {
    return (value < 10 ? "0" : "") + std::to_string(value);
}

}  // namespace

void Writer::Closer::operator()(std::FILE* file) const noexcept {
    std::fclose(file);
}

void Reader::Closer::operator()(std::FILE* file) const noexcept {
    std::fclose(file);
}

std::expected<Writer, std::string> Writer::open(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        return std::unexpected("create " + path.parent_path().string() + ": " + ec.message());
    }
    const bool exists = std::filesystem::exists(path, ec);
    std::FILE* file = std::fopen(path.c_str(), exists ? "r+b" : "w+b");
    if (file == nullptr) {
        return std::unexpected(errno_text("open", path));
    }
    std::uint64_t size = 0;
    if (exists) {
        const auto end = valid_end(file, path);
        if (!end) {
            std::fclose(file);
            return std::unexpected(end.error());
        }
        size = *end;
        // Cut a torn last record (a crash mid-write) so new records follow a complete one.
        if (::ftruncate(::fileno(file), static_cast<off_t>(size)) != 0) {
            std::fclose(file);
            return std::unexpected(errno_text("truncate", path));
        }
        std::fseek(file, 0, SEEK_END);
    } else {
        const auto header = file_header();
        if (std::fwrite(header.data(), 1, header.size(), file) != header.size()) {
            std::fclose(file);
            return std::unexpected(errno_text("write", path));
        }
        size = kHeaderSize;
    }
    std::setvbuf(file, nullptr, _IOFBF, 1 << 20);
    return Writer(path, file, size);
}

std::expected<void, std::string> Writer::append(std::uint32_t type,
                                                std::span<const std::byte> payload) {
    if (!file_) {
        return std::unexpected("journal closed");
    }
    if (payload.size() > kMaxRecord) {
        return std::unexpected("record too large");
    }
    const std::uint32_t head[2] = {static_cast<std::uint32_t>(payload.size()), type};
    if (std::fwrite(head, 1, sizeof head, file_.get()) != sizeof head ||
        std::fwrite(payload.data(), 1, payload.size(), file_.get()) != payload.size()) {
        return std::unexpected(errno_text("write", path_));
    }
    size_ += sizeof head + payload.size();
    return {};
}

std::expected<void, std::string> Writer::append_gap(Gap gap) {
    std::array<std::byte, 16> payload{};
    std::memcpy(payload.data(), &gap.time_ns, 8);
    std::memcpy(payload.data() + 8, &gap.lost_bytes, 8);
    return append(kGapType, payload);
}

std::expected<void, std::string> Writer::flush() {
    if (file_ && std::fflush(file_.get()) != 0) {
        return std::unexpected(errno_text("flush", path_));
    }
    return {};
}

std::expected<void, std::string> Writer::close() {
    if (!file_) {
        return {};
    }
    auto flushed = flush();
    if (flushed && ::fsync(::fileno(file_.get())) != 0) {
        flushed = std::unexpected(errno_text("fsync", path_));
    }
    file_.reset();
    return flushed;
}

std::expected<Reader, std::string> Reader::open(const std::filesystem::path& path) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return std::unexpected(errno_text("open", path));
    }
    std::array<std::byte, kHeaderSize> header{};
    if (std::fread(header.data(), 1, header.size(), file) != header.size() ||
        std::memcmp(header.data(), kMagic.data(), kMagic.size()) != 0) {
        std::fclose(file);
        return std::unexpected(path.string() + ": not a journal file");
    }
    return Reader(file);
}

std::optional<Record> Reader::next() {
    std::uint32_t head[2];
    const std::size_t got = std::fread(head, 1, sizeof head, file_.get());
    if (got == 0) {
        return std::nullopt;
    }
    if (got != sizeof head || head[0] > kMaxRecord) {
        torn_ = true;
        return std::nullopt;
    }
    buffer_.resize(head[0]);
    if (std::fread(buffer_.data(), 1, head[0], file_.get()) != head[0]) {
        torn_ = true;
        return std::nullopt;
    }
    return Record{.type = head[1], .payload = buffer_};
}

Gap decode_gap(std::span<const std::byte> payload) {
    Gap gap;
    if (payload.size() >= 16) {
        std::memcpy(&gap.time_ns, payload.data(), 8);
        std::memcpy(&gap.lost_bytes, payload.data() + 8, 8);
    }
    return gap;
}

std::filesystem::path day_dir(const std::filesystem::path& root, std::int64_t unix_seconds) {
    const std::chrono::sys_seconds local{std::chrono::seconds{unix_seconds + kIst}};
    const std::chrono::year_month_day ymd{std::chrono::floor<std::chrono::days>(local)};
    return root / (std::to_string(static_cast<int>(ymd.year())) + "-" +
                   two_digits(static_cast<unsigned>(ymd.month())) + "-" +
                   two_digits(static_cast<unsigned>(ymd.day())));
}

std::filesystem::path hour_file(const std::filesystem::path& root, std::int64_t unix_seconds) {
    const std::int64_t hour = (((unix_seconds + kIst) % 86'400) + 86'400) % 86'400 / 3600;
    return day_dir(root, unix_seconds) / (two_digits(hour) + ".mdj");
}

std::vector<std::filesystem::path> hour_files(const std::filesystem::path& day) {
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(day, ec)) {
        if (entry.path().extension() == ".mdj") {
            files.push_back(entry.path());
        }
    }
    std::ranges::sort(files);
    return files;
}

std::size_t prune(const std::filesystem::path& root, std::int64_t unix_seconds, int keep_days) {
    // Day folders are named YYYY-MM-DD, so names compare like dates.
    const std::string oldest_kept =
        day_dir(root, unix_seconds - std::int64_t{keep_days} * 86'400).filename().string();
    std::size_t removed = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        const std::string name = entry.path().filename().string();
        if (entry.is_directory() && name.size() == 10 && name[4] == '-' && name < oldest_kept) {
            std::filesystem::remove_all(entry.path(), ec);
            removed += ec ? 0 : 1;
        }
    }
    return removed;
}

}  // namespace mda::journal

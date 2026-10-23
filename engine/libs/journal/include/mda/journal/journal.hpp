#pragma once

// The tick journal's files (ADR-0026): data/journal/<IST date>/<HH>.mdj, one per IST hour.
// A file is a 16-byte header ("MDAJRNL1", format version, reserved) and records
// `u32 length · u32 bus type · payload`, the bus payload unchanged. A torn last record
// (a crash mid-write) is ignored by readers and cut off when a writer reopens the file.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mda::journal {

/// Journal-only record: the recorder was lapped. Payload: i64 time (ns), u64 lost bytes.
inline constexpr std::uint32_t kGapType = 0x7FFF0001;
inline constexpr std::uint32_t kMaxRecord = 16 * 1024 * 1024;

struct Gap {
    std::int64_t time_ns = 0;
    std::uint64_t lost_bytes = 0;
};

struct Record {
    std::uint32_t type = 0;
    std::span<const std::byte> payload;  ///< valid until the next read
};

class Writer {
public:
    /// Opens `path` for appending (creating it and its folders), cutting a torn tail.
    [[nodiscard]] static std::expected<Writer, std::string> open(const std::filesystem::path& path);

    [[nodiscard]] std::expected<void, std::string> append(std::uint32_t type, std::span<const std::byte> payload);
    [[nodiscard]] std::expected<void, std::string> append_gap(Gap gap);
    /// Hands buffered records to the OS.
    [[nodiscard]] std::expected<void, std::string> flush();
    /// Flushes, fsyncs and closes; the writer is unusable afterwards.
    [[nodiscard]] std::expected<void, std::string> close();

    [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    struct Closer {
        void operator()(std::FILE* file) const noexcept;
    };
    Writer(std::filesystem::path path, std::FILE* file, std::uint64_t size)
        : path_(std::move(path)), file_(file), size_(size) {}

    std::filesystem::path path_;
    std::unique_ptr<std::FILE, Closer> file_;
    std::uint64_t size_ = 0;
};

class Reader {
public:
    [[nodiscard]] static std::expected<Reader, std::string> open(const std::filesystem::path& path);

    /// The next record; nothing at the end of the file or at a torn last record.
    [[nodiscard]] std::optional<Record> next();

    /// The file ended in the middle of a record.
    [[nodiscard]] bool torn() const noexcept { return torn_; }

private:
    struct Closer {
        void operator()(std::FILE* file) const noexcept;
    };
    explicit Reader(std::FILE* file) : file_(file) {}

    std::unique_ptr<std::FILE, Closer> file_;
    std::vector<std::byte> buffer_;
    bool torn_ = false;
};

[[nodiscard]] Gap decode_gap(std::span<const std::byte> payload);

/// <root>/<IST date> and <root>/<IST date>/<HH>.mdj for a Unix time.
[[nodiscard]] std::filesystem::path day_dir(const std::filesystem::path& root, std::int64_t unix_seconds);
[[nodiscard]] std::filesystem::path hour_file(const std::filesystem::path& root, std::int64_t unix_seconds);

/// A day's hour files, in time order.
[[nodiscard]] std::vector<std::filesystem::path> hour_files(const std::filesystem::path& day);

/// Deletes day folders older than `keep_days` (by their name); returns how many.
std::size_t prune(const std::filesystem::path& root, std::int64_t unix_seconds, int keep_days);

}  // namespace mda::journal

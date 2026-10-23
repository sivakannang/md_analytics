#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <mda/journal/journal.hpp>

namespace mda::journal {
namespace {

constexpr std::int64_t kThursday1015 = 1'792'607'400 + 10 * 3600 + 15 * 60;  // 2026-10-22 10:15 IST

std::span<const std::byte> bytes(const std::string& text) {
    return std::as_bytes(std::span(text.data(), text.size()));
}

std::string text(std::span<const std::byte> payload) {
    return std::string(reinterpret_cast<const char*>(payload.data()), payload.size());
}

class JournalTest : public ::testing::Test {
protected:
    JournalTest() {
        root_ =
            std::filesystem::temp_directory_path() /
            ("mda_journal_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
             "_" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(root_);
    }
    ~JournalTest() override { std::filesystem::remove_all(root_); }

    std::vector<std::pair<std::uint32_t, std::string>> read_all(const std::filesystem::path& path,
                                                                bool* torn = nullptr) {
        auto reader = Reader::open(path).value();
        std::vector<std::pair<std::uint32_t, std::string>> out;
        while (const auto record = reader.next()) {
            out.emplace_back(record->type, text(record->payload));
        }
        if (torn != nullptr) {
            *torn = reader.torn();
        }
        return out;
    }

    std::filesystem::path root_;
};

TEST_F(JournalTest, NamesFilesByIstDayAndHour) {
    EXPECT_EQ(hour_file(root_, kThursday1015), root_ / "2026-10-22" / "10.mdj");
    EXPECT_EQ(hour_file(root_, kThursday1015 + 14 * 3600),
              root_ / "2026-10-23" / "00.mdj");  // 00:15 IST
}

TEST_F(JournalTest, WritesAndReadsRecordsInOrder) {
    const auto path = hour_file(root_, kThursday1015);
    {
        auto writer = Writer::open(path).value();
        ASSERT_TRUE(writer.append(1, bytes("frame one")).has_value());
        ASSERT_TRUE(writer.append(4, bytes("status")).has_value());
        ASSERT_TRUE(writer.append_gap({.time_ns = 42, .lost_bytes = 4096}).has_value());
        ASSERT_TRUE(writer.close().has_value());
    }
    auto reader = Reader::open(path).value();
    auto record = reader.next();
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->type, 1u);
    EXPECT_EQ(text(record->payload), "frame one");
    EXPECT_EQ(reader.next()->type, 4u);
    const auto gap = reader.next();
    ASSERT_EQ(gap->type, kGapType);
    EXPECT_EQ(decode_gap(gap->payload).lost_bytes, 4096u);
    EXPECT_FALSE(reader.next().has_value());
    EXPECT_FALSE(reader.torn());
}

TEST_F(JournalTest, ReopeningAppendsAndCutsATornTail) {
    const auto path = hour_file(root_, kThursday1015);
    {
        auto writer = Writer::open(path).value();
        ASSERT_TRUE(writer.append(1, bytes("first")).has_value());
        ASSERT_TRUE(writer.close().has_value());
    }
    {  // a crash mid-record: a header promising 100 bytes, then 3
        std::ofstream out(path, std::ios::binary | std::ios::app);
        const std::uint32_t head[2] = {100, 1};
        out.write(reinterpret_cast<const char*>(head), sizeof head);
        out.write("abc", 3);
    }
    bool torn = false;
    EXPECT_EQ(read_all(path, &torn).size(), 1u);
    EXPECT_TRUE(torn);

    auto writer = Writer::open(path).value();
    ASSERT_TRUE(writer.append(1, bytes("second")).has_value());
    ASSERT_TRUE(writer.close().has_value());
    const auto records = read_all(path, &torn);
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[1].second, "second");
    EXPECT_FALSE(torn);
}

TEST_F(JournalTest, RejectsOtherFiles) {
    std::filesystem::create_directories(root_);
    std::ofstream(root_ / "x.mdj") << "not a journal at all";
    EXPECT_FALSE(Reader::open(root_ / "x.mdj").has_value());
    EXPECT_FALSE(Writer::open(root_ / "x.mdj").has_value());
}

TEST_F(JournalTest, ListsHoursAndPrunesOldDays) {
    for (const int hours : {0, 1, -48, -24 * 100}) {
        auto writer = Writer::open(hour_file(root_, kThursday1015 + hours * 3600)).value();
        ASSERT_TRUE(writer.close().has_value());
    }
    const auto today = hour_files(day_dir(root_, kThursday1015));
    ASSERT_EQ(today.size(), 2u);
    EXPECT_EQ(today[0].filename(), "10.mdj");
    EXPECT_EQ(today[1].filename(), "11.mdj");
    EXPECT_EQ(prune(root_, kThursday1015, 90), 1u);  // only the 100-day-old folder
    EXPECT_TRUE(std::filesystem::exists(day_dir(root_, kThursday1015 - 48 * 3600)));
}

}  // namespace
}  // namespace mda::journal

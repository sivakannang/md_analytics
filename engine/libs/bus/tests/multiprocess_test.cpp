// Cross-process tests for the bus (design §11, Unit 3).
//
// ThreadSanitizer cannot see across processes, so correctness here is checked end to
// end: every payload carries (writer id, sequence) and bytes derived from them. Reader
// processes verify every delivered message is intact and in order, including while
// being lapped and across a writer killed with SIGKILL mid-publish.
//
// Child processes never touch gtest: they report through a pipe and _exit().

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <mda/bus/bus.hpp>

namespace mda::bus {
namespace {

constexpr MsgType kData = 1;
constexpr MsgType kEnd = 2;
constexpr std::size_t kCapacity = 4 * 1024 * 1024;
constexpr unsigned kChildTimeoutSeconds = 60;  // a hung child is killed by SIGALRM
constexpr int kReceiveTimeoutMs = 90'000;

#if defined(__SANITIZE_THREAD__)
constexpr bool kThreadSanitizer = true;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
constexpr bool kThreadSanitizer = true;
#else
constexpr bool kThreadSanitizer = false;
#endif
#else
constexpr bool kThreadSanitizer = false;
#endif

// --------------------------------------------------------------------------- payloads

std::byte fill_byte(std::uint64_t seq, std::uint64_t writer, std::size_t index) {
    return static_cast<std::byte>((seq * 131 + writer * 17 + index * 7) & 0xFF);
}

std::size_t payload_size(std::uint64_t seq) {
    return 16 + (seq * 37) % 241;  // 16..256 bytes
}

/// [u64 seq][u64 writer id][bytes derived from both]
std::vector<std::byte> make_payload(std::uint64_t seq, std::uint64_t writer) {
    std::vector<std::byte> payload(payload_size(seq));
    for (std::size_t i = 16; i < payload.size(); ++i) {
        payload[i] = fill_byte(seq, writer, i);
    }
    std::memcpy(payload.data(), &seq, 8);
    std::memcpy(payload.data() + 8, &writer, 8);
    return payload;
}

struct Decoded {
    std::uint64_t seq = 0;
    std::uint64_t writer = 0;
    bool intact = false;
};

Decoded decode(std::span<const std::byte> payload) {
    Decoded d;
    if (payload.size() < 16) {
        return d;
    }
    std::memcpy(&d.seq, payload.data(), 8);
    std::memcpy(&d.writer, payload.data() + 8, 8);
    d.intact = payload.size() == payload_size(d.seq);
    for (std::size_t i = 16; d.intact && i < payload.size(); ++i) {
        d.intact = payload[i] == fill_byte(d.seq, d.writer, i);
    }
    return d;
}

// --------------------------------------------------------------------------- processes

struct Pipe {
    int read_fd = -1;
    int write_fd = -1;

    Pipe() {
        int fds[2];
        if (::pipe(fds) == 0) {
            read_fd = fds[0];
            write_fd = fds[1];
        }
    }
    ~Pipe() {
        ::close(read_fd);
        ::close(write_fd);
    }
    void send(const void* data, std::size_t size) const {
        auto* bytes = static_cast<const char*>(data);
        while (size > 0) {
            const ssize_t n = ::write(write_fd, bytes, size);
            if (n <= 0) {
                return;
            }
            bytes += n;
            size -= static_cast<std::size_t>(n);
        }
    }
    /// Reads exactly `size` bytes; false on timeout. (The parent holds the write end too,
    /// so a dead child never produces EOF — the timeout is what prevents a hang.)
    bool receive(void* data, std::size_t size) const {
        auto* bytes = static_cast<char*>(data);
        while (size > 0) {
            pollfd waiting{.fd = read_fd, .events = POLLIN, .revents = 0};
            if (::poll(&waiting, 1, kReceiveTimeoutMs) <= 0) {
                return false;
            }
            const ssize_t n = ::read(read_fd, bytes, size);
            if (n <= 0) {
                return false;
            }
            bytes += n;
            size -= static_cast<std::size_t>(n);
        }
        return true;
    }
};

struct ReaderResult {
    std::uint64_t delivered = 0;
    std::uint64_t laps = 0;
    std::uint64_t corrupt = 0;       // payload did not match its (seq, writer) pattern
    std::uint64_t out_of_order = 0;  // seq not increasing within a writer, or writer went back
    std::uint64_t last_seq = 0;
    std::uint64_t last_writer = 0;
    bool ended = false;
};

struct ReaderSpec {
    std::string name;
    /// Simulates a consumer that stalls: after its first message it stops reading until
    /// the writer is two full rings ahead, so it is guaranteed to be lapped whatever the
    /// relative speed of writer and reader (sanitizers slow them very differently).
    bool stall = false;
};

/// Blocks until the bus tail is at least two ring capacities past `position`, or the
/// writer has stopped publishing.
void wait_until_lapped(const std::string& bus, std::uint64_t position) {
    std::uint64_t previous_tail = 0;
    for (int idle = 0; idle < 200;) {
        const auto snapshot = inspect_bus(bus);
        if (!snapshot || snapshot->tail >= position + 2 * kCapacity) {
            return;
        }
        idle = snapshot->tail == previous_tail ? idle + 1 : 0;
        previous_tail = snapshot->tail;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

ReaderResult run_reader(const std::string& bus, const ReaderSpec& spec, const Pipe& ready) {
    ReaderResult result;
    auto reader = Reader::attach(bus, {.name = spec.name});
    if (!reader) {
        ::_exit(10);
    }
    const char signal_ready = 1;
    ready.send(&signal_ready, 1);

    auto handler = [&](MsgType type, std::span<const std::byte> payload) {
        if (type == kEnd) {
            result.ended = true;
            return;
        }
        const Decoded d = decode(payload);
        if (type != kData || !d.intact) {
            ++result.corrupt;
            return;
        }
        const bool in_order = result.delivered == 0 || d.writer > result.last_writer ||
                              (d.writer == result.last_writer && d.seq > result.last_seq);
        if (!in_order) {
            ++result.out_of_order;
        }
        result.last_seq = d.seq;
        result.last_writer = d.writer;
        ++result.delivered;
        if (spec.stall && result.delivered == 1) {
            wait_until_lapped(bus, reader->position());
        }
    };
    while (!result.ended) {
        const PollResult polled = reader->poll(handler);
        if (polled.status == PollStatus::Lapped) {
            ++result.laps;
        }
    }
    return result;  // Reader destroyed here: its slot is released
}

struct Child {
    pid_t pid = -1;
};

/// Forks a reader process; it signals `ready` once attached and writes its
/// ReaderResult to `results` when it sees the end marker.
Child spawn_reader(const std::string& bus, const ReaderSpec& spec, const Pipe& ready,
                   const Pipe& results) {
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::alarm(kChildTimeoutSeconds);
        const ReaderResult result = run_reader(bus, spec, ready);
        results.send(&result, sizeof(result));
        ::_exit(0);
    }
    return {pid};
}

/// Forks a writer process that publishes (seq, writer id) forever until killed.
Child spawn_endless_writer(const std::string& bus, std::uint64_t writer_id, const Pipe& ready) {
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::alarm(kChildTimeoutSeconds);
        auto writer = Writer::create_or_attach({.name = bus, .capacity = kCapacity});
        if (!writer) {
            ::_exit(10);
        }
        const char signal_ready = 1;
        ready.send(&signal_ready, 1);
        for (std::uint64_t seq = 0;; ++seq) {
            (void)writer->publish(kData, make_payload(seq, writer_id));
        }
    }
    return {pid};
}

void wait_ready(const Pipe& ready, int count) {
    for (int i = 0; i < count; ++i) {
        char c = 0;
        ASSERT_TRUE(ready.receive(&c, 1)) << "child failed to start";
    }
}

/// Waits for a child; true if it exited normally with status 0.
bool exited_cleanly(Child child) {
    int status = 0;
    return ::waitpid(child.pid, &status, 0) == child.pid && WIFEXITED(status) &&
           WEXITSTATUS(status) == 0;
}

void publish_messages(Writer& writer, std::uint64_t count, std::uint64_t writer_id) {
    for (std::uint64_t seq = 0; seq < count; ++seq) {
        ASSERT_TRUE(writer.publish(kData, make_payload(seq, writer_id)).has_value());
    }
}

void publish_end(Writer& writer) {
    ASSERT_TRUE(writer.publish(kEnd, {}).has_value());
}

void expect_valid(const ReaderResult& r, const std::string& name) {
    EXPECT_TRUE(r.ended) << name;
    EXPECT_EQ(r.corrupt, 0u) << name;
    EXPECT_EQ(r.out_of_order, 0u) << name;
    EXPECT_GT(r.delivered, 0u) << name;
}

class MultiProcessBusTest : public ::testing::Test {
protected:
    void SetUp() override { name_ = "mp" + std::to_string(::getpid()); }
    void TearDown() override { (void)remove_bus(name_); }

    std::string name_;
};

// --------------------------------------------------------------------------- tests

TEST_F(MultiProcessBusTest, ThreeReaderProcessesReceiveIntactOrderedMessages) {
    // ~150 MB of records: dozens of wraps of the 4 MB ring. Fewer under TSan, which
    // slows every memory access by an order of magnitude; correctness is checked the same.
    constexpr std::uint64_t kMessages = kThreadSanitizer ? 200'000 : 1'000'000;
    auto writer = Writer::create_or_attach({.name = name_, .capacity = kCapacity});
    ASSERT_TRUE(writer.has_value()) << to_string(writer.error());

    const std::vector<ReaderSpec> specs = {
        {.name = "fast-1"}, {.name = "fast-2"}, {.name = "stalled", .stall = true}};
    Pipe ready;
    Pipe results;
    std::vector<Child> children;
    for (const auto& spec : specs) {
        children.push_back(spawn_reader(name_, spec, ready, results));
    }
    wait_ready(ready, static_cast<int>(specs.size()));
    ASSERT_EQ(inspect_bus(name_)->readers.size(), specs.size());

    publish_messages(*writer, kMessages, 1);
    publish_end(*writer);

    std::uint64_t slow_laps = 0;
    for (std::size_t i = 0; i < specs.size(); ++i) {
        ReaderResult result;
        ASSERT_TRUE(results.receive(&result, sizeof(result)));
        expect_valid(result, "reader " + std::to_string(i));
        slow_laps = std::max(slow_laps, result.laps);
        RecordProperty("reader" + std::to_string(i) + "_delivered",
                       std::to_string(result.delivered));
        RecordProperty("reader" + std::to_string(i) + "_laps", std::to_string(result.laps));
    }
    for (const Child child : children) {
        EXPECT_TRUE(exited_cleanly(child));
    }
    EXPECT_GT(slow_laps, 0u) << "stalled reader was never lapped: lap path not exercised";
    EXPECT_TRUE(inspect_bus(name_)->readers.empty()) << "slots not released on exit";
}

TEST_F(MultiProcessBusTest, WriterKilledMidPublishIsReplacedWithoutCorruption) {
    Pipe writer_ready;
    const Child first_writer = spawn_endless_writer(name_, 1, writer_ready);
    wait_ready(writer_ready, 1);

    Pipe ready;
    Pipe results;
    std::vector<Child> readers;
    for (const char* name : {"reader-a", "reader-b"}) {
        readers.push_back(spawn_reader(name_, {.name = name}, ready, results));
    }
    wait_ready(ready, 2);

    // Let it run, then kill the writer with no chance to clean up, most likely mid-publish.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ASSERT_EQ(::kill(first_writer.pid, SIGKILL), 0);
    int status = 0;
    ASSERT_EQ(::waitpid(first_writer.pid, &status, 0), first_writer.pid);

    const auto before = inspect_bus(name_);
    ASSERT_TRUE(before.has_value());
    EXPECT_EQ(before->writer.pid, static_cast<std::uint32_t>(first_writer.pid));
    EXPECT_FALSE(before->writer_alive);
    EXPECT_GT(before->writer_stats.messages, 0u);

    // A new writer takes over the dead writer's bus and continues.
    auto replacement = Writer::create_or_attach({.name = name_, .capacity = kCapacity});
    ASSERT_TRUE(replacement.has_value()) << to_string(replacement.error());
    EXPECT_EQ(replacement->epoch(), 2u);
    publish_messages(*replacement, 100'000, 2);
    publish_end(*replacement);

    for (std::size_t i = 0; i < readers.size(); ++i) {
        ReaderResult result;
        ASSERT_TRUE(results.receive(&result, sizeof(result)));
        expect_valid(result, "reader " + std::to_string(i));
        EXPECT_EQ(result.last_writer, 2u) << "reader " << i << " never saw the new writer";
    }
    for (const Child child : readers) {
        EXPECT_TRUE(exited_cleanly(child));
    }
}

TEST_F(MultiProcessBusTest, KilledReaderDoesNotAffectOthersAndShowsAsDead) {
    auto writer = Writer::create_or_attach({.name = name_, .capacity = kCapacity});
    ASSERT_TRUE(writer.has_value());

    Pipe ready;
    Pipe results;
    const Child survivor = spawn_reader(name_, {.name = "survivor"}, ready, results);
    const Child victim = spawn_reader(name_, {.name = "victim"}, ready, results);
    wait_ready(ready, 2);

    publish_messages(*writer, 200'000, 1);
    ASSERT_EQ(::kill(victim.pid, SIGKILL), 0);
    int status = 0;
    ASSERT_EQ(::waitpid(victim.pid, &status, 0), victim.pid);
    publish_messages(*writer, 200'000, 2);
    publish_end(*writer);

    ReaderResult result;
    ASSERT_TRUE(results.receive(&result, sizeof(result)));
    expect_valid(result, "survivor");
    EXPECT_TRUE(exited_cleanly(survivor));

    const auto snapshot = inspect_bus(name_);
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_EQ(snapshot->readers.size(), 1u);  // survivor released its slot; victim could not
    EXPECT_EQ(snapshot->readers.front().name, "victim");
    EXPECT_FALSE(snapshot->readers.front().process_alive);
}

}  // namespace
}  // namespace mda::bus

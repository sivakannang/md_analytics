// mda-bus-bench — latency and throughput of the market data bus vs alternatives
// (docs/design/market-data-bus.md §11, Unit 4).
//
//   mda-bus-bench [--quick]
//
// Latency: the writer stamps each message with a monotonic clock reading (system-wide,
// so comparable across processes) and sends at a fixed pace, so we measure delivery
// latency rather than queueing. The reader records now - stamp for every message.
// Throughput: the writer publishes as fast as it can; the reader counts what arrives.
//
// Compared transports:
//   shm  — the bus: shared-memory broadcast ring, reader in another process
//   thread — the same ring between two threads of one process (no shm, no processes)
//   uds  — Unix domain datagram socket between two processes (kernel copy + syscalls)
//
// Run only release builds; numbers from debug or sanitizer builds are meaningless.

#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
#include <pthread.h>
#include <sys/qos.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <mda/bus/bus.hpp>
#include <mda/bus/wait_strategy.hpp>

namespace {

using namespace mda::bus;

constexpr MsgType kData = 1;
constexpr MsgType kEnd = 2;
constexpr std::size_t kBusCapacity = 64 * 1024 * 1024;
constexpr const char* kBusName = "bench";
constexpr std::size_t kPayloadSizes[] = {16, 64, 256, 1024};

// --------------------------------------------------------------------------- utilities

std::uint64_t now_ns() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

void spin_until(std::uint64_t deadline_ns) noexcept {
    while (now_ns() < deadline_ns) {
        cpu_relax();
    }
}

/// macOS can't pin threads to cores; the closest is asking for the highest QoS class,
/// which makes the scheduler prefer performance cores over efficiency cores.
void prefer_performance_cores() {
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

struct Pipe {
    int fds[2] = {-1, -1};
    Pipe() {
        if (::pipe(fds) != 0) {
            std::abort();
        }
    }
    ~Pipe() {
        ::close(fds[0]);
        ::close(fds[1]);
    }
    void send(const void* data, std::size_t size) const {
        auto* bytes = static_cast<const char*>(data);
        while (size > 0) {
            const ssize_t n = ::write(fds[1], bytes, size);
            if (n <= 0) {
                return;
            }
            bytes += n;
            size -= static_cast<std::size_t>(n);
        }
    }
    bool receive(void* data, std::size_t size) const {
        auto* bytes = static_cast<char*>(data);
        while (size > 0) {
            pollfd waiting{.fd = fds[0], .events = POLLIN, .revents = 0};
            if (::poll(&waiting, 1, 120'000) <= 0) {
                return false;
            }
            const ssize_t n = ::read(fds[0], bytes, size);
            if (n <= 0) {
                return false;
            }
            bytes += n;
            size -= static_cast<std::size_t>(n);
        }
        return true;
    }
    void signal() const {
        const char c = 1;
        send(&c, 1);
    }
    bool wait() const {
        char c = 0;
        return receive(&c, 1);
    }
};

/// Forks a child that runs `body` and sends its result back; the parent gets the pid.
template <typename Result, typename Body>
pid_t fork_child(const Pipe& results, Body body) {
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::alarm(300);
        prefer_performance_cores();
        const Result result = body();
        results.send(&result, sizeof(result));
        ::_exit(0);
    }
    return pid;
}

template <typename Result>
Result collect(const Pipe& results, pid_t child) {
    Result result{};
    if (!results.receive(&result, sizeof(result))) {
        std::fprintf(stderr, "benchmark child did not report\n");
    }
    int status = 0;
    ::waitpid(child, &status, 0);
    return result;
}

// --------------------------------------------------------------------------- latency

struct Latency {
    std::uint64_t samples = 0;
    std::uint64_t laps = 0;
    std::uint64_t min = 0, p50 = 0, p90 = 0, p99 = 0, p999 = 0, max = 0;
};

Latency summarize(std::vector<std::uint64_t>& samples, std::uint64_t laps) {
    Latency result{.samples = samples.size(), .laps = laps};
    if (samples.empty()) {
        return result;
    }
    std::ranges::sort(samples);
    auto at = [&](double p) {
        return samples[static_cast<std::size_t>(p * static_cast<double>(samples.size() - 1))];
    };
    result.min = samples.front();
    result.p50 = at(0.50);
    result.p90 = at(0.90);
    result.p99 = at(0.99);
    result.p999 = at(0.999);
    result.max = samples.back();
    return result;
}

struct LatencyConfig {
    std::uint64_t messages;
    std::uint64_t warmup;       // first samples discarded (page faults, caches, frequency)
    std::uint64_t interval_ns;  // pacing between sends
    std::size_t payload_size;
};

/// Reader side shared by all ring-based variants: record now - stamp per message.
template <typename PollOnce>
Latency consume_latency(const LatencyConfig& config, PollOnce poll_once) {
    std::vector<std::uint64_t> samples;
    samples.reserve(config.messages);
    std::uint64_t seen = 0;
    bool ended = false;
    auto handler = [&](MsgType type, std::span<const std::byte> payload) {
        const std::uint64_t received = now_ns();
        if (type == kEnd) {
            ended = true;
            return;
        }
        std::uint64_t sent = 0;
        std::memcpy(&sent, payload.data(), sizeof(sent));
        if (seen++ >= config.warmup) {
            samples.push_back(received - sent);
        }
    };
    std::uint64_t laps = 0;
    while (!ended) {
        laps += poll_once(handler);
    }
    return summarize(samples, laps);
}

/// Writer side: paced sends stamped with the send time.
template <typename Send>
void produce_latency(const LatencyConfig& config, Send send) {
    std::vector<std::byte> payload(config.payload_size);
    std::uint64_t next = now_ns();
    for (std::uint64_t i = 0; i < config.messages + config.warmup; ++i) {
        next += config.interval_ns;
        spin_until(next);
        const std::uint64_t stamp = now_ns();
        std::memcpy(payload.data(), &stamp, sizeof(stamp));
        send(kData, payload);
    }
}

template <WaitStrategy W>
Latency shm_latency(const LatencyConfig& config, W wait) {
    (void)remove_bus(kBusName);
    auto writer = Writer::create_or_attach({.name = kBusName, .capacity = kBusCapacity});
    if (!writer) {
        std::fprintf(stderr, "bus: %s\n", to_string(writer.error()).c_str());
        return {};
    }
    Pipe ready, results;
    const pid_t child = fork_child<Latency>(results, [&] {
        auto reader = Reader::attach(kBusName, {.name = "bench-latency"});
        ready.signal();
        return consume_latency(config, [&](auto& handler) -> std::uint64_t {
            const PollResult polled = reader->poll(handler);
            if (polled.status == PollStatus::Empty) {
                wait.idle();
            } else {
                wait.reset();
            }
            return polled.status == PollStatus::Lapped ? 1 : 0;
        });
    });
    ready.wait();
    produce_latency(config, [&](MsgType type, std::span<const std::byte> payload) {
        (void)writer->publish(type, payload);
    });
    (void)writer->publish(kEnd, {});
    const Latency result = collect<Latency>(results, child);
    (void)remove_bus(kBusName);
    return result;
}

/// The same ring between two threads of one process.
Latency thread_latency(const LatencyConfig& config) {
    auto counters = std::make_unique<RingCounters>();
    std::vector<std::uint64_t> storage(kBusCapacity / 8);
    const auto ring = std::as_writable_bytes(std::span(storage));
    auto writer = RingWriter::create(*counters, ring).value();
    auto reader = RingReader::create(*counters, ring).value();

    Latency result;
    std::atomic<bool> ready{false};
    std::thread consumer([&] {
        prefer_performance_cores();
        ready.store(true);
        BusySpin wait;
        result = consume_latency(config, [&](auto& handler) -> std::uint64_t {
            const PollResult polled = reader.poll(handler);
            if (polled.status == PollStatus::Empty) {
                wait.idle();
            }
            return polled.status == PollStatus::Lapped ? 1 : 0;
        });
    });
    while (!ready.load()) {
        cpu_relax();
    }
    produce_latency(config, [&](MsgType type, std::span<const std::byte> payload) {
        (void)writer.publish(type, payload);
    });
    (void)writer.publish(kEnd, {});
    consumer.join();
    return result;
}

// --------------------------------------------------------------------------- Unix sockets

struct SocketPair {
    int fds[2] = {-1, -1};
    SocketPair() {
        (void)::socketpair(AF_UNIX, SOCK_DGRAM, 0, fds);
        const int buffer = 4 * 1024 * 1024;
        for (int fd : fds) {
            ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buffer, sizeof(buffer));
            ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer));
        }
    }
    ~SocketPair() {
        ::close(fds[0]);
        ::close(fds[1]);
    }
};

/// Datagram send; retries while the receiver's buffer is full (back-pressure).
void socket_send(int fd, MsgType type, std::span<const std::byte> payload,
                 std::vector<std::byte>& frame) {
    frame.resize(1 + payload.size());
    frame[0] = static_cast<std::byte>(type);
    std::memcpy(frame.data() + 1, payload.data(), payload.size());
    while (::send(fd, frame.data(), frame.size(), 0) < 0 &&
           (errno == ENOBUFS || errno == EAGAIN || errno == EINTR)) {
        cpu_relax();
    }
}

/// Receives one datagram; returns false if none (non-blocking mode).
template <typename Handler>
bool socket_receive(int fd, bool blocking, std::vector<std::byte>& buffer, Handler& handler) {
    const ssize_t n = ::recv(fd, buffer.data(), buffer.size(), blocking ? 0 : MSG_DONTWAIT);
    if (n <= 0) {
        return false;
    }
    handler(static_cast<MsgType>(buffer[0]),
            std::span<const std::byte>(buffer).subspan(1, static_cast<std::size_t>(n) - 1));
    return true;
}

Latency socket_latency(const LatencyConfig& config, bool blocking) {
    SocketPair sockets;
    Pipe ready, results;
    const pid_t child = fork_child<Latency>(results, [&] {
        std::vector<std::byte> buffer(64 * 1024);
        ready.signal();
        return consume_latency(config, [&](auto& handler) -> std::uint64_t {
            if (!socket_receive(sockets.fds[1], blocking, buffer, handler)) {
                cpu_relax();
            }
            return 0;
        });
    });
    ready.wait();
    std::vector<std::byte> frame;
    produce_latency(config, [&](MsgType type, std::span<const std::byte> payload) {
        socket_send(sockets.fds[0], type, payload, frame);
    });
    socket_send(sockets.fds[0], kEnd, {}, frame);
    return collect<Latency>(results, child);
}

// --------------------------------------------------------------------------- throughput

struct Throughput {
    std::uint64_t delivered = 0;
    std::uint64_t laps = 0;
    std::uint64_t first_ns = 0;
    std::uint64_t last_ns = 0;
};

template <typename PollOnce>
Throughput consume_all(PollOnce poll_once) {
    Throughput result;
    bool ended = false;
    auto handler = [&](MsgType type, std::span<const std::byte>) {
        if (type == kEnd) {
            ended = true;
            return;
        }
        const std::uint64_t now = now_ns();
        if (result.delivered++ == 0) {
            result.first_ns = now;
        }
        result.last_ns = now;
    };
    while (!ended) {
        result.laps += poll_once(handler);
    }
    return result;
}

struct ThroughputRow {
    double writer_mps = 0;   // million messages per second, writer side
    double reader_mps = 0;   // million messages per second, as received
    double delivered = 0;    // fraction of messages the reader received
    double reader_gbps = 0;  // payload GB/s received
    std::uint64_t laps = 0;
};

ThroughputRow make_row(std::uint64_t messages, std::size_t size, std::uint64_t writer_ns,
                       const Throughput& r) {
    ThroughputRow row;
    row.writer_mps = static_cast<double>(messages) / static_cast<double>(writer_ns) * 1e3;
    const double reader_ns =
        static_cast<double>(std::max<std::uint64_t>(1, r.last_ns - r.first_ns));
    row.reader_mps = static_cast<double>(r.delivered) / reader_ns * 1e3;
    row.delivered = static_cast<double>(r.delivered) / static_cast<double>(messages);
    row.reader_gbps = static_cast<double>(r.delivered * size) / reader_ns;
    row.laps = r.laps;
    return row;
}

ThroughputRow shm_throughput(std::uint64_t messages, std::size_t size) {
    (void)remove_bus(kBusName);
    auto writer = Writer::create_or_attach({.name = kBusName, .capacity = kBusCapacity});
    Pipe ready, results;
    const pid_t child = fork_child<Throughput>(results, [&] {
        auto reader = Reader::attach(kBusName, {.name = "bench-throughput"});
        ready.signal();
        return consume_all([&](auto& handler) -> std::uint64_t {
            return reader->poll(handler).status == PollStatus::Lapped ? 1 : 0;
        });
    });
    ready.wait();
    const std::vector<std::byte> payload(size);
    const std::uint64_t start = now_ns();
    for (std::uint64_t i = 0; i < messages; ++i) {
        (void)writer->publish(kData, payload);
    }
    const std::uint64_t elapsed = now_ns() - start;
    (void)writer->publish(kEnd, {});
    const Throughput result = collect<Throughput>(results, child);
    (void)remove_bus(kBusName);
    return make_row(messages, size, elapsed, result);
}

ThroughputRow socket_throughput(std::uint64_t messages, std::size_t size) {
    SocketPair sockets;
    Pipe ready, results;
    const pid_t child = fork_child<Throughput>(results, [&] {
        std::vector<std::byte> buffer(64 * 1024);
        ready.signal();
        return consume_all([&](auto& handler) -> std::uint64_t {
            (void)socket_receive(sockets.fds[1], true, buffer, handler);
            return 0;
        });
    });
    ready.wait();
    const std::vector<std::byte> payload(size);
    std::vector<std::byte> frame;
    const std::uint64_t start = now_ns();
    for (std::uint64_t i = 0; i < messages; ++i) {
        socket_send(sockets.fds[0], kData, payload, frame);
    }
    const std::uint64_t elapsed = now_ns() - start;
    socket_send(sockets.fds[0], kEnd, {}, frame);
    return make_row(messages, size, elapsed, collect<Throughput>(results, child));
}

/// Cost of publish() alone, with no reader attached.
double publish_ns(std::uint64_t messages, std::size_t size) {
    (void)remove_bus(kBusName);
    auto writer = Writer::create_or_attach({.name = kBusName, .capacity = kBusCapacity});
    const std::vector<std::byte> payload(size);
    const std::uint64_t start = now_ns();
    for (std::uint64_t i = 0; i < messages; ++i) {
        (void)writer->publish(kData, payload);
    }
    const double per_message =
        static_cast<double>(now_ns() - start) / static_cast<double>(messages);
    (void)remove_bus(kBusName);
    return per_message;
}

// --------------------------------------------------------------------------- report

void print_latency(const char* name, const Latency& l) {
    auto us = [](std::uint64_t ns) { return static_cast<double>(ns) / 1000.0; };
    std::printf("  %-26s %9llu %8.2f %8.2f %8.2f %8.2f %9.2f %9.2f %5llu\n", name,
                static_cast<unsigned long long>(l.samples), us(l.min), us(l.p50), us(l.p90),
                us(l.p99), us(l.p999), us(l.max), static_cast<unsigned long long>(l.laps));
}

void print_throughput(const char* name, std::size_t size, const ThroughputRow& row) {
    std::printf("  %-8s %6zu %12.2f %12.2f %10.1f%% %9.2f %6llu\n", name, size, row.writer_mps,
                row.reader_mps, row.delivered * 100.0, row.reader_gbps,
                static_cast<unsigned long long>(row.laps));
}

}  // namespace

int main(int argc, char** argv) {
    const bool quick = argc > 1 && std::string_view(argv[1]) == "--quick";
#ifndef NDEBUG
    std::printf("WARNING: not a release build (NDEBUG undefined); numbers are meaningless\n\n");
#endif
    prefer_performance_cores();

    const LatencyConfig latency{.messages = quick ? 50'000ull : 200'000ull,
                                .warmup = 10'000,
                                .interval_ns = 5'000,  // 200k msg/s
                                .payload_size = 64};
    const LatencyConfig slow_latency{.messages = quick ? 2'000ull : 5'000ull,
                                     .warmup = 200,
                                     .interval_ns = 200'000,  // 5k msg/s: live Kite-like rate
                                     .payload_size = 64};

    std::printf("One-way latency, 64-byte messages, microseconds\n");
    std::printf("  %-26s %9s %8s %8s %8s %8s %9s %9s %5s\n", "transport / wait", "samples", "min",
                "p50", "p90", "p99", "p99.9", "max", "laps");
    print_latency("thread, busy-spin", thread_latency(latency));
    print_latency("shm process, busy-spin", shm_latency(latency, BusySpin{}));
    print_latency("shm process, backoff", shm_latency(latency, Backoff{}));
    print_latency("shm process, backoff @5k/s", shm_latency(slow_latency, Backoff{}));
    print_latency("shm process, sleep 100us",
                  shm_latency(slow_latency, Sleep(std::chrono::microseconds(100))));
    print_latency("uds process, busy-poll", socket_latency(latency, false));
    print_latency("uds process, blocking", socket_latency(latency, true));

    const std::uint64_t messages = quick ? 500'000 : 2'000'000;
    std::printf("\nThroughput, writer publishing as fast as possible (million msgs/s)\n");
    std::printf("  %-8s %6s %12s %12s %11s %9s %6s\n", "", "bytes", "writer", "reader", "delivered",
                "GB/s", "laps");
    for (const std::size_t size : kPayloadSizes) {
        print_throughput("shm", size, shm_throughput(messages, size));
        print_throughput("uds", size, socket_throughput(messages, size));
    }

    std::printf("\npublish() alone, no readers (ns per message)\n");
    for (const std::size_t size : kPayloadSizes) {
        std::printf("  %6zu bytes  %6.1f ns\n", size, publish_ns(messages, size));
    }
    return 0;
}

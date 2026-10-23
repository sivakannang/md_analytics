#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <mda/runtime/logging.hpp>

namespace mda::runtime {

void setup_logging(std::string_view process, const LoggingConfig& config) {
    std::vector<spdlog::sink_ptr> sinks;
    if (config.console) {
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    }
    if (!config.directory.empty()) {
        std::error_code ignored;
        std::filesystem::create_directories(config.directory, ignored);
        const auto file = config.directory / (std::string(process) + ".log");
        sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            file.string(), 10 * 1024 * 1024, 5));
    }

    // Async: callers only enqueue; a background thread formats and writes. When the queue
    // is full the oldest message is dropped instead of blocking the caller.
    spdlog::init_thread_pool(8192, 1);
    auto logger = std::make_shared<spdlog::async_logger>(
        std::string(process), sinks.begin(), sinks.end(), spdlog::thread_pool(),
        spdlog::async_overflow_policy::overrun_oldest);
    logger->set_pattern("%Y-%m-%d %H:%M:%S.%e %^%-5l%$ [%n] %v");
    logger->set_level(spdlog::level::from_str(config.level));
    logger->flush_on(spdlog::level::warn);
    spdlog::set_default_logger(logger);
    spdlog::flush_every(std::chrono::seconds(2));
}

void shutdown_logging() {
    spdlog::shutdown();
}

}  // namespace mda::runtime

#include "logging.h"
#include <cstddef>
#include <memory>
#include <utility>
#include <spdlog/async.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace logging {
    constexpr std::size_t queue_size = 8192;
    constexpr std::size_t worker_threads = 1;

    void init() {
        spdlog::init_thread_pool(queue_size, worker_threads);

        auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

        auto logger = std::make_shared<spdlog::async_logger>(
            "iotrail", std::move(sink), spdlog::thread_pool(),
            spdlog::async_overflow_policy::block);

        logger->set_pattern("[%H:%M:%S] [%^%l%$] %v");
        logger->set_level(spdlog::level::info);
        logger->flush_on(spdlog::level::trace);

        spdlog::set_default_logger(std::move(logger));
    }

    void shutdown() { spdlog::shutdown(); }

}  // namespace logging

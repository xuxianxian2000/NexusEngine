#include "nexus/core/log.h"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>

namespace nexus {

std::shared_ptr<spdlog::logger> Log::s_engine_logger;
std::shared_ptr<spdlog::logger> Log::s_app_logger;

void Log::init() {
    std::vector<spdlog::sink_ptr> sinks;
    sinks.emplace_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    sinks.emplace_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>("nexus.log", true));

    sinks[0]->set_pattern("%^[%T] [%n] %v%$");
    sinks[1]->set_pattern("[%T] [%l] [%n] %v");

    s_engine_logger = std::make_shared<spdlog::logger>("NEXUS", sinks.begin(), sinks.end());
    spdlog::register_logger(s_engine_logger);
    s_engine_logger->set_level(spdlog::level::trace);
    s_engine_logger->flush_on(spdlog::level::trace);

    s_app_logger = std::make_shared<spdlog::logger>("APP", sinks.begin(), sinks.end());
    spdlog::register_logger(s_app_logger);
    s_app_logger->set_level(spdlog::level::trace);
    s_app_logger->flush_on(spdlog::level::trace);

    s_engine_logger->info("NexusEngine logging initialized");
}

void Log::shutdown() {
    if (s_engine_logger) {
        s_engine_logger->info("NexusEngine logging shutdown");
    }
    spdlog::shutdown();
    // Drop the dangling handles so a later NX_* / double shutdown is safe.
    s_engine_logger.reset();
    s_app_logger.reset();
}

} // namespace nexus

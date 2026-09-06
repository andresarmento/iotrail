/*
 *  IoTrail - sistema leve de persistencia e streaming de eventos para IoT.
 *  Copyright Andre Sarmento - 2026
 *
 *  Logging 
 */
#pragma once
#include <spdlog/spdlog.h>

namespace logging {
    void init();
    void shutdown();

    using spdlog::trace;
    using spdlog::debug;
    using spdlog::info;
    using spdlog::warn;
    using spdlog::error;
    using spdlog::critical;

    // logging::set_level(logging::level::debug) - vale pros loggers ja
    // registrados e pros que vierem (arquivo rotativo, Fase 9).
    namespace level = spdlog::level;
    using spdlog::set_level;
}

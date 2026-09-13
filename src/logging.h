/*
 *  IoTrail - sistema leve de persistencia e streaming de eventos para IoT.
 *  Copyright Andre Sarmento - 2026
 *
 *  Logging.h
 */
#pragma once
#include <spdlog/spdlog.h>

namespace logging {
    void init();
    void shutdown();

    // Funcoes importadas
    using spdlog::trace;
    using spdlog::debug;
    using spdlog::info;
    using spdlog::warn;
    using spdlog::error;
    using spdlog::critical;
    using spdlog::set_level;

    // Alias para o namespace spdlog::level
    namespace level = spdlog::level;
}

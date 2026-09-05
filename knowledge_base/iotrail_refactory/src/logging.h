#pragma once

#include <spdlog/spdlog.h>

namespace logging {
    void init();
    void shutdown();

    // Reexporta as funcoes de log do spdlog sob este namespace, pra tudo que e'
    // log ter o mesmo prefixo. E' declaracao using, nao wrapper: nao ha camada
    // de indirecao nem sobrecarga, e a API de formatacao {} continua a mesma.
    using spdlog::trace;
    using spdlog::debug;
    using spdlog::info;
    using spdlog::warn;
    using spdlog::error;
    using spdlog::critical;
}

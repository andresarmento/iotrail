/*
 *  IoTrail - sistema leve de persistencia e streaming de eventos para IoT.
 *  Copyright Andre Sarmento - 2026
 *
 *  Linha de comando
 */
#pragma once
#include <filesystem>
#include <optional>

namespace cmdline {
    struct options {
        std::filesystem::path config;
        int verbose = 0;
    };

    std::optional<options> parse(int argc, char* argv[]);
}

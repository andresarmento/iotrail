/*
 *  IoTrail - sistema leve de persistencia e streaming de eventos para IoT.
 *  Copyright Andre Sarmento - 2026
 *
 *  Localizacao de arquivos
 */
#pragma once
#include <filesystem>

namespace paths {
    std::filesystem::path exe_dir();

    // Onde esta o iotrail.conf: "-c <arquivo>" vence; sem ele, o iotrail.conf
    // do diretorio do executavel. 
    std::filesystem::path config_from_args(int argc, char* argv[]);
}

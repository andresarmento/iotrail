/*
 *  IoTrail - sistema leve de persistencia e streaming de eventos para IoT.
 *  Copyright Andre Sarmento - 2026
 *
 *  Localizacao de arquivos
 */
#pragma once
#include <filesystem>

namespace paths {
    // Diretorio onde o executavel esta. Vazio se o SO nao souber dizer.
    // Quem compoe o caminho da config a partir dele e' o cmdline.
    std::filesystem::path exe_dir();
}

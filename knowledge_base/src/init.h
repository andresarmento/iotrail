#pragma once

#include <spdlog/spdlog.h>
#include <csignal>

// Configura o pattern/flush do spdlog e instala o handler de SIGINT.
void init(void);

// true depois que o processo recebe SIGINT (Ctrl+C).
bool stopRequested(void);
/*
 *  IoTrail - sistema leve de persistencia e streaming de eventos para IoT.
 *
 *  Copyright André Sarmento - 2026
 */

#include "ini.h"
#include "logging.h"
#include "paths.h"
#include "signals.h"
#include <chrono>
#include <filesystem>
#include <thread>

int main(int argc, char* argv[]) {
    logging::init();

    // Antes dos sinais: se a linha de comando estiver errada nao ha nada pra
    // parar de forma ordenada, e sem signals::init() ninguem espera pelo
    // shutdown_done() no caminho de erro.
    const std::filesystem::path config_path = paths::config_from_args(argc, argv);
    if (config_path.empty()) {
        logging::shutdown();
        return 1;
    }
    logging::info("config: {}", config_path.string());

    const auto sections = ini::parse_file(config_path);
    if (!sections) {
        logging::shutdown();
        return 1;
    }
    logging::info("config lida: {} secoes", sections->size());
    for (const auto& sec : *sections) {
        logging::debug("  [{}:{}] linha {}, {} chaves", sec.type, sec.name, sec.line,
                       sec.entries.size());
        for (const auto& e : sec.entries) {
            logging::trace("    {} = \"{}\"", e.key, e.value);
        }
    }

    signals::init();

    logging::info("IoTrail subiu, Ctrl+C para encerrar");

    while (!signals::stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    logging::info("IoTrail encerrando");
    logging::shutdown();
    signals::shutdown_done(); // Deve ser a última linha
    return 0;
}

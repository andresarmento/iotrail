/*
 *  IoTrail - sistema leve de persistencia e streaming de eventos para IoT.
 *
 *  Copyright André Sarmento - 2026
 */

#include "logging.h"
#include "paths.h"
#include "signals.h"
#include <chrono>
#include <filesystem>
#include <thread>

int main(int argc, char* argv[]) {
    logging::init();

    const std::filesystem::path config_path = paths::config_from_args(argc, argv);
    if (config_path.empty()) {
        logging::shutdown();
        return 1;
    }
    logging::info("config: {}", config_path.string());
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

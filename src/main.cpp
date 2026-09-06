/*
 *  IoTrail - sistema leve de persistencia e streaming de eventos para IoT.
 *
 *  Copyright André Sarmento - 2026
 */

#include "cmdline.h"
#include "ini.h"
#include "logging.h"
#include "signals.h"
#include <chrono>
#include <thread>

int main(int argc, char* argv[]) {
    logging::init();

    // Command line options
    const auto opts = cmdline::parse(argc, argv);
    if (!opts) {
        logging::shutdown();
        return 1;
    }
  
    // Log level
    if (opts->verbose == 1) {
        logging::set_level(logging::level::debug);
    } else if (opts->verbose >= 2) {
        logging::set_level(logging::level::trace);
    }
  
    // Config file
    logging::debug("config: {}", opts->config.string());
    const auto sections = ini::parse_file(opts->config);
    if (!sections) {
        logging::shutdown();
        return 1;
    }

    // Testes
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

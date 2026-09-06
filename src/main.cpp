/*
 *  IoTrail - sistema leve de persistencia e streaming de eventos para IoT.
 *
 *  Copyright André Sarmento - 2026
 */

#include "client.h"
#include "cmdline.h"
#include "config.h"
#include "logging.h"
#include "mqtt.h"
#include "signals.h"
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

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
    const auto settings = config::load(opts->config);
    if (!settings) {
        logging::shutdown();
        return 1;
    }

    // Debug config
    logging::info("config lida: {} broker(s), {} stream(s), data_dir {}",
                  settings->brokers.size(), settings->streams.size(),
                  settings->data_dir.string());
    for (const auto& br : settings->brokers) {
        logging::debug("  broker {} -> {}:{} (client_id {})", br.name, br.host, br.port,
                       br.client_id);
    }
    for (const auto& st : settings->streams) {
        logging::debug("  stream {} <- broker {}, {} topico(s)", st.name, st.broker,
                       st.topics.size());
        for (const auto& topic : st.topics) {
            logging::trace("    {}", topic);
        }
    }

    // Setup MQTT clients
    if (!mqtt::init()) {
        logging::shutdown();
        return 1;
    }

    std::vector<std::unique_ptr<mqtt::client>> clients;
    for (const auto& br : settings->brokers) {
        clients.push_back(std::make_unique<mqtt::client>(br));
        if (!clients.back()->start()) {
            clients.clear();
            mqtt::shutdown();
            logging::shutdown();
            return 1;
        }
    }

    // Setup signals
    signals::init();

    // Loop thread main
    logging::info("IoTrail subiu, Ctrl+C para encerrar");

    while (!signals::stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // Shutdown
    logging::info("IoTrail encerrando");
    clients.clear(); // para e destroi cada cliente ANTES do lib_cleanup
    mqtt::shutdown();
    logging::shutdown();
    signals::shutdown_done(); // Deve ser a última linha
    return 0;
}

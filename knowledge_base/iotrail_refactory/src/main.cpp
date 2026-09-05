/*********************************************************************************************
 *  IoTrail e' um sistema leve de persistencia e streaming de eventos para ambientes IoT
 *
 *
 *  Copyright Andre Sarmento - 2026
 *********************************************************************************************/

#include "config.h"
#include "logging.h"
#include "mqtt_client.h"
#include "paths.h"
#include "signals.h"

#include <chrono>
#include <memory>
#include <thread>
#include <vector>

int main() {
    logging::init();
    signals::init();
    mqtt::init();

    /* Load configuration file */
    auto loaded = config::load(paths::executableDir() / "iotrail.conf");
    if (!loaded) {
        logging::error("[config] configuracao invalida, encerrando");
        logging::shutdown();
        return 1;
    }
    const config::Config& cfg = *loaded;

    /* Create clients, one per broker */
    std::vector<std::unique_ptr<mqtt::Client>> clients;
    for (const auto& broker : cfg.brokers) {
        // Streams deste broker, na ordem de declaracao. Ponteiros pra dentro
        // de cfg, que vive ate o fim do main() - depois dos clientes serem
        // destruidos.
        std::vector<const config::Stream*> streams;
        for (const auto& stream : cfg.streams) {
            if (stream.broker == broker.name) streams.push_back(&stream);
        }

        auto client = std::make_unique<mqtt::Client>(broker, std::move(streams));
        if (client->start()) {
            clients.push_back(std::move(client));
        }
    }

    if (clients.empty()) {
        logging::error("[mqtt] nenhum cliente pode ser criado, encerrando");
        mqtt::shutdown();
        logging::shutdown();
        return 1;
    }

    logging::info("iotrail: rodando com {} cliente(s), Ctrl+C para encerrar", clients.size());

    while (!signals::stopRequested()) {
        for (auto& client : clients) {
            client->tick();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    logging::info("iotrail: shutdown");
    for (auto& client : clients) {
        client->stop();
    }
    clients.clear(); 
    mqtt::shutdown();

    logging::shutdown();
    return 0;
}

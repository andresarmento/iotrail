/*
 *  IoTrail - sistema leve de persistencia e streaming de eventos para IoT.
 *
 *  Copyright André Sarmento - 2026
 */

#include "logging.h"
#include "signals.h"
#include <chrono>
#include <thread>

int main() {
    logging::init();
    signals::init();

    logging::info("IoTrail subiu, Ctrl+C para encerrar");

    while (!signals::stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    logging::info("IoTrail encerrando");
    logging::shutdown();
    signals::shutdown_done();
    return 0;
}

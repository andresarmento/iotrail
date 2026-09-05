/*
 *  IoTrail - sistema leve de persistencia e streaming de eventos para IoT.
 *  Copyright Andre Sarmento - 2026
 *
 *  Parada ordenada
 */
#pragma once

namespace signals {
    void init();
    void request_stop();
    bool stop_requested();
    void shutdown_done();
}

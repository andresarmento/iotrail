/*
 *  IoTrail - sistema leve de persistencia e streaming de eventos para IoT.
 *
 *  Copyright Andre Sarmento - 2026
 */

#include "logging.h"

int main() {
    // main provisorio: sobe o log, diz oi e sai. A parada ordenada de verdade
    // (sinais, espera, encerramento) e' a tarefa 1.3.
    logging::init();
    logging::info("IoTrail subiu");
    logging::shutdown();
    return 0;
}

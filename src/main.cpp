/*
 *  IoTrail - sistema leve de persistencia e streaming de eventos para IoT.
 *
 *  Copyright Andre Sarmento - 2026
 */

#include "logging.h"
#include "signals.h"

#include <chrono>
#include <thread>

// Espera em polling, nao condition_variable: o main ainda nao tem trabalho
// pendurado nela, e notificar uma cv de dentro de um handler de SIGINT nao e'
// async-signal-safe. Se um dia a espera precisar acordar na hora, o caminho e'
// um evento do SO, nao cv. Custo hoje: ate 200 ms de atraso pra sair - e esses
// 200 ms saem do prazo que o Windows da pra fechar a janela.
constexpr auto poll_interval = std::chrono::milliseconds(200);

int main() {
    logging::init();
    signals::init();

    logging::info("IoTrail subiu, Ctrl+C para encerrar");

    while (!signals::stop_requested()) {
        std::this_thread::sleep_for(poll_interval);
    }

    logging::info("IoTrail encerrando");
    logging::shutdown();

    // Por ultimo, depois de tudo que precisa sobreviver ao encerramento: pode
    // haver um handler de console bloqueado aqui, segurando o processo vivo.
    signals::shutdown_done();
    return 0;
}

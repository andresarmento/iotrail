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

    // Ultima coisa que o main faz antes de sair: solta o handler do console que
    // esta segurando o processo vivo. Sem isto, fechar a janela custa a espera
    // inteira do shutdown_grace toda vez.
    void shutdown_done();
}

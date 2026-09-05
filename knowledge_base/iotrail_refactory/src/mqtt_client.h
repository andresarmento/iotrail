#pragma once

#include <atomic>
#include <chrono>
#include <vector>

#include "config.h"

struct mosquitto;
struct mosquitto_message;

namespace mqtt {
    // Uma vez por processo: init() antes de qualquer Client, shutdown() depois
    // do ultimo ser destruido.
    void init();
    void shutdown();

    // Um cliente por broker. A thread de rede e' criada pela lib
    // (mosquitto_loop_start), nao por nos.
    //
    // Decidido 2026-09-04, item 2.1: connect() sincrono (a alternativa) ficava
    // preso no SYN timeout quando o host nao respondia - medido, 19s de boot
    // travado com um host inalcancavel. connect_async nao bloqueia, mas exige
    // loop_start (mosquitto.h:616).
    //
    // Os callbacks rodam na thread da lib, uma por cliente: com N brokers, N
    // callbacks podem estar em execucao ao mesmo tempo.
    class Client {
      public:
        // As streams sao as deste broker, na ordem de declaracao. Sao
        // ponteiros pra dentro da Config, que precisa continuar viva enquanto o
        // cliente existir.
        //
        // Cada cliente carrega so a propria lista: nao ha estrutura
        // compartilhada entre as N threads da lib, e por isso nao ha lock
        // nenhum no caminho da mensagem.
        Client(config::Broker broker, std::vector<const config::Stream*> streams);
        ~Client();

        Client(const Client&) = delete;
        Client& operator=(const Client&) = delete;

        bool start();
        void stop();

        // Chamado periodicamente pelo main(). Existe porque o auto-reconnect da
        // lib so cobre queda DEPOIS de uma conexao estabelecida
        // (mosquitto.h:1657, "unexpectedly disconnected") - medido: com o broker
        // fora do ar no boot, a lib manda um CONNECT e nunca mais tenta. Sem
        // isto, "processo sobe sempre" viria com o cliente morto calado.
        void tick();

      private:
        static void onConnect(mosquitto* mosq, void* self, int rc);
        static void onDisconnect(mosquitto* mosq, void* self, int rc);
        static void onSubscribe(mosquitto* mosq, void* self, int mid, int qos_count,
                                const int* granted_qos);
        static void onMessage(mosquitto* mosq, void* self, const mosquitto_message* msg);
        // Com connect_async, falha de TCP nao passa por on_connect nem por
        // on_disconnect - o log da lib e' o unico canal que a reporta.
        static void onLog(mosquitto* mosq, void* self, int level, const char* str);

        const config::Broker broker_;
        const std::vector<const config::Stream*> streams_;
        mosquitto* mosq_ = nullptr;

        // Escritos na thread da lib (callbacks), lidos na do main() (tick).
        std::atomic<bool> connected_{false};
        std::atomic<bool> ever_connected_{false};

        // So a thread do main() toca: start(), stop() e tick() nao sao chamados
        // de dentro dos callbacks.
        bool running_ = false;
        unsigned retry_delay_s_ = 1;
        std::chrono::steady_clock::time_point next_retry_{};
    };
}

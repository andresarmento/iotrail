/*
 *  IoTrail - sistema leve de persistencia e streaming de eventos para IoT.
 *  Copyright Andre Sarmento - 2026
 *
 *  Cliente MQTT: um por broker
 */
#pragma once
#include "config.h"

#include <atomic>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

struct mosquitto;
struct mosquitto_message;

namespace mqtt {
    class client {
      public:
        // As streams sao as deste broker. Ponteiros pra dentro do settings, que
        // vive no main e sobrevive aos clientes.
        client(config::broker broker, std::vector<const config::stream*> streams);
        ~client();

        client(const client&) = delete;
        client& operator=(const client&) = delete;

        bool start();
        void stop();

      private:
        static void on_connect(mosquitto* mosq, void* self, int rc);
        static void on_disconnect(mosquitto* mosq, void* self, int rc);
        static void on_subscribe(mosquitto* mosq, void* self, int mid, int qos_count,
                                 const int* granted_qos);
        static void on_message(mosquitto* mosq, void* self, const mosquitto_message* msg);
        static void on_log(mosquitto* mosq, void* self, int level, const char* str);

        mosquitto* mosq_ = nullptr;
        const config::broker broker_;
        const std::vector<const config::stream*> streams_;

        // mid do SUBSCRIBE -> padrao pedido, pra dizer no log QUAL inscricao o
        // broker recusou. So a thread da lib toca: on_connect e on_subscribe
        // rodam nela, entao nao ha lock de proposito.
        std::unordered_map<int, std::string> pending_subs_;

        // Mensagem que nao casou com stream nenhuma. Nao deveria acontecer: as
        // inscricoes sao exatamente a uniao dos padroes das streams, entao cair
        // aqui significa que o nosso matcher e o do broker discordam. Aviso na
        // primeira e contagem no encerramento, pra nao inundar o log se a
        // suposicao acima estiver errada.
        bool unmapped_warned_ = false;
        std::atomic<size_t> unmapped_count_{0};

        std::atomic<bool> connected_{false};
        std::atomic<bool> ever_connected_{false};

        bool running_ = false;
        bool joined_ = false;
    };
}

/*
 *  IoTrail - sistema leve de persistencia e streaming de eventos para IoT.
 *  Copyright Andre Sarmento - 2026
 *
 *  Cliente MQTT: um por broker
 */
#pragma once
#include "config.h"

#include <atomic>

struct mosquitto;

namespace mqtt {
    class client {
      public:
        explicit client(config::broker broker);
        ~client();

        client(const client&) = delete;
        client& operator=(const client&) = delete;

        bool start();
        void stop();

      private:
        static void on_connect(mosquitto* mosq, void* self, int rc);
        static void on_disconnect(mosquitto* mosq, void* self, int rc);
        static void on_log(mosquitto* mosq, void* self, int level, const char* str);

        mosquitto* mosq_ = nullptr;
        const config::broker broker_;

        std::atomic<bool> connected_{false};
        std::atomic<bool> ever_connected_{false};

        bool running_ = false;
        bool joined_ = false;
    };
}

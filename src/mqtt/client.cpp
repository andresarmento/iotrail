#include "client.h"

#include "logging.h"

#include <mosquitto.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <utility>

namespace mqtt {
    static constexpr int keepalive_s = 60;
    static constexpr unsigned reconnect_delay_s = 1;
    static constexpr unsigned reconnect_delay_max_s = 60;

    static std::string error_text(int rc) {
        if (rc == MOSQ_ERR_ERRNO) {
            return std::string(mosquitto_strerror(rc)) + ": " + std::strerror(errno);
        }
        return mosquitto_strerror(rc);
    }

    client::client(config::broker broker) : broker_(std::move(broker)) {}

    client::~client() {
        stop();
        if (mosq_ != nullptr && joined_) {
            mosquitto_destroy(mosq_);
        }
    }

    bool client::start() {
        mosq_ = mosquitto_new(broker_.client_id.c_str(), true, this);
        if (mosq_ == nullptr) {
            logging::error("[mqtt/{}] mosquitto_new falhou: {}", broker_.name,
                           std::strerror(errno));
            return false;
        }

        mosquitto_connect_callback_set(mosq_, on_connect);
        mosquitto_disconnect_callback_set(mosq_, on_disconnect);
        mosquitto_log_callback_set(mosq_, on_log);

        // Backoff exponencial da propria lib, 1s a 60s. So atua com
        // loop_start/loop_forever, e cobre apenas queda DEPOIS de uma conexao
        // estabelecida - a primeira e' supervisao nossa.
        mosquitto_reconnect_delay_set(mosq_, reconnect_delay_s, reconnect_delay_max_s, true);

        // Nao bloqueia: a conexao acontece na thread da lib. O sincrono ficava
        // preso no timeout do SYN, 19s de boot travado com host inalcancavel.
        const int rc = mosquitto_connect_async(mosq_, broker_.host.c_str(), broker_.port,
                                               keepalive_s);
        if (rc != MOSQ_ERR_SUCCESS) {
            logging::warn("[mqtt/{}] connect_async em {}:{}: {}", broker_.name, broker_.host,
                          broker_.port, error_text(rc));
        }

        const int loop_rc = mosquitto_loop_start(mosq_);
        if (loop_rc != MOSQ_ERR_SUCCESS) {
            logging::error("[mqtt/{}] loop_start falhou: {}", broker_.name, error_text(loop_rc));
            return false;
        }

        running_ = true;
        logging::info("[mqtt/{}] conectando em {}:{} (client_id \"{}\")", broker_.name,
                      broker_.host, broker_.port, broker_.client_id);
        return true;
    }

    void client::stop() {
        if (!running_) return;
        running_ = false;
 
        const bool force = !connected_.load();
        mosquitto_disconnect(mosq_);
        mosquitto_loop_stop(mosq_, force);
        joined_ = true;

        logging::info("[mqtt/{}] encerrado", broker_.name);
    }

    void client::on_connect(mosquitto*, void* self, int rc) {
        client* c = static_cast<client*>(self);

        if (rc != 0) {
            // Nao reconecta daqui: a lib refaz a tentativa com o backoff do start().
            logging::error("[mqtt/{}] broker recusou a conexao: {} (rc {})", c->broker_.name,
                           mosquitto_connack_string(rc), rc);
            return;
        }

        c->connected_.store(true);
        c->ever_connected_.store(true);
        logging::info("[mqtt/{}] conectado em {}:{}", c->broker_.name, c->broker_.host,
                      c->broker_.port);
    }

    void client::on_disconnect(mosquitto*, void* self, int rc) {
        client* c = static_cast<client*>(self);
        c->connected_.store(false);

        // rc == 0 e' a desconexao que nos pedimos, no stop().
        if (rc == 0) return;

        // Quem retenta depende de ja ter conectado alguma vez, e dizer "a lib
        // reconecta" nos dois casos mandaria procurar problema no lugar errado.
        if (c->ever_connected_.load()) {
            logging::warn("[mqtt/{}] conexao perdida - a lib reconecta", c->broker_.name);
        } else {
            logging::warn("[mqtt/{}] conexao inicial falhou", c->broker_.name);
        }
    }

    void client::on_log(mosquitto*, void* self, int level, const char* str) {
        const client* c = static_cast<const client*>(self);
        const char* text = str != nullptr ? str : "";

        // Com connect_async, falha de TCP nao passa por on_connect nem por
        // on_disconnect: este e' o unico canal que a reporta. O DEBUG da lib vai
        // pro nosso trace porque ela loga cada PINGREQ/PINGRESP.
        if (level == MOSQ_LOG_ERR || level == MOSQ_LOG_WARNING) {
            logging::warn("[mqtt/{}] {}", c->broker_.name, text);
        } else if (level == MOSQ_LOG_DEBUG) {
            logging::trace("[mqtt/{}] {}", c->broker_.name, text);
        } else {
            logging::debug("[mqtt/{}] {}", c->broker_.name, text);
        }
    }
}

#include "client.h"

#include "logging.h"

#include <mosquitto.h>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <string>
#include <utility>

namespace mqtt {
    static constexpr int subscribe_qos = 0;
    // Valor que o broker devolve no SUBACK quando recusa a inscricao.
    static constexpr int suback_failure = 0x80;
    static constexpr unsigned reconnect_delay_s = 1;
    static constexpr unsigned reconnect_delay_max_s = 60;

    static std::string error_text(int rc) {
        if (rc == MOSQ_ERR_ERRNO) {
            return std::string(mosquitto_strerror(rc)) + ": " + std::strerror(errno);
        }
        return mosquitto_strerror(rc);
    }

    client::client(config::broker broker, std::vector<const config::stream*> streams)
        : broker_(std::move(broker)), streams_(std::move(streams)) {}

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
        mosquitto_subscribe_callback_set(mosq_, on_subscribe);
        mosquitto_message_callback_set(mosq_, on_message);
        mosquitto_log_callback_set(mosq_, on_log);

        // Backoff exponencial da propria lib, 1s a 60s. So atua com
        // loop_start/loop_forever, e cobre apenas queda DEPOIS de uma conexao
        // estabelecida - a primeira e' supervisao nossa.
        mosquitto_reconnect_delay_set(mosq_, reconnect_delay_s, reconnect_delay_max_s, true);

        // Prepara a conexao; quem a executa e' a thread da lib, criada no loop_start em seguida
        const int rc = mosquitto_connect_async(mosq_, broker_.host.c_str(), broker_.port,
                                               broker_.keepalive);
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

        // Depois do join: a thread da lib parou, entao o contador esta estavel.
        const size_t unmapped = unmapped_count_.load();
        if (unmapped > 0) {
            logging::warn("[mqtt/{}] {} mensagem(ns) sem stream no total", broker_.name,
                          unmapped);
        }

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

        // Subscreve a cada (re)conexao, nao uma vez no start(): com
        // clean_session=true o broker esquece as inscricoes quando a conexao
        // cai. A uniao dos padroes sai daqui, das streams deste broker, e nao de
        // um campo derivado na config.
        //
        // Um SUBSCRIBE por padrao, e nao subscribe_multiple: assim o mid do
        // SUBACK identifica QUAL padrao o broker recusou. Custo: N pacotes por
        // conexao, uma vez.
        std::vector<std::string> seen;   // dedup: inclui os que falharam
        size_t requested = 0;            // so os que a lib aceitou enviar
        for (const config::stream* st : c->streams_) {
            for (const std::string& topic : st->topics) {
                // Duas streams do mesmo broker podem declarar o mesmo padrao; o
                // SUBSCRIBE repetido seria inofensivo, mas apareceria duas vezes
                // no log e no SUBACK.
                if (std::find(seen.begin(), seen.end(), topic) != seen.end()) {
                    continue;
                }
                seen.push_back(topic);

                int mid = 0;
                const int sub_rc = mosquitto_subscribe(c->mosq_, &mid, topic.c_str(), subscribe_qos);
                if (sub_rc != MOSQ_ERR_SUCCESS) {
                    logging::error("[mqtt/{}] subscribe \"{}\" falhou: {}", c->broker_.name, topic,
                                   error_text(sub_rc));
                    continue;
                }

                ++requested;
                c->pending_subs_[mid] = topic;
                logging::debug("[mqtt/{}] subscrevendo \"{}\" (qos {})", c->broker_.name, topic, subscribe_qos);
            }
        }

        logging::info("[mqtt/{}] {} inscricao(oes) pedida(s)", c->broker_.name, requested);
    }

    void client::on_subscribe(mosquitto*, void* self, int mid, int qos_count,
                              const int* granted_qos) {
        client* c = static_cast<client*>(self);

        const auto it = c->pending_subs_.find(mid);
        const std::string topic = it != c->pending_subs_.end() ? it->second : "?";
        if (it != c->pending_subs_.end()) c->pending_subs_.erase(it);

        // Silencioso no caso normal: so interessa quando o broker recusa, que e'
        // o que distingue "padrao invalido ou sem permissao" de "ninguem
        // publicou ainda".
        for (int i = 0; i < qos_count; ++i) {
            if (granted_qos[i] == suback_failure) {
                logging::warn("[mqtt/{}] broker recusou a inscricao em \"{}\"", c->broker_.name,
                              topic);
            }
        }
    }

    void client::on_message(mosquitto*, void* self, const mosquitto_message* msg) {
        client* c = static_cast<client*>(self);

        const auto arrived_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count();

        const char* topic = msg->topic != nullptr ? msg->topic : "";

        // streams_ e' imutavel desde a construcao e pertence so a este cliente,
        // entao a busca nao precisa de lock mesmo com N callbacks concorrentes.
        //
        // TODAS as streams cujo padrao casa, nao so a primeira: duas streams
        // cobrindo o mesmo topico e' escolha de quem configurou, e com retencao
        // e replay independentes por stream isso faz sentido. Quando o writer
        // entrar, sao N push(), um por stream.
        bool matched = false;
        for (const config::stream* st : c->streams_) {
            for (const std::string& pattern : st->topics) {
                bool hit = false;
                // Funcao da lib: trata + no meio do nivel, # so no fim, e o fato
                // de # nao casar com $SYS. Matcher de wildcard escrito a mao
                // erra nesses cantos.
                if (mosquitto_topic_matches_sub(pattern.c_str(), topic, &hit) != MOSQ_ERR_SUCCESS) {
                    continue;
                }
                if (!hit) continue;

                matched = true;
                logging::debug("[mqtt/{}] {} -> stream {} ({} bytes, ts {})", c->broker_.name,
                               topic, st->name, msg->payloadlen, arrived_ms);
                // Um padrao que casa ja resolve a stream; os outros padroes dela
                // nao mudam nada.
                break;
            }
        }

        if (!matched) {
            c->unmapped_count_.fetch_add(1, std::memory_order_relaxed);
            if (!c->unmapped_warned_) {
                c->unmapped_warned_ = true;
                logging::warn("[mqtt/{}] mensagem em \"{}\" nao casou com stream nenhuma - "
                              "as inscricoes vem das streams, entao isto indica bug de "
                              "casamento de padrao",
                              c->broker_.name, topic);
            }
        }
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

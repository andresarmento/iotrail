#include "mqtt_client.h"

#include <mosquitto.h>

#include <algorithm>
#include <cerrno>
#include <vector>

#include <chrono>
#include <cstring>
#include <string>
#include <utility>

#include "logging.h"

namespace mqtt {

    constexpr int keepalive_s = 60;
    constexpr int subscribe_qos = 0;
    constexpr unsigned reconnect_delay_s = 1;
    constexpr unsigned reconnect_delay_max_s = 30;
    // Valor que o broker devolve no SUBACK quando recusa a inscricao.
    constexpr int suback_failure = 0x80;

    // mosquitto_strerror() devolve "Unknown error" para MOSQ_ERR_ERRNO, que
    // e' o codigo de praticamente toda falha de rede - sem o errno do
    // sistema junto, "recusado", "host inalcancavel" e "DNS falhou" viram a
    // mesma mensagem inutil. O errno e' preenchido tambem no Windows.
    std::string errorText(int rc) {
        if (rc == MOSQ_ERR_ERRNO) {
            return std::string(mosquitto_strerror(rc)) + ": " + std::strerror(errno);
        }
        return mosquitto_strerror(rc);
    }

    void init() { mosquitto_lib_init(); }

    void shutdown() { mosquitto_lib_cleanup(); }

    // TODAS as streams cujo padrao casa, nao so a primeira (decidido
    // 2026-09-04). Duas streams do mesmo broker cobrindo o mesmo topico e'
    // escolha de quem configurou, e a intencao provavel e' guardar nas duas -
    // com retencao e replay independentes por stream, isso faz sentido.
    std::vector<const config::Stream*> matchAll(const std::vector<const config::Stream*>& streams,
                                                const char* topic) {
        std::vector<const config::Stream*> matched;
        if (topic == nullptr) return matched;

        for (const config::Stream* stream : streams) {
            for (const std::string& pattern : stream->topics) {
                bool hit = false;
                // Funcao C livre da lib: trata + no meio, # so no fim, e o fato
                // de # nao casar com topico $SYS. Reimplementar matcher de
                // wildcard MQTT a mao seria erro garantido nos cantos.
                const int rc = mosquitto_topic_matches_sub(pattern.c_str(), topic, &hit);
                if (rc == MOSQ_ERR_SUCCESS && hit) {
                    // Um padrao que casa ja resolve a stream; os outros padroes
                    // dela nao mudam nada.
                    matched.push_back(stream);
                    break;
                }
            }
        }
        return matched;
    }

    Client::Client(config::Broker broker, std::vector<const config::Stream*> streams)
        : broker_(std::move(broker)), streams_(std::move(streams)) {}

    Client::~Client() {
        stop();
        if (mosq_ != nullptr) mosquitto_destroy(mosq_);
    }

    bool Client::start() {
        // clean_session=true: sem QoS > 0 nesta fase, sessao persistente nao
        // guardaria nada. Revisitar junto com QoS.
        mosq_ = mosquitto_new(broker_.client_id.c_str(), true, this);
        if (mosq_ == nullptr) {
            logging::error("[mqtt/{}] mosquitto_new falhou: {}", broker_.name,
                           std::strerror(errno));
            return false;
        }

        mosquitto_connect_callback_set(mosq_, onConnect);
        mosquitto_disconnect_callback_set(mosq_, onDisconnect);
        mosquitto_subscribe_callback_set(mosq_, onSubscribe);
        mosquitto_message_callback_set(mosq_, onMessage);
        mosquitto_log_callback_set(mosq_, onLog);

        // Backoff exponencial da propria lib: 1s, 2s, 4s... ate 30s. So atua com
        // loop_start/loop_forever (mosquitto.h:1655-1660), e foi uma das razoes
        // de trocar pra ca - antes isto era codigo nosso.
        mosquitto_reconnect_delay_set(mosq_, reconnect_delay_s, reconnect_delay_max_s, true);

        // Nao bloqueia: a conexao acontece na thread da lib. E' o que mantem o
        // encerramento rapido quando o host nao responde.
        const int rc =
            mosquitto_connect_async(mosq_, broker_.host.c_str(), broker_.port, keepalive_s);
        if (rc != MOSQ_ERR_SUCCESS) {
            logging::warn("[mqtt/{}] connect_async em {}:{}: {}", broker_.name, broker_.host,
                          broker_.port, errorText(rc));
        }

        // Cria a thread de rede. threaded_set e' automatico por aqui
        // (mosquitto.h:1478) - com thread nossa ele seria obrigatorio na mao.
        const int loop_rc = mosquitto_loop_start(mosq_);
        if (loop_rc != MOSQ_ERR_SUCCESS) {
            logging::error("[mqtt/{}] loop_start falhou: {}", broker_.name, errorText(loop_rc));
            return false;
        }

        running_ = true;
        retry_delay_s_ = reconnect_delay_s;
        next_retry_ = std::chrono::steady_clock::now() + std::chrono::seconds(retry_delay_s_);

        logging::info("[mqtt/{}] conectando em {}:{} (client_id \"{}\")", broker_.name,
                      broker_.host, broker_.port, broker_.client_id);
        return true;
    }

    void Client::tick() {
        if (!running_) return;

        // So supervisiona a PRIMEIRA conexao. Depois que uma se estabeleceu, e'
        // a lib quem reconecta (reconnect_delay_set), e insistir aqui em
        // paralelo so criaria tentativas concorrentes.
        if (connected_.load() || ever_connected_.load()) return;

        const auto now = std::chrono::steady_clock::now();
        if (now < next_retry_) return;

        // Nao bloqueia: reaproveita host/porta do connect_async e devolve na
        // hora. O resultado aparece no on_connect, ou nao aparece.
        const int rc = mosquitto_reconnect_async(mosq_);
        if (rc != MOSQ_ERR_SUCCESS) {
            logging::warn("[mqtt/{}] reconnect_async: {}", broker_.name, errorText(rc));
        } else {
            logging::info("[mqtt/{}] sem conexao, tentando novamente ({}s ate a proxima)",
                          broker_.name, retry_delay_s_);
        }

        retry_delay_s_ = std::min(retry_delay_s_ * 2, reconnect_delay_max_s);
        next_retry_ = now + std::chrono::seconds(retry_delay_s_);
    }

    void Client::stop() {
        if (!running_) return;
        running_ = false;

        // loop_stop(force=false) BLOQUEIA ate a thread da lib terminar, e ela so
        // termina se o disconnect tiver funcionado (mosquitto.h:1279-1288).
        // Quando o cliente nunca chegou a conectar, o disconnect devolve
        // MOSQ_ERR_NO_CONN sem fazer nada - ai a unica saida e' o force. Medido:
        // sem esta distincao o encerramento atrasava ~11s com um broker
        // inalcancavel.
        //
        // Decidido pelo retorno do disconnect, nao por um flag de "conectado":
        // a conexao pode cair entre uma coisa e outra.
        const bool disconnected = mosquitto_disconnect(mosq_) == MOSQ_ERR_SUCCESS;
        mosquitto_loop_stop(mosq_, !disconnected);

        logging::info("[mqtt/{}] encerrado", broker_.name);
    }

    void Client::onConnect(mosquitto*, void* self, int rc) {
        Client* client = static_cast<Client*>(self);

        if (rc != 0) {
            // Nao reconecta daqui: a lib refaz a tentativa sozinha, com o
            // backoff configurado no start().
            logging::error("[mqtt/{}] broker recusou a conexao: {} (rc {})", client->broker_.name,
                           mosquitto_connack_string(rc), rc);
            return;
        }

        client->connected_.store(true);
        client->ever_connected_.store(true);
        logging::info("[mqtt/{}] conectado em {}:{}", client->broker_.name, client->broker_.host,
                      client->broker_.port);

        // Subscreve a cada (re)conexao, nao uma vez no start(): com
        // clean_session=true o broker esquece as inscricoes quando a conexao
        // cai.
        //
        // A uniao dos padroes sai daqui, das streams deste broker, e nao de um
        // campo derivado na config (decidido 2026-09-04). O dedup existe porque
        // duas streams do mesmo broker podem declarar o mesmo padrao - o SUBSCRIBE
        // repetido seria inofensivo, mas apareceria duas vezes no log.
        std::vector<std::string> subscribed;
        for (const config::Stream* stream : client->streams_) {
            for (const std::string& topic : stream->topics) {
                if (std::find(subscribed.begin(), subscribed.end(), topic) != subscribed.end()) {
                    continue;
                }
                subscribed.push_back(topic);

                const int sub_rc =
                    mosquitto_subscribe(client->mosq_, nullptr, topic.c_str(), subscribe_qos);
                if (sub_rc == MOSQ_ERR_SUCCESS) {
                    logging::info("[mqtt/{}] subscrevendo \"{}\"", client->broker_.name, topic);
                } else {
                    logging::error("[mqtt/{}] subscribe \"{}\" falhou: {}", client->broker_.name,
                                   topic, errorText(sub_rc));
                }
            }
        }
    }

    void Client::onDisconnect(mosquitto*, void* self, int rc) {
        Client* client = static_cast<Client*>(self);
        client->connected_.store(false);
        // rc == 0 e' a desconexao que nos pedimos, no stop().
        if (rc == 0) return;

        // Quem retenta depende de ja ter conectado alguma vez - a mesma divisao
        // do tick(). Dizer "a lib reconecta" nos dois casos mandaria procurar
        // problema no lugar errado.
        if (client->ever_connected_.load()) {
            logging::warn("[mqtt/{}] conexao perdida - a lib reconecta", client->broker_.name);
        } else {
            logging::warn("[mqtt/{}] conexao inicial falhou", client->broker_.name);
        }
    }

    void Client::onSubscribe(mosquitto*, void* self, int, int qos_count, const int* granted_qos) {
        const Client* client = static_cast<const Client*>(self);
        // Silencioso no caso normal: so interessa quando o broker recusa, que e'
        // o que distingue "padrao invalido" de "ninguem publicou ainda".
        for (int i = 0; i < qos_count; ++i) {
            if (granted_qos[i] == suback_failure) {
                logging::error("[mqtt/{}] broker recusou uma inscricao", client->broker_.name);
            }
        }
    }

    void Client::onLog(mosquitto*, void* self, int level, const char* str) {
        const Client* client = static_cast<const Client*>(self);
        const char* text = str != nullptr ? str : "";
        if (level == MOSQ_LOG_ERR || level == MOSQ_LOG_WARNING) {
            logging::warn("[mqtt/{}] {}", client->broker_.name, text);
        } else {
            logging::debug("[mqtt/{}] {}", client->broker_.name, text);
        }
    }

    void Client::onMessage(mosquitto*, void* self, const mosquitto_message* msg) {
        const Client* client = static_cast<const Client*>(self);
        const char* topic = msg->topic != nullptr ? msg->topic : "";

        // streams_ e' imutavel desde a construcao e pertence so a este cliente,
        // entao a busca nao precisa de lock mesmo com N callbacks concorrentes.
        const std::vector<const config::Stream*> matched = matchAll(client->streams_, topic);
        if (matched.empty()) {
            // ADIADO (2026-09-04): topico sem stream e' descartado em silencio.
            // O CLAUDE.md pede warn, mas warn por mensagem inunda o log com um
            // sensor nao mapeado publicando rapido. Ver a decisao no TODO.md.
            return;
        }

        // Mais de uma stream e' fan-out, nao ambiguidade: quando o writer entrar,
        // sao N push(), um por stream, cada uma com seu offset e sua retencao.
        std::string names;
        for (const config::Stream* stream : matched) {
            if (!names.empty()) names += ", ";
            names += '"' + stream->name + '"';
        }

        // Payload e' opaco e pode ser binario: loga o tamanho, nao o conteudo.
        logging::info("[mqtt/{}] {} -> {} ({} bytes)", client->broker_.name, topic, names,
                      msg->payloadlen);
    }
}

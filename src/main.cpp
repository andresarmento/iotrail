/*
 *  IoTrail - sistema leve de persistencia e streaming de eventos para IoT.
 *
 *  Copyright André Sarmento - 2026
 */

#include <vector>
#include <thread>
#include <memory>
#include <chrono>

#include "logging.h"
#include "client.h"
#include "cmdline.h"
#include "config.h"
#include "meta.h"
#include "mqtt.h"
#include "signals.h"


int main(int argc, char* argv[]) {
    logging::init();

    // Command line options
    const auto opts = cmdline::parse(argc, argv);
    if (!opts) {
        logging::shutdown();
        return 1;
    }
  
    // Log level
    if (opts->verbose == 1) {
        logging::set_level(logging::level::debug);
    } else if (opts->verbose >= 2) {
        logging::set_level(logging::level::trace);
    }
  
    // Config file
    logging::debug("config: {}", opts->config.string());
    const auto settings = config::load(opts->config);
    if (!settings) {
        logging::shutdown();
        return 1;
    }

    // Debug config
    logging::info("config lida: {} broker(s), {} stream(s), data_dir {}",
                  settings->brokers.size(), settings->streams.size(),
                  settings->data_dir.string());
    for (const auto& br : settings->brokers) {
        logging::debug("  broker {} -> {}:{} (client_id {}, keepalive {}s)", br.name, br.host,
                       br.port, br.client_id, br.keepalive);
    }
    for (const auto& st : settings->streams) {
        logging::debug("  stream {} <- broker {}, {} topico(s)", st.name, st.broker,
                       st.topics.size());
        for (const auto& topic : st.topics) {
            logging::trace("    {}", topic);
        }
    }

    // Tabelas de topicos, uma por stream, abertas ANTES de conectar em broker
    // nenhum: criar pasta, arquivo e sync_dir e' a operacao mais lenta do
    // modulo, e sob demanda ela cairia dentro do on_message. Num edge
    // desatendido, descobrir no boot que o disco nao deixa escrever vale mais
    // que descobrir as 3h, com a primeira mensagem na mao.
    //
    // Vetor paralelo a settings->streams - mesma ordem, mesmo indice. unique_ptr
    // porque o meta segura um descritor e nao e' movivel, e nulo marca a stream
    // que nao abriu. Declarado aqui, antes dos clientes, pra ser destruido
    // depois deles.
    std::vector<std::unique_ptr<storage::meta>> metas;
    metas.reserve(settings->streams.size());
    for (const auto& st : settings->streams) {
        auto m = std::make_unique<storage::meta>();
        if (!m->open(settings->data_dir, st.name)) m.reset();  // o motivo ja foi logado
        metas.push_back(std::move(m));
    }

    // Setup MQTT clients
    if (!mqtt::init()) {
        logging::shutdown();
        return 1;
    }

    std::vector<std::unique_ptr<mqtt::client>> clients;
    for (const auto& br : settings->brokers) {
        // Obtem as streams para o broker em questão, cada uma com a tabela de
        // topicos dela ao lado. A stream cujo .meta nao abriu fica de fora: sem
        // tabela nao ha como gravar, e subscrever pra descartar gastaria banda
        // e log sem guardar nada.
        std::vector<mqtt::client::target> streams;
        for (size_t i = 0; i < settings->streams.size(); ++i) {
            const config::stream& st = settings->streams[i];
            if (st.broker != br.name || !metas[i]) continue;
            streams.push_back({&st, metas[i].get()});
        }

        if (streams.empty()) {
            logging::warn("[mqtt/{}] nenhuma stream gravavel - cliente nao sobe", br.name);
            continue;
        }

        clients.push_back(std::make_unique<mqtt::client>(br, std::move(streams)));
        if (!clients.back()->start()) {
            clients.clear();
            mqtt::shutdown();
            logging::shutdown();
            return 1;
        }
    }

    // Sem cliente nenhum nao ha o que receber: e' o caso de toda stream ter
    // falhado ao abrir, e ficar de pe so pra dormir 200 ms em loop esconderia
    // isso do operador.
    if (clients.empty()) {
        logging::error("nenhum cliente subiu - nada a fazer");
        mqtt::shutdown();
        logging::shutdown();
        return 1;
    }

    // Setup signals
    signals::init();

    // Loop thread main
    logging::info("IoTrail subiu, Ctrl+C para encerrar");

    while (!signals::stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // Shutdown
    logging::info("IoTrail encerrando");
    clients.clear(); // para e destroi cada cliente ANTES do lib_cleanup
    mqtt::shutdown();
    logging::shutdown();
    signals::shutdown_done(); // Deve ser a última linha
    return 0;
}

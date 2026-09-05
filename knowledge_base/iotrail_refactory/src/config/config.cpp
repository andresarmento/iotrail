#include "config.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>

#include "ini.h"
#include "logging.h"

namespace config {

    // MQTT 3.1.1 garante aceitacao de 1-23 caracteres; acima disso o servidor
    // pode aceitar, e o mosquitto aceita.
    constexpr size_t client_id_max_guaranteed = 23;

    static bool parsePort(const std::string& text, int& out) {
        if (text.empty()) return false;
        char* end = nullptr;
        const long value = std::strtol(text.c_str(), &end, 10);
        // Exigir que o strtol tenha consumido a string inteira rejeita "1883x"
        // e "abc", que std::atoi aceitaria em silencio como 1883 e 0.
        if (end == nullptr || *end != '\0') return false;
        if (value < 1 || value > 65535) return false;
        out = static_cast<int>(value);
        return true;
    }

    static std::vector<std::string> splitList(const std::string& value) {
        std::vector<std::string> topics;
        size_t start = 0;
        while (start <= value.size()) {
            const size_t comma = value.find(',', start);
            const size_t end = (comma == std::string::npos) ? value.size() : comma;

            const std::string item = value.substr(start, end - start);
            const size_t first = item.find_first_not_of(" \t");
            if (first != std::string::npos) {
                const size_t last = item.find_last_not_of(" \t");
                topics.push_back(item.substr(first, last - first + 1));
            }

            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        return topics;
    }

    // Chave escrita errada e' invisivel de outra forma: a config sobe, so que
    // sem o valor que o operador achou que tinha configurado.
    static void warnUnknownKeys(const ini::Section& section,
                                const std::vector<std::string>& known) {
        for (const auto& entry : section.entries) {
            if (std::find(known.begin(), known.end(), entry.key) == known.end()) {
                logging::warn("[config] linha {}: [{}:{}] chave desconhecida \"{}\", ignorada",
                              section.line, section.type, section.name, entry.key);
            }
        }
    }

    bool validStreamName(const std::string& name) {
        if (name.empty()) return false;

        for (const unsigned char c : name) {
            if (std::isalnum(c) == 0 && c != '_' && c != '-') return false;
        }

        // Reservados do DOS: passam na regra de caracteres acima, mas NUL.log
        // nao e' criavel no Windows nem com extensao. Comparacao sem caixa.
        static const std::array<const char*, 22> reserved = {
            "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4",
            "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3",
            "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};

        std::string upper = name;
        for (char& c : upper) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        return std::none_of(reserved.begin(), reserved.end(),
                            [&upper](const char* r) { return upper == r; });
    }

    static bool loadBroker(const ini::Section& section, Config& out) {
        warnUnknownKeys(section, {"type", "host", "port", "client_id"});

        const bool duplicate = std::any_of(
            out.brokers.begin(), out.brokers.end(),
            [&section](const Broker& b) { return b.name == section.name; });
        if (duplicate) {
            logging::error("[config] linha {}: broker \"{}\" declarado mais de uma vez",
                           section.line, section.name);
            return false;
        }

        Broker broker;
        broker.name = section.name;

        if (const std::string* type = section.find("type")) broker.type = *type;
        if (broker.type != "mqtt") {
            logging::error("[config] linha {}: broker \"{}\": protocolo \"{}\" nao suportado",
                           section.line, section.name, broker.type);
            return false;
        }

        // Sem default: 1883 e' a porta padrao do MQTT registrada na IANA, mas
        // 127.0.0.1 nao e' convencao nenhuma pra "onde esta meu broker" -
        // seria escolher um destino que ninguem escreveu.
        const std::string* host = section.find("host");
        if (host == nullptr || host->empty()) {
            logging::error("[config] linha {}: broker \"{}\": falta \"host=\"",
                           section.line, section.name);
            return false;
        }
        broker.host = *host;

        if (const std::string* port = section.find("port")) {
            if (!parsePort(*port, broker.port)) {
                logging::error("[config] linha {}: broker \"{}\": porta invalida \"{}\" (esperado 1-65535)",
                               section.line, section.name, *port);
                return false;
            }
        }

        // Default derivado do nome da secao, que e' o que o src/main.cpp antigo
        // ja fazia. Configuravel porque amarrar a identidade MQTT ao nome da
        // secao faz renomear a secao trocar de sessao em silencio quando
        // clean_session=false - e porque ACL de broker costuma casar por id.
        if (const std::string* client_id = section.find("client_id")) {
            if (client_id->empty()) {
                logging::error("[config] linha {}: broker \"{}\": \"client_id=\" esta vazio",
                               section.line, section.name);
                return false;
            }
            broker.client_id = *client_id;
        } else {
            broker.client_id = "iotrail-" + broker.name;
        }

        // Aviso, nao erro: derrubar o boot por um limite que a maioria dos
        // brokers ignora seria pior que o problema.
        if (broker.client_id.size() > client_id_max_guaranteed) {
            logging::warn("[config] linha {}: broker \"{}\": client_id \"{}\" tem {} caracteres, "
                          "acima dos {} garantidos pelo MQTT 3.1.1 - broker estrito pode recusar",
                          section.line, section.name, broker.client_id,
                          broker.client_id.size(), client_id_max_guaranteed);
        }

        // Dois clientes com o mesmo id no MESMO broker se derrubam
        // (MQTT 3.1.1 [MQTT-3.1.4-2]). Em brokers diferentes ids iguais
        // convivem, entao host e porta entram na checagem.
        const bool clash = std::any_of(
            out.brokers.begin(), out.brokers.end(), [&broker](const Broker& b) {
                return b.client_id == broker.client_id && b.host == broker.host
                       && b.port == broker.port;
            });
        if (clash) {
            logging::warn("[config] linha {}: broker \"{}\": client_id \"{}\" ja usado em {}:{} - "
                          "os dois clientes vao se derrubar",
                          section.line, section.name, broker.client_id, broker.host, broker.port);
        }

        out.brokers.push_back(std::move(broker));
        return true;
    }

    static bool loadStream(const ini::Section& section,
                           const std::vector<std::string>& declared_brokers, Config& out) {
        warnUnknownKeys(section, {"broker", "topics"});

        if (!validStreamName(section.name)) {
            logging::error("[config] linha {}: stream \"{}\": nome invalido (aceito: A-Z a-z 0-9 _ - ; "
                           "reservados do DOS como CON/NUL/COM1 nao sao criaveis no Windows)",
                           section.line, section.name);
            return false;
        }

        const bool duplicate = std::any_of(
            out.streams.begin(), out.streams.end(),
            [&section](const Stream& s) { return s.name == section.name; });
        if (duplicate) {
            logging::error("[config] linha {}: stream \"{}\" declarada mais de uma vez",
                           section.line, section.name);
            return false;
        }

        Stream stream;
        stream.name = section.name;

        // Obrigatorio: sem default "o unico broker declarado". Escolher origem
        // por omissao e' o mesmo problema do host= sem valor - numa config com
        // dois brokers, esquecer a linha faria a stream escutar o outro calada.
        const std::string* broker = section.find("broker");
        if (broker == nullptr) {
            logging::error("[config] linha {}: stream \"{}\": falta \"broker=\"",
                           section.line, section.name);
            return false;
        }

        // Lista rejeitada explicitamente: sem esta checagem "casa,fabrica"
        // viraria um nome literal e o erro sairia como "broker nao declarado",
        // que nao diz ao operador o que ele fez de errado.
        const std::vector<std::string> names = splitList(*broker);
        if (names.empty()) {
            logging::error("[config] linha {}: stream \"{}\": \"broker=\" esta vazio",
                           section.line, section.name);
            return false;
        }
        if (names.size() > 1) {
            logging::error("[config] linha {}: stream \"{}\": \"broker=\" aceita um broker so - "
                           "pra ouvir varios, declare uma stream por broker",
                           section.line, section.name);
            return false;
        }

        stream.broker = names.front();

        // Checado contra os brokers DECLARADOS, nao contra os carregados com
        // sucesso: senao um broker com porta invalida geraria tambem um
        // "broker desconhecido" aqui, culpando a stream por erro alheio.
        if (std::find(declared_brokers.begin(), declared_brokers.end(), stream.broker)
            == declared_brokers.end()) {
            logging::error("[config] linha {}: stream \"{}\": broker \"{}\" nao declarado",
                           section.line, section.name, stream.broker);
            return false;
        }

        const std::string* topics = section.find("topics");
        if (topics == nullptr) {
            logging::error("[config] linha {}: stream \"{}\": falta \"topics=\"",
                           section.line, section.name);
            return false;
        }

        stream.topics = splitList(*topics);
        if (stream.topics.empty()) {
            logging::error("[config] linha {}: stream \"{}\": \"topics=\" esta vazio",
                           section.line, section.name);
            return false;
        }

        out.streams.push_back(std::move(stream));
        return true;
    }

    std::optional<Config> load(const std::filesystem::path& path) {
        const auto sections = ini::parseFile(path.string());
        if (!sections) return std::nullopt;

        Config config;
        bool ok = true;

        // Duas passadas, pra uma stream poder referenciar um broker declarado
        // depois dela. A ordem das secoes nao importa pro vinculo; ela so
        // importa entre streams, onde define o roteamento.
        std::vector<std::string> declared_brokers;
        for (const auto& section : *sections) {
            if (section.type != "broker") continue;
            declared_brokers.push_back(section.name);
            if (!loadBroker(section, config)) ok = false;
        }

        // Uma secao ruim nao interrompe as outras: o operador ve todos os
        // problemas de uma vez, em vez de um por boot.
        for (const auto& section : *sections) {
            if (section.type == "broker") {
                continue;
            } else if (section.type == "stream") {
                if (!loadStream(section, declared_brokers, config)) ok = false;
            } else if (section.type.empty()) {
                logging::error("[config] linha {}: secao \"[{}]\" sem tipo - use \"[broker:{}]\" "
                               "ou \"[stream:{}]\"",
                               section.line, section.name, section.name, section.name);
                ok = false;
            } else {
                logging::warn("[config] linha {}: tipo de secao desconhecido \"{}\", ignorada",
                              section.line, section.type);
            }
        }

        if (config.brokers.empty()) {
            logging::error("[config] nenhum broker configurado");
            ok = false;
        }
        if (config.streams.empty()) {
            logging::error("[config] nenhuma stream configurada");
            ok = false;
        }

        // A config nao deriva nada a partir da relacao broker/stream (decidido
        // 2026-09-04): ela reflete o arquivo e valida. Quem precisa da uniao de
        // topics= por broker e' o cliente MQTT, e ele a monta a partir das
        // proprias streams - uma representacao derivada em vez de duas.
        //
        // Isto aqui e' validacao, nao derivacao: nao e' erro, a config e'
        // valida, mas conectar num broker que nao alimenta stream nenhuma nunca
        // e' o que o operador quis.
        for (const Broker& broker : config.brokers) {
            const bool used =
                std::any_of(config.streams.begin(), config.streams.end(),
                            [&broker](const Stream& s) { return s.broker == broker.name; });
            if (!used) {
                logging::warn("[config] broker \"{}\" nao e' usado por nenhuma stream - vai "
                              "conectar sem subscrever nada",
                              broker.name);
            }
        }

        if (!ok) return std::nullopt;
        return config;
    }
}

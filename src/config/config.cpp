#include "config.h"

#include "logging.h"
#include "paths.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>

namespace fs = std::filesystem;

namespace config {
    // MQTT 3.1.1 garante aceitacao de 1-23 caracteres; acima disso o servidor
    // pode aceitar, e o mosquitto aceita.
    static constexpr size_t client_id_max_guaranteed = 23;
    static constexpr char data_dir_default[] = "data";

    static bool parse_port(const std::string& text, int& out) {
        if (text.empty()) return false;
        char* end = nullptr;
        const long value = std::strtol(text.c_str(), &end, 10);
        // Exigir que o strtol tenha consumido a string inteira rejeita "1883x" e
        // "abc", que std::atoi aceitaria calado como 1883 e 0.
        if (end == nullptr || *end != '\0') return false;
        if (value < 1 || value > 65535) return false;
        out = static_cast<int>(value);
        return true;
    }

    static std::vector<std::string> split_list(const std::string& value) {
        std::vector<std::string> items;
        size_t start = 0;
        while (start <= value.size()) {
            const size_t comma = value.find(',', start);
            const size_t end = (comma == std::string::npos) ? value.size() : comma;

            const std::string item = value.substr(start, end - start);
            const size_t first = item.find_first_not_of(" \t");
            if (first != std::string::npos) {
                const size_t last = item.find_last_not_of(" \t");
                items.push_back(item.substr(first, last - first + 1));
            }

            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        return items;
    }

    // Chave escrita errada e' invisivel de outra forma: a config sobe, so que
    // sem o valor que o operador achou que tinha configurado.
    static void warn_unknown_keys(const ini::section& sec,
                                  const std::vector<std::string>& known) {
        for (const auto& e : sec.entries) {
            if (std::find(known.begin(), known.end(), e.key) == known.end()) {
                logging::warn("[config] linha {}: [{}] chave desconhecida \"{}\", ignorada",
                              sec.line, sec.name, e.key);
            }
        }
    }

    bool valid_stream_name(const std::string& name) {
        if (name.empty()) return false;

        for (const unsigned char c : name) {
            if (std::isalnum(c) == 0 && c != '_' && c != '-') return false;
        }

        // Reservados do DOS: passam na regra de caracteres acima, mas NUL.log
        // nao e' criavel no Windows nem com extensao.
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

    static bool load_general(const ini::section& sec, settings& out) {
        warn_unknown_keys(sec, {"data_dir"});

        std::string value = data_dir_default;
        if (const std::string* data_dir = sec.find("data_dir")) {
            if (data_dir->empty()) {
                logging::error("[config] linha {}: [general]: \"data_dir=\" esta vazio", sec.line);
                return false;
            }
            value = *data_dir;
        }

        // Relativo resolve contra o diretorio do executavel, nao contra o de
        // trabalho nem contra o do -c: mover a config nao move os dados.
        fs::path dir(value);
        if (dir.is_relative()) {
            const fs::path base = paths::exe_dir();
            if (base.empty()) return false;
            dir = base / dir;
        }
        dir = dir.lexically_normal();

        std::error_code ec;
        if (fs::exists(dir, ec) && !fs::is_directory(dir, ec)) {
            logging::error("[config] linha {}: [general]: \"{}\" existe e nao e' um diretorio",
                           sec.line, dir.string());
            return false;
        }

        out.data_dir = dir;
        return true;
    }

    static bool load_broker(const ini::section& sec, settings& out) {
        warn_unknown_keys(sec, {"type", "host", "port", "client_id"});

        broker br;
        br.name = sec.name;

        if (const std::string* type = sec.find("type")) br.type = *type;
        if (br.type != "mqtt") {
            logging::error("[config] linha {}: broker \"{}\": protocolo \"{}\" nao suportado",
                           sec.line, sec.name, br.type);
            return false;
        }

        // Sem default: 1883 e' a porta padrao do MQTT na IANA, mas 127.0.0.1 nao
        // e' convencao nenhuma pra "onde esta meu broker".
        const std::string* host = sec.find("host");
        if (host == nullptr || host->empty()) {
            logging::error("[config] linha {}: broker \"{}\": falta \"host=\"", sec.line, sec.name);
            return false;
        }
        br.host = *host;

        if (const std::string* port = sec.find("port")) {
            if (!parse_port(*port, br.port)) {
                logging::error("[config] linha {}: broker \"{}\": porta invalida \"{}\" "
                               "(esperado 1-65535)",
                               sec.line, sec.name, *port);
                return false;
            }
        }

        if (const std::string* client_id = sec.find("client_id")) {
            if (client_id->empty()) {
                logging::error("[config] linha {}: broker \"{}\": \"client_id=\" esta vazio",
                               sec.line, sec.name);
                return false;
            }
            br.client_id = *client_id;
        } else {
            br.client_id = "iotrail-" + br.name;
        }

        if (br.client_id.size() > client_id_max_guaranteed) {
            logging::warn("[config] linha {}: broker \"{}\": client_id \"{}\" tem {} caracteres, "
                          "acima dos {} garantidos pelo MQTT 3.1.1 - broker estrito pode recusar",
                          sec.line, sec.name, br.client_id, br.client_id.size(),
                          client_id_max_guaranteed);
        }

        // Dois clientes com o mesmo id no MESMO broker se derrubam
        // (MQTT 3.1.1 [MQTT-3.1.4-2]); em brokers diferentes convivem.
        const bool clash = std::any_of(
            out.brokers.begin(), out.brokers.end(), [&br](const broker& b) {
                return b.client_id == br.client_id && b.host == br.host && b.port == br.port;
            });
        if (clash) {
            logging::warn("[config] linha {}: broker \"{}\": client_id \"{}\" ja usado em {}:{} - "
                          "os dois clientes vao se derrubar",
                          sec.line, sec.name, br.client_id, br.host, br.port);
        }

        out.brokers.push_back(std::move(br));
        return true;
    }

    static bool load_stream(const ini::section& sec,
                            const std::vector<std::string>& declared_brokers, settings& out) {
        warn_unknown_keys(sec, {"broker", "topics"});

        if (!valid_stream_name(sec.name)) {
            logging::error("[config] linha {}: stream \"{}\": nome invalido (aceito: A-Z a-z 0-9 "
                           "_ - ; reservados do DOS como CON/NUL/COM1 nao sao criaveis no Windows)",
                           sec.line, sec.name);
            return false;
        }

        stream st;
        st.name = sec.name;

        const std::string* broker_key = sec.find("broker");
        if (broker_key == nullptr) {
            logging::error("[config] linha {}: stream \"{}\": falta \"broker=\"",
                           sec.line, sec.name);
            return false;
        }

        // Lista rejeitada na cara: sem isto "casa,fabrica" viraria um nome
        // literal e o erro sairia como "broker nao declarado", que nao diz ao
        // operador o que ele fez de errado.
        const std::vector<std::string> names = split_list(*broker_key);
        if (names.empty()) {
            logging::error("[config] linha {}: stream \"{}\": \"broker=\" esta vazio",
                           sec.line, sec.name);
            return false;
        }
        if (names.size() > 1) {
            logging::error("[config] linha {}: stream \"{}\": \"broker=\" aceita um broker so - "
                           "pra ouvir varios, declare uma stream por broker",
                           sec.line, sec.name);
            return false;
        }
        st.broker = names.front();

        // Contra os brokers DECLARADOS, nao contra os que carregaram com
        // sucesso: senao um broker com porta invalida geraria tambem um "broker
        // desconhecido" aqui, culpando a stream por erro alheio.
        if (std::find(declared_brokers.begin(), declared_brokers.end(), st.broker)
            == declared_brokers.end()) {
            logging::error("[config] linha {}: stream \"{}\": broker \"{}\" nao declarado",
                           sec.line, sec.name, st.broker);
            return false;
        }

        const std::string* topics = sec.find("topics");
        if (topics == nullptr) {
            logging::error("[config] linha {}: stream \"{}\": falta \"topics=\"",
                           sec.line, sec.name);
            return false;
        }

        // Padrao de topico nao e' validado aqui: quem diz se um filtro e' legal
        // e' o broker, na subscricao (Fase 2).
        st.topics = split_list(*topics);
        if (st.topics.empty()) {
            logging::error("[config] linha {}: stream \"{}\": \"topics=\" esta vazio",
                           sec.line, sec.name);
            return false;
        }

        out.streams.push_back(std::move(st));
        return true;
    }

    std::optional<settings> validate(const ini::sections& sections) {
        settings out;
        bool ok = true;

        // Primeira passada so junta os nomes, sem validar nada: e' o que permite
        // uma stream citar um broker declarado abaixo dela sem que as mensagens
        // saiam fora da ordem do arquivo.
        std::vector<std::string> declared_brokers;
        for (const auto& sec : sections) {
            if (sec.type == "broker") declared_brokers.push_back(sec.name);
        }

        // Nomes JA VISTOS, nao os carregados com sucesso: senao duas secoes com
        // o mesmo nome, a primeira com outro erro, passariam como uma so - e a
        // duplicata so apareceria no boot seguinte.
        std::vector<std::string> seen_brokers;
        std::vector<std::string> seen_streams;
        bool seen_general = false;

        // Uma secao ruim nao interrompe as outras.
        for (const auto& sec : sections) {
            if (sec.type == "broker") {
                if (std::find(seen_brokers.begin(), seen_brokers.end(), sec.name)
                    != seen_brokers.end()) {
                    logging::error("[config] linha {}: broker \"{}\" declarado mais de uma vez",
                                   sec.line, sec.name);
                    ok = false;
                    continue;
                }
                seen_brokers.push_back(sec.name);
                if (!load_broker(sec, out)) ok = false;
            } else if (sec.type == "stream") {
                if (std::find(seen_streams.begin(), seen_streams.end(), sec.name)
                    != seen_streams.end()) {
                    logging::error("[config] linha {}: stream \"{}\" declarada mais de uma vez",
                                   sec.line, sec.name);
                    ok = false;
                    continue;
                }
                seen_streams.push_back(sec.name);
                if (!load_stream(sec, declared_brokers, out)) ok = false;
            } else if (sec.type.empty() && sec.name == "general") {
                if (seen_general) {
                    logging::error("[config] linha {}: [general] declarada mais de uma vez",
                                   sec.line);
                    ok = false;
                    continue;
                }
                seen_general = true;
                if (!load_general(sec, out)) ok = false;
            } else if (sec.type.empty()) {
                logging::error("[config] linha {}: secao \"[{}]\" sem tipo - use \"[broker:{}]\" "
                               "ou \"[stream:{}]\"",
                               sec.line, sec.name, sec.name, sec.name);
                ok = false;
            } else {
                logging::warn("[config] linha {}: tipo de secao desconhecido \"{}\", ignorada",
                              sec.line, sec.type);
            }
        }

        // [general] e' opcional; sem ela vale o default.
        if (out.data_dir.empty()) {
            const fs::path base = paths::exe_dir();
            if (base.empty()) {
                ok = false;
            } else {
                out.data_dir = (base / data_dir_default).lexically_normal();
            }
        }

        if (out.brokers.empty()) {
            logging::error("[config] nenhum broker configurado");
            ok = false;
        }
        if (out.streams.empty()) {
            logging::error("[config] nenhuma stream configurada");
            ok = false;
        }

        // Config valida, mas conectar num broker que nao alimenta stream nenhuma
        // nunca e' o que o operador quis.
        for (const broker& br : out.brokers) {
            const bool used = std::any_of(out.streams.begin(), out.streams.end(),
                                          [&br](const stream& s) { return s.broker == br.name; });
            if (!used) {
                logging::warn("[config] broker \"{}\" nao e' usado por nenhuma stream - vai "
                              "conectar sem subscrever nada",
                              br.name);
            }
        }

        if (!ok) return std::nullopt;
        return out;
    }

    std::optional<settings> load(const fs::path& path) {
        const auto sections = ini::parse_file(path);
        if (!sections) return std::nullopt;
        return validate(*sections);
    }
}

#include "ini.h"

#include "logging.h"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <istream>

namespace fs = std::filesystem;

namespace ini {
    static std::string trim(const std::string& s) {
        const size_t start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return {};
        const size_t end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }

    const std::string* section::find(const std::string& key) const {
        for (const auto& e : entries) {
            if (e.key == key) return &e.value;
        }
        return nullptr;
    }

    std::optional<sections> parse(std::istream& in, const std::string& origin) {
        sections result;
        std::string raw;
        int line_number = 0;
        int errors = 0;

        while (std::getline(in, raw)) {
            ++line_number;

            // BOM UTF-8: o Notepad e o VS Code gravam por padrao no Windows, e
            // sem tirar os 3 bytes a primeira secao do arquivo fica invisivel -
            // o '[' deixa de ser o primeiro caractere da linha.
            if (line_number == 1 && raw.rfind("\xEF\xBB\xBF", 0) == 0) {
                raw.erase(0, 3);
            }

            const std::string line = trim(raw);

            if (line.empty() || line.front() == '#' || line.front() == ';') continue;

            if (line.front() == '[') {
                if (line.back() != ']') {
                    logging::error("{}:{}: cabecalho de secao sem ']' no fim", origin, line_number);
                    ++errors;
                    continue;
                }

                const std::string header = trim(line.substr(1, line.size() - 2));

                section sec;
                sec.line = line_number;

                const size_t colon = header.find(':');
                if (colon == std::string::npos) {
                    sec.name = header;
                } else {
                    sec.type = trim(header.substr(0, colon));
                    sec.name = trim(header.substr(colon + 1));
                }

                if (sec.name.empty()) {
                    logging::error("{}:{}: secao sem nome: \"{}\"", origin, line_number, line);
                    ++errors;
                    continue;
                }

                result.push_back(std::move(sec));
                continue;
            }

            const size_t equals = line.find('=');
            if (equals == std::string::npos) {
                logging::error("{}:{}: linha sem '=': \"{}\"", origin, line_number, line);
                ++errors;
                continue;
            }

            const std::string key = trim(line.substr(0, equals));
            const std::string value = trim(line.substr(equals + 1));

            if (key.empty()) {
                logging::error("{}:{}: chave vazia antes do '='", origin, line_number);
                ++errors;
                continue;
            }

            if (result.empty()) {
                logging::error("{}:{}: \"{}\" aparece antes de qualquer secao",
                               origin, line_number, key);
                ++errors;
                continue;
            }

            section& current = result.back();
            if (current.find(key) != nullptr) {
                logging::error("{}:{}: chave \"{}\" repetida na secao \"{}\"",
                               origin, line_number, key, current.name);
                ++errors;
                continue;
            }

            current.entries.push_back({key, value});
        }

        if (errors > 0) {
            logging::error("{}: {} erro(s) de sintaxe", origin, errors);
            return std::nullopt;
        }
        return result;
    }

    std::optional<sections> parse_file(const fs::path& path) {
        errno = 0;
        std::ifstream file(path);
        if (!file) {
            const int err = errno;
            logging::error("nao consegui abrir \"{}\": {}", path.string(),
                           err != 0 ? std::strerror(err) : "motivo desconhecido");
            return std::nullopt;
        }
        return parse(file, path.string());
    }
}

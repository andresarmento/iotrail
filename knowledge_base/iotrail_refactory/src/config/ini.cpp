#include "ini.h"

#include <fstream>

#include "logging.h"

namespace ini {

    static std::string trim(const std::string& s) {
        const size_t start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return {};
        const size_t end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }

    const std::string* Section::find(const std::string& key) const {
        for (const auto& entry : entries) {
            if (entry.key == key) return &entry.value;
        }
        return nullptr;
    }

    std::optional<Sections> parseFile(const std::string& path) {
        Sections sections;

        std::ifstream file(path);
        if (!file) {
            logging::error("[config] nao foi possivel abrir \"{}\"", path);
            return std::nullopt;
        }

        std::string raw;
        int line_number = 0;
        bool ok = true;

        while (std::getline(file, raw)) {
            ++line_number;

            // BOM UTF-8. Notepad e VS Code gravam por padrao no Windows, e sem
            // remover a primeira secao do arquivo fica invisivel pro parser.
            if (line_number == 1 && raw.rfind("\xEF\xBB\xBF", 0) == 0) {
                raw.erase(0, 3);
            }

            const std::string line = trim(raw);
            if (line.empty() || line.front() == '#' || line.front() == ';') continue;

            if (line.front() == '[') {
                if (line.back() != ']') {
                    logging::error("[config] {}:{}: cabecalho de secao sem ']' no fim",
                                   path, line_number);
                    ok = false;
                    continue;
                }

                const std::string header = trim(line.substr(1, line.size() - 2));

                Section section;
                section.line = line_number;

                const size_t colon = header.find(':');
                if (colon == std::string::npos) {
                    section.name = header;
                } else {
                    section.type = trim(header.substr(0, colon));
                    section.name = trim(header.substr(colon + 1));
                }

                if (section.name.empty()) {
                    logging::error("[config] {}:{}: secao sem nome: \"{}\"",
                                   path, line_number, line);
                    ok = false;
                    continue;
                }

                sections.push_back(std::move(section));
                continue;
            }

            const size_t equals = line.find('=');
            if (equals == std::string::npos) {
                logging::error("[config] {}:{}: linha sem '=': \"{}\"", path, line_number, line);
                ok = false;
                continue;
            }

            const std::string key = trim(line.substr(0, equals));
            const std::string value = trim(line.substr(equals + 1));

            if (key.empty()) {
                logging::error("[config] {}:{}: chave vazia antes do '='", path, line_number);
                ok = false;
                continue;
            }

            // Par fora de qualquer secao. Silenciar seria perder config que o
            // operador escreveu achando que valia.
            if (sections.empty()) {
                logging::error("[config] {}:{}: \"{}\" aparece antes de qualquer secao",
                               path, line_number, key);
                ok = false;
                continue;
            }

            Section& current = sections.back();
            if (current.find(key) != nullptr) {
                logging::error("[config] {}:{}: chave \"{}\" repetida na secao \"{}\"",
                               path, line_number, key, current.name);
                ok = false;
                continue;
            }

            current.entries.push_back({key, value});
        }

        if (!ok) return std::nullopt;
        return sections;
    }
}

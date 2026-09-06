/*
 *  IoTrail - sistema leve de persistencia e streaming de eventos para IoT.
 *  Copyright Andre Sarmento - 2026
 *
 *  Parser INI generico
 */
#pragma once
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace ini {
    struct entry {
        std::string key;
        std::string value;
    };

    // Cabecalho aceito nas duas formas:
    //     [broker:casa]  -> type = "broker", name = "casa"
    //     [casa]         -> type = "",       name = "casa"
    // Exigir o tipo e' regra de dominio, nao do parser.
    struct section {
        std::string type;
        std::string name;
        int line = 0;
        std::vector<entry> entries;

        const std::string* find(const std::string& key) const;
    };

    using sections = std::vector<section>;

    std::optional<sections> parse(std::istream& in, const std::string& origin);
    std::optional<sections> parse_file(const std::filesystem::path& path);
}

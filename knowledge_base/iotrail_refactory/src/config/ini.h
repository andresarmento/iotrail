#pragma once

#include <optional>
#include <string>
#include <vector>

// Parser INI generico: quebra o arquivo em secoes e pares chave=valor
namespace ini {
    struct Entry {
        std::string key;
        std::string value;
    };

    // Cabecalho aceito nas duas formas:
    //     [broker:casa]  -> type = "broker", name = "casa"
    //     [casa]         -> type = "",       name = "casa"
    // O parser aceita as duas; exigir o tipo e' regra de dominio, fica no
    // config.cpp.
    struct Section {
        std::string type;
        std::string name;
        int line = 0;

        std::vector<Entry> entries;

        const std::string* find(const std::string& key) const;
    };

    using Sections = std::vector<Section>;

    // Secoes na ordem de declaracao.
    //
    // nullopt se o arquivo nao abrir ou tiver qualquer erro de sintaxe. Mesmo
    // assim o arquivo e' lido ate o fim e TODOS os problemas vao pro log - o
    // operador ve tudo o que precisa corrigir de uma vez, nao um erro por boot.
    std::optional<Sections> parseFile(const std::string& path);
}

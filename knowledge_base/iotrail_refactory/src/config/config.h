#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace config {
    struct Broker {
        std::string name;
        std::string type = "mqtt";
        std::string host;
        int port = 1883;
        // Default derivado: "iotrail-<name>". Configuravel porque o ClientId e'
        // a chave da sessao no broker e ACL de broker costuma casar por ele.
        std::string client_id;
    };

    // Um broker por stream (decidido 2026-09-03). O registro no segmento nao
    // guarda a origem (docs/formato_segmento.md secao 4), entao agregar brokers
    // numa stream descartaria a proveniencia na hora da gravacao, sem volta. O
    // log unificado se reconstroi na leitura, juntando streams por timestamp.
    struct Stream {
        std::string name;
        std::string broker;
        std::vector<std::string> topics;
    };

    struct Config {
        std::vector<Broker> brokers;
        std::vector<Stream> streams;
    };

    std::optional<Config> load(const std::filesystem::path& path);
    bool validStreamName(const std::string& name);
}

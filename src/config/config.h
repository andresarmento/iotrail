/*
 *  IoTrail - sistema leve de persistencia e streaming de eventos para IoT.
 *  Copyright Andre Sarmento - 2026
 *
 *  Regras do dominio sobre o que o ini leu
 */
#pragma once
#include "ini.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace config {
    struct broker {
        std::string name;
        std::string type = "mqtt";
        std::string host;
        int port = 1883;
        std::string client_id;
    };

    struct stream {
        std::string name;
        std::string broker;
        std::vector<std::string> topics;
    };

    struct settings {
        // [general]. Ja resolvido pra absoluto.
        std::filesystem::path data_dir;

        std::vector<broker> brokers;
        std::vector<stream> streams;
    };

    // Reflete o arquivo e valida; nao deriva nada. A uniao de topics= por broker
    // e' o cliente MQTT que monta a partir das streams (Fase 2).
    //
    // nullopt quando algo fatal apareceu - avisos nao impedem o boot. Como no
    // ini, tudo e' checado antes de desistir e os problemas ja foram logados.
    std::optional<settings> validate(const ini::sections& sections);
    std::optional<settings> load(const std::filesystem::path& path);

    bool valid_stream_name(const std::string& name);
}

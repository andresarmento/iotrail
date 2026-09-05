#include "config.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cstdlib>
#include <fstream>

namespace {
    std::string trim(const std::string& s) {
        size_t start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return "";
        size_t end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }
}

std::vector<BrokerConfig> loadConfig(const std::string& path) {
    std::vector<BrokerConfig> brokers;
    std::ifstream file(path);
    if (!file) {
        spdlog::warn("[config] \"{}\" not found", path);
        return brokers;
    }

    BrokerConfig* current = nullptr;
    std::string line;
    while (std::getline(file, line)) {
        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') continue;

        if (trimmed.front() == '[' && trimmed.back() == ']') {
            brokers.push_back(BrokerConfig{});
            current = &brokers.back();
            current->name = trimmed.substr(1, trimmed.size() - 2);
            continue;
        }

        if (current == nullptr) continue; 

        size_t eq = trimmed.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(trimmed.substr(0, eq));
        std::string value = trim(trimmed.substr(eq + 1));

        if (key == "type") {
            current->type = value;
        } else if (key == "host") {
            current->host = value;
        } else if (key == "port") {
            current->port = std::atoi(value.c_str());
        }
    }

    brokers.erase(
        std::remove_if(brokers.begin(), brokers.end(), [](const BrokerConfig& b) {
            if (b.type != "mqtt") {
                spdlog::warn("[config] broker \"{}\": Not supported protocol (\"{}\")", b.name, b.type);
                return true;
            }
                return false;
            }
    ), brokers.end());
    
    spdlog::info("[config] \"{}\": {} valid broker(s)", path, brokers.size());
    return brokers;
}

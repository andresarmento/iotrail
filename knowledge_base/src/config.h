#pragma once

#include <string>
#include <vector>

struct BrokerConfig {
  std::string name;
  std::string type = "mqtt";
  std::string host = "127.0.0.1";
  int port = 1883;
};

std::vector<BrokerConfig> loadConfig(const std::string& path);

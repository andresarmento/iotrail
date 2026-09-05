/*********************************************************************************************
 *  IoTrail é um sistema leve de persistência e streaming de eventos para ambientes IoT
 *  
 *  
 *  Copyright André Sarmento - 2026
 *********************************************************************************************/

#include <mosquittopp.h>
#include "config.h"
#include "init.h"
#include "iotrail_client.h"
#include "segment_writer.h"

int main() {
    init();

    // Read config file
    std::vector<BrokerConfig> brokers = loadConfig("iotrail.conf");
  
    if (brokers.empty()) {
        spdlog::warn("[config] no valid brokers, using default (127.0.0.1:1883)");
        brokers.push_back(BrokerConfig{"default", "mqtt", "127.0.0.1", 1883});
    } else if (brokers.size() > 1) {
        spdlog::warn("[config] {} brokers found, connecting on first broker "
        "(\"{}\") multi-broker not implemented yet", brokers.size(), brokers.front().name);
    }
    const BrokerConfig& cfg = brokers.front();

    SegmentWriter segmentWriter("data", "default");
    mosqpp::lib_init();
    IotrailClient client(("iotrail-" + cfg.name).c_str(), cfg, segmentWriter);

    while (!stopRequested()) {
        int rc = client.loop(200);
        if (rc != MOSQ_ERR_SUCCESS) {
        spdlog::warn("[client] loop() erro: {}, trying to reconnect...", mosqpp::strerror(rc));
        client.reconnect();
        }
    }

    client.disconnect();
    mosqpp::lib_cleanup();
    segmentWriter.stop();
    return 0;
}

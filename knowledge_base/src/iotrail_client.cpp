#include "iotrail_client.h"
#include <spdlog/spdlog.h>
#include <string_view>

namespace {
  constexpr const char* kTopic = "#";
}

IotrailClient::IotrailClient(const char* id, const BrokerConfig& cfg, SegmentWriter& writer) : mosquittopp(id), writer_(writer) {
    spdlog::info("[client] conectando no broker \"{}\" ({}:{})...", cfg.name, cfg.host, cfg.port);
    int rc = connect(cfg.host.c_str(), cfg.port, 60);
    if (rc != MOSQ_ERR_SUCCESS) {
        spdlog::error("[client] connect() falhou: {}", mosqpp::strerror(rc));
    }
}

void IotrailClient::on_connect(int rc) {
    if (rc == 0) {
        spdlog::info("[client] conectado. subscrevendo em \"{}\"...", kTopic);
        subscribe(nullptr, kTopic, 0);
    } else {
        spdlog::warn("[client] conexao recusada, rc={} ({})", rc, mosqpp::connack_string(rc));
    }
}

void IotrailClient::on_disconnect(int rc) {
    spdlog::info("[client] desconectado, rc={}", rc);
}

void IotrailClient::on_subscribe(int mid, int qosCount, const int* grantedQos) {
    spdlog::info("[client] subscribe confirmado (mid={}, qos[0]={})", mid, qosCount > 0 ? grantedQos[0] : -1);
}

void IotrailClient::on_message(const mosquitto_message* msg) {
    std::string_view payload(static_cast<const char*>(msg->payload), msg->payloadlen);
    spdlog::info("[msg] topico=\"{}\" payload=\"{}\" ({} bytes)", msg->topic, payload, msg->payloadlen);
    writer_.push(msg->topic, std::string(payload));
}

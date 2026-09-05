#pragma once

#include <mosquittopp.h>
#include "config.h"
#include "segment_writer.h"

class IotrailClient : public mosqpp::mosquittopp {
    public:
        IotrailClient(const char* id, const BrokerConfig& cfg, SegmentWriter& writer);

        void on_connect(int rc) override;
        void on_disconnect(int rc) override;
        void on_subscribe(int mid, int qosCount, const int* grantedQos) override;
        void on_message(const mosquitto_message* msg) override;

    private:
        SegmentWriter& writer_;
};

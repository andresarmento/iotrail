#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

class SegmentWriter {
    public:
        // Cria <dataDir>/<streamName>/ se nao existir. Se o segmento ja existir de
        // uma execucao anterior, ele NAO e' truncado: o construtor roda a varredura
        // de recuperacao (docs/formato_segmento.md secao 7), descarta um eventual
        // registro parcial no fim e retoma o contador de offset de onde parou.
        SegmentWriter(const std::string& dataDir, const std::string& streamName);
        ~SegmentWriter();

        SegmentWriter(const SegmentWriter&) = delete;
        SegmentWriter& operator=(const SegmentWriter&) = delete;

        void push(std::string topic, std::string payload);

        void stop();

    private:
        struct QueuedRecord {
            uint64_t timestamp_ms;
            std::string topic;
            std::string payload;
        };

        void writerLoop();
        void drainAndWrite();
        void maybeSync();
        bool openSegment();
        bool recoverExisting();

        std::string streamName_;
        std::string path_;
        FILE* file_ = nullptr;

        // Proximo offset a ser atribuido. Vem da varredura de recuperacao quando o
        // segmento ja existia; comeca em 0 num segmento novo.
        uint64_t nextOffset_ = 0;

        std::deque<QueuedRecord> queue_;
        std::mutex queueMutex_;
        std::atomic<bool> stopRequested_{false};
        std::thread thread_;
        std::chrono::steady_clock::time_point lastSync_;
};

#include "segment_writer.h"

#include <spdlog/spdlog.h>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <utility>
#include <vector>

#include "crc32.h"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

constexpr auto kWriteInterval = std::chrono::milliseconds(300);
constexpr auto kSyncInterval = std::chrono::milliseconds(1000);

// Formato em disco - ver docs/formato_segmento.md, que e' a fonte de verdade.
constexpr char kMagic[4] = {'I', 'O', 'T', 'R'};
constexpr uint16_t kFormatVersion = 1;

// Limites de sanidade (secao 5 da spec). Servem em dois momentos: barram na
// entrada (push) o que nao poderia ser gravado, e limitam a leitura na
// varredura de recuperacao antes que o CRC possa validar os tamanhos.
constexpr uint16_t kMaxTopicLen = 1024;
constexpr uint32_t kMaxPayloadLen = 1024 * 1024;  // 1 MiB

#pragma pack(push, 1)
struct SegmentHeader {
  char magic[4];
  uint16_t format_version;
  uint64_t base_offset;
};

struct RecordHeader {
  uint32_t crc32;
  uint64_t offset;
  uint64_t timestamp_ms;
  uint16_t topic_len;
  uint32_t payload_len;
};
#pragma pack(pop)

static_assert(sizeof(SegmentHeader) == 14, "SegmentHeader precisa ser compacto, sem padding");
static_assert(sizeof(RecordHeader) == 26, "RecordHeader precisa ser compacto, sem padding");

uint64_t nowMillis() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count()
    );
}

void syncToDisk(FILE* out) {
  std::fflush(out);
#ifdef _WIN32
  _commit(_fileno(out));
#else
  fsync(fileno(out));
#endif
}

// Corta o arquivo em `size` bytes, descartando um rabo corrompido.
bool truncateTo(FILE* out, long size) {
  std::fflush(out);
#ifdef _WIN32
  return _chsize_s(_fileno(out), size) == 0;
#else
  return ftruncate(fileno(out), size) == 0;
#endif
}

}  // namespace

SegmentWriter::SegmentWriter(const std::string& dataDir, const std::string& streamName) : streamName_(streamName) {
    const std::filesystem::path dir = std::filesystem::path(dataDir) / streamName;

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        spdlog::error("[segment/{}] falha ao criar \"{}\": {}", streamName_,
                    dir.string(), ec.message());
        return;
    }

    path_ = (dir / (streamName + "-00000.log")).string();
    if (!openSegment()) return;

    lastSync_ = std::chrono::steady_clock::now();
    thread_ = std::thread(&SegmentWriter::writerLoop, this);
}

SegmentWriter::~SegmentWriter() {
    stop();
}

bool SegmentWriter::openSegment() {
    file_ = std::fopen(path_.c_str(), "r+b");

    if (file_) {
        if (!recoverExisting()) {
            std::fclose(file_);
            file_ = nullptr;
            return false;
        }
        return true;
    }

    file_ = std::fopen(path_.c_str(), "w+b");
    if (!file_) {
        spdlog::error("[segment/{}] falha ao criar \"{}\"", streamName_, path_);
        return false;
    }

    SegmentHeader header{};
    std::memcpy(header.magic, kMagic, sizeof(kMagic));
    header.format_version = kFormatVersion;
    header.base_offset = 0;

    char bytes[sizeof(SegmentHeader)];
    std::memcpy(bytes, &header, sizeof(header));
    if (std::fwrite(bytes, 1, sizeof(bytes), file_) != sizeof(bytes)) {
        spdlog::error("[segment/{}] falha ao gravar header em \"{}\"", streamName_, path_);
        std::fclose(file_);
        file_ = nullptr;
        return false;
    }
    syncToDisk(file_);

    nextOffset_ = 0;
    spdlog::info("[segment/{}] segmento novo em \"{}\"", streamName_, path_);
    return true;
}

// Varredura de recuperacao - docs/formato_segmento.md secao 7. Percorre os
// registros do segmento validando cada um; ao encontrar o primeiro invalido
// (ou o fim do arquivo), corta ali. E' o que torna o offset confiavel entre
// reinicios depois de uma queda de energia no meio de uma escrita.
bool SegmentWriter::recoverExisting() {
    std::fseek(file_, 0, SEEK_SET);

    char headerBytes[sizeof(SegmentHeader)];
    if (std::fread(headerBytes, 1, sizeof(headerBytes), file_) != sizeof(headerBytes)) {
        spdlog::error("[segment/{}] \"{}\" menor que o header - nao e' um segmento", streamName_, path_);
        return false;
    }

    SegmentHeader header{};
    std::memcpy(&header, headerBytes, sizeof(header));

    if (std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0) {
        spdlog::error("[segment/{}] \"{}\" nao e' um segmento IoTrail (magic)", streamName_, path_);
        return false;
    }
    if (header.format_version != kFormatVersion) {
        spdlog::error("[segment/{}] \"{}\" tem format_version {}, esperado {}", streamName_, path_, header.format_version, kFormatVersion);
        return false;
    }

    long pos = static_cast<long>(sizeof(SegmentHeader));
    uint64_t nextOffset = header.base_offset;
    const char* motivo = nullptr;

    for (;;) {
        std::fseek(file_, pos, SEEK_SET);

        char recordBytes[sizeof(RecordHeader)];
        if (std::fread(recordBytes, 1, sizeof(recordBytes), file_) != sizeof(recordBytes)) {
            break;  // fim integro do arquivo (ou header parcial no rabo)
        }

        RecordHeader record{};
        std::memcpy(&record, recordBytes, sizeof(record));

        if (record.topic_len > kMaxTopicLen) {
            motivo = "topic_len acima do limite";
            break;
        }
        if (record.payload_len > kMaxPayloadLen) {
            motivo = "payload_len acima do limite";
            break;
        }
        if (record.offset != nextOffset) {
            motivo = "offset fora de sequencia";
            break;
        }

        const size_t bodyLen = static_cast<size_t>(record.topic_len) + record.payload_len;
        std::vector<char> body(bodyLen);
        if (bodyLen > 0 && std::fread(body.data(), 1, bodyLen, file_) != bodyLen) {
            motivo = "registro truncado";
            break;
        }

        uint32_t crc = crc32Update(0u, recordBytes + sizeof(record.crc32), sizeof(recordBytes) - sizeof(record.crc32));
        if (bodyLen > 0) crc = crc32Update(crc, body.data(), bodyLen);
        if (crc != record.crc32) {
            motivo = "crc32 nao confere";
            break;
        }

        pos += static_cast<long>(sizeof(RecordHeader) + bodyLen);
        ++nextOffset;
    }

    std::fseek(file_, 0, SEEK_END);
    const long fileSize = std::ftell(file_);

    if (pos < fileSize) {
        const long descartados = fileSize - pos;
        spdlog::warn("[segment/{}] rabo corrompido em \"{}\": {} ({} bytes descartados)",
            streamName_, path_, motivo ? motivo : "registro incompleto", descartados);
        if (!truncateTo(file_, pos)) {
            spdlog::error("[segment/{}] falha ao truncar \"{}\"", streamName_, path_);
            return false;
        }
    }

    std::fseek(file_, 0, SEEK_END);
    nextOffset_ = nextOffset;

    const uint64_t registros = nextOffset - header.base_offset;
    spdlog::info("[segment/{}] retomando \"{}\": {} registros validos, proximo offset {}",
        streamName_, path_, registros, nextOffset_);
    return true;
}

void SegmentWriter::push(std::string topic, std::string payload) {
    if (topic.size() > kMaxTopicLen) {
        spdlog::warn("[segment/{}] topico de {} bytes acima do limite ({}), mensagem descartada", streamName_, topic.size(), kMaxTopicLen);
        return;
    }
    if (payload.size() > kMaxPayloadLen) {
        spdlog::warn("[segment/{}] payload de {} bytes acima do limite ({}), mensagem descartada (topico \"{}\")",
                    streamName_, payload.size(), kMaxPayloadLen, topic);
        return;
    }

    std::lock_guard<std::mutex> lock(queueMutex_);
    queue_.push_back(QueuedRecord{nowMillis(), std::move(topic), std::move(payload)});
}

void SegmentWriter::stop() {
    if (!thread_.joinable()) return;  // ja parado, ou a abertura falhou no ctor

    stopRequested_.store(true);
    thread_.join();

    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
    }
}

void SegmentWriter::drainAndWrite() {
    std::vector<QueuedRecord> batch;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        while (!queue_.empty()) {
            batch.push_back(std::move(queue_.front()));
            queue_.pop_front();
        }
    }

    for (const auto& record : batch) {
        RecordHeader header{};
        header.crc32 = 0;  // preenchido depois, quando o registro estiver montado
        header.offset = nextOffset_;
        header.timestamp_ms = record.timestamp_ms;
        header.topic_len = static_cast<uint16_t>(record.topic.size());
        header.payload_len = static_cast<uint32_t>(record.payload.size());

        // Monta o registro inteiro num buffer e faz UM fwrite. Duas razoes: o CRC
        // precisa ser calculado antes de gravar (ele fica no inicio do registro),
        // e uma escrita unica cria menos fronteiras de escrita parcial - que e'
        // exatamente o que o CRC esta la pra detectar.
        std::vector<char> buffer(sizeof(RecordHeader) + record.topic.size() +
                                record.payload.size());
        size_t at = 0;
        std::memcpy(buffer.data() + at, &header, sizeof(header));
        at += sizeof(header);
        std::memcpy(buffer.data() + at, record.topic.data(), record.topic.size());
        at += record.topic.size();
        std::memcpy(buffer.data() + at, record.payload.data(), record.payload.size());

        // CRC cobre do byte 4 ate o fim do registro (spec secao 4).
        const uint32_t crc = crc32(buffer.data() + sizeof(header.crc32), buffer.size() - sizeof(header.crc32));
        std::memcpy(buffer.data(), &crc, sizeof(crc));

        if (std::fwrite(buffer.data(), 1, buffer.size(), file_) != buffer.size()) {
        spdlog::error("[segment/{}] escrita falhou no offset {}", streamName_,
                        nextOffset_);
        return;  // nao incrementa o offset: o registro nao entrou
        }
        ++nextOffset_;
    }
}

void SegmentWriter::maybeSync() {
    const auto now = std::chrono::steady_clock::now();
    if (now - lastSync_ >= kSyncInterval) {
        syncToDisk(file_);
        lastSync_ = now;
    }
}

void SegmentWriter::writerLoop() {
    while (!stopRequested_.load()) {
        std::this_thread::sleep_for(kWriteInterval);
        drainAndWrite();
        maybeSync();
    }

    // Sync final: drena o que chegou entre a ultima iteracao e o stop(), e
    // garante que o ultimo lote nao fique so na page cache.
    drainAndWrite();
    syncToDisk(file_);
    spdlog::info("[segment/{}] sync final executado, proximo offset {}", streamName_, nextOffset_);
}

// Fase 0 do roadmap: protótipo de escrita sequencial em arquivo.
// Layout do registro (endianness nativa, header compacto seguido dos blobs):
//   [8 bytes]  timestamp_ms  (uint64_t, epoch em milissegundos)
//   [2 bytes]  topic_len     (uint16_t)
//   [4 bytes]  payload_len   (uint32_t)
//   [N bytes]  topic
//   [M bytes]  payload
//
// Escrita direta, sem fila/batch/flush/fsync explícitos — o objetivo aqui é só
// validar o formato do registro, não a estratégia de persistência (fase 4).

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct FakeMqttMessage {
    std::string topic;
    std::string payload;
};

#pragma pack(push, 1)
struct RecordHeader {
    uint64_t timestamp_ms;
    uint16_t topic_len;
    uint32_t payload_len;
};
#pragma pack(pop)

static_assert(sizeof(RecordHeader) == 14, "RecordHeader precisa ser compacto, sem padding");

uint64_t nowMillis() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

void writeRecord(std::ofstream& out, const FakeMqttMessage& msg) {
    RecordHeader header{};
    header.timestamp_ms = nowMillis();
    header.topic_len = static_cast<uint16_t>(msg.topic.size());
    header.payload_len = static_cast<uint32_t>(msg.payload.size());
    char headerBytes[sizeof(RecordHeader)];
    std::memcpy(headerBytes, &header, sizeof(header));

    out.write(headerBytes, sizeof(headerBytes));
    out.write(msg.topic.data(), header.topic_len);
    out.write(msg.payload.data(), header.payload_len);
}

}  // namespace

int main() {
    const std::vector<FakeMqttMessage> messages = {
        {"sensores/temp1", "23.5"},
        {"sensores/temp2", "24.1"},
        {"sensores/temp1", "23.6"},
        {"sensores/umid1", "61"},
        {"sensores/temp3", "22.9"},
        {"sensores/umid1", "60"},
        {"sensores/temp2", "24.3"},
        {"sensores/temp1", "23.7"},
    };

    const std::filesystem::path outDir = ".";
    const std::filesystem::path outFile = outDir / "test_segment.bin";
    std::filesystem::create_directories(outDir);

    std::ofstream out(outFile, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "Falha ao abrir " << outFile << " para escrita\n";
        return 1;
    }

    std::streamoff totalBytes = 0;
    for (const auto& msg : messages) {
        const std::streamoff before = out.tellp();
        writeRecord(out, msg);
        totalBytes += out.tellp() - before;
    }
    // sem flush()/close() explícito e sem fsync: o fechamento via RAII no fim
    // do escopo entrega o buffer ao SO, mas não força gravação em disco físico.

    std::cout << messages.size() << " mensagens gravadas em " << outFile.string()
              << " (" << totalBytes << " bytes)\n";

    return 0;
}

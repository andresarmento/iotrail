// Fase 0 do roadmap, evolução do test_0: agora existe uma estrutura em memória
// (fila) alimentada por uma thread produtora (simula o cliente MQTT recebendo
// 1 mensagem/s) e drenada por uma thread escritora que acorda a cada
// write_interval, pega tudo que estiver disponível e grava no arquivo.
// Ainda sem sync_interval/fsync — isso continua para a Fase 4.
//
// Layout do registro (igual ao test_0, little-endian, header compacto):
//   [8 bytes]  timestamp_ms  (uint64_t)
//   [2 bytes]  topic_len     (uint16_t)
//   [4 bytes]  payload_len   (uint32_t)
//   [N bytes]  topic
//   [M bytes]  payload

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr auto kProduceInterval = std::chrono::milliseconds(600);
constexpr auto kWriteInterval = std::chrono::milliseconds(1000);

struct FakeMqttMessage {
    std::string topic;
    std::string payload;
};

// Registro já com timestamp atribuído no momento em que "chegou" (produção),
// não no momento em que for gravado — são instantes diferentes agora que
// produção e escrita rodam em ritmos próprios.
struct QueuedRecord {
    uint64_t timestamp_ms;
    std::string topic;
    std::string payload;
};

// Layout do cabeçalho de cada registro gravado
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

// Grava o registro no arquivo
void writeRecord(std::ofstream& out, const QueuedRecord& record) {
    RecordHeader header{};
    header.timestamp_ms = record.timestamp_ms;
    header.topic_len = static_cast<uint16_t>(record.topic.size());
    header.payload_len = static_cast<uint32_t>(record.payload.size());

    char headerBytes[sizeof(RecordHeader)];
    std::memcpy(headerBytes, &header, sizeof(header));

    out.write(headerBytes, sizeof(headerBytes));
    out.write(record.topic.data(), header.topic_len);
    out.write(record.payload.data(), header.payload_len);
}

// Thread produtora: simula o cliente MQTT recebendo 1 mensagem por segundo e
// empurrando pra fila em memória compartilhada.
void producerLoop(const std::vector<FakeMqttMessage>& messages,
                   std::deque<QueuedRecord>& queue,
                   std::mutex& queueMutex,
                   std::atomic<bool>& producerDone) {
    for (const auto& msg : messages) {
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            queue.push_back(QueuedRecord{nowMillis(), msg.topic, msg.payload});
        }
        std::cout << "[produtor] enfileirou " << msg.topic << "=" << msg.payload << "\n";
        std::this_thread::sleep_for(kProduceInterval);
    }
    producerDone.store(true);
}

// Thread escritora: acorda a cada write_interval, drena tudo que a produtora
// já colocou na fila e grava no arquivo. Termina quando a produtora sinalizar
// que acabou e a fila estiver vazia.
void writerLoop(std::ofstream& out,
                 std::deque<QueuedRecord>& queue,
                 std::mutex& queueMutex,
                 const std::atomic<bool>& producerDone) {
    std::vector<QueuedRecord> batch;

    for (;;) {
        std::this_thread::sleep_for(kWriteInterval);

        batch.clear();
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            while (!queue.empty()) {
                batch.push_back(std::move(queue.front()));
                queue.pop_front();
            }
        }

        for (const auto& record : batch) {
            writeRecord(out, record);
            std::cout << "[consumidor] gravou " << record.topic << "=" << record.payload << "\n";
        }

        if (producerDone.load() && batch.empty()) {
            break;
        }
    }
}

} // namespace

int main() {
    // Array de mensagens, apenas para teste, será removido no futuro
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

    // Cria o arquivo de saída
    const std::filesystem::path outFile = "test_segment.bin";
    std::ofstream out(outFile, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "Falha ao abrir " << outFile << " para escrita\n";
        return 1;
    }

    // Cria a fila com STL
    std::deque<QueuedRecord> queue;
    // Mutex para controle de exclusão mútua para a fila
    std::mutex queueMutex;
    // 
    std::atomic<bool> producerDone{false};

    // Cria as threads de teste: producer e writer
    // producer: simulador client MQTT que alimenta a fila
    // writer  : lê a fila e grava no arquivo
    std::thread producer(producerLoop, std::cref(messages), std::ref(queue),
                          std::ref(queueMutex), std::ref(producerDone));
    std::thread writer(writerLoop, std::ref(out), std::ref(queue),
                        std::ref(queueMutex), std::cref(producerDone));

    producer.join();
    writer.join();

    std::cout << messages.size() << " mensagens processadas, gravadas em "
              << outFile.string() << "\n";

    return 0;
}

// Fase 0 do roadmap, evolução do test_1: além do write_interval (que já
// batchava e gravava no arquivo), agora existe um sync_interval independente
// que força a gravação física em disco (fsync/_commit) periodicamente,
// limitando a janela de dados que poderiam ser perdidos numa queda de
// energia. Ao encerrar, um sync final garante que o último lote gravado não
// fique só na page cache.
//
// Layout do registro (igual ao test_0/test_1, little-endian, header compacto):
//   [8 bytes]  timestamp_ms  (uint64_t)
//   [2 bytes]  topic_len     (uint16_t)
//   [4 bytes]  payload_len   (uint32_t)
//   [N bytes]  topic
//   [M bytes]  payload

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

constexpr auto kProduceInterval = std::chrono::milliseconds(600);
constexpr auto kWriteInterval = std::chrono::milliseconds(300);
constexpr auto kSyncInterval = std::chrono::milliseconds(1000);

struct FakeMqttMessage {
    std::string topic;
    std::string payload;
};

// Registro já com timestamp atribuído no momento em que "chegou" (produção),
// não no momento em que for gravado.
struct QueuedRecord {
    uint64_t timestamp_ms;
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

void writeRecord(FILE* out, const QueuedRecord& record) {
    RecordHeader header{};
    header.timestamp_ms = record.timestamp_ms;
    header.topic_len = static_cast<uint16_t>(record.topic.size());
    header.payload_len = static_cast<uint32_t>(record.payload.size());

    char headerBytes[sizeof(RecordHeader)];
    std::memcpy(headerBytes, &header, sizeof(header));

    std::fwrite(headerBytes, 1, sizeof(headerBytes), out);
    std::fwrite(record.topic.data(), 1, header.topic_len, out);
    std::fwrite(record.payload.data(), 1, header.payload_len, out);
}

// Força o SO a persistir fisicamente em disco o que já foi escrito (que até
// aqui pode estar só na page cache). fflush() esvazia o buffer da libc pro
// SO; _commit/fsync é que de fato manda o SO gravar em disco.
void syncToDisk(FILE* out) {
    std::fflush(out);
#ifdef _WIN32
    _commit(_fileno(out));
#else
    fsync(fileno(out));
#endif
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

// Thread escritora: acorda a cada write_interval, drena a fila e grava no
// arquivo (page cache). Independentemente disso, a cada sync_interval força
// fsync/_commit pra garantir durabilidade. Os dois timers são checados na
// mesma thread/loop (sem thread dedicada de sync) pra evitar acesso
// concorrente ao mesmo FILE* sem sincronização extra. Termina quando a
// produtora sinalizar que acabou e a fila estiver vazia.
void writerLoop(FILE* out,
                 std::deque<QueuedRecord>& queue,
                 std::mutex& queueMutex,
                 const std::atomic<bool>& producerDone) {
    std::vector<QueuedRecord> batch;
    auto lastSync = std::chrono::steady_clock::now();

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

        const auto now = std::chrono::steady_clock::now();
        if (now - lastSync >= kSyncInterval) {
            syncToDisk(out);
            lastSync = now;
            std::cout << "[consumidor] sync (fsync) executado\n";
        }

        if (producerDone.load() && batch.empty()) {
            break;
        }
    }

    // Sync final: garante que o último lote gravado não fique só na page
    // cache caso o loop termine antes do próximo sync_interval vencer.
    syncToDisk(out);
    std::cout << "[consumidor] sync final executado\n";
}

}  // namespace

int main() {
    // Array de mensagens, para teste
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

    // Cria o arquivo de saída. FILE* em vez de std::ofstream porque
    // syncToDisk() precisa do descritor nativo (via _fileno/fileno) pra
    // chamar _commit/fsync — std::ofstream não expõe isso de forma portável.
    const std::filesystem::path outFile = "test_segment.bin";
    FILE* out = std::fopen(outFile.string().c_str(), "wb");
    if (!out) {
        std::cerr << "Falha ao abrir " << outFile << " para escrita\n";
        return 1;
    }

    // Cria a fila com STL
    std::deque<QueuedRecord> queue;
    // Mutex que protege o acesso concorrente à fila (produtora x escritora)
    std::mutex queueMutex;
    // Flag atômica: sinaliza da produtora pra escritora que não vem mais mensagem
    std::atomic<bool> producerDone{false};

    // Cria as threads de teste: producer e writer
    // producer: simulador client MQTT que alimenta a fila
    // writer  : lê a fila, grava no arquivo e sincroniza com disco periodicamente
    std::thread producer(producerLoop, std::cref(messages), std::ref(queue),
                          std::ref(queueMutex), std::ref(producerDone));
    std::thread writer(writerLoop, out, std::ref(queue),
                        std::ref(queueMutex), std::cref(producerDone));

    producer.join();
    writer.join();

    std::fclose(out);

    std::cout << messages.size() << " mensagens processadas, gravadas em "
              << outFile.string() << "\n";

    return 0;
}

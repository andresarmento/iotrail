/*
 *  IoTrail - sistema leve de persistencia e streaming de eventos para IoT.
 *  Copyright Andre Sarmento - 2026
 *
 *  Metadados da stream: o dicionario topico -> id (docs/FORMATO.md secao 5)
 */
#include "meta.h"

#include <cerrno>
#include <cstring>
#include <unordered_set>
#include <vector>

#include "crc32.h"
#include "format.h"
#include "logging.h"

namespace fs = std::filesystem;

namespace {

#pragma pack(push, 1)
    struct meta_header {
        char magic[4];
        uint16_t format_version;
        uint16_t header_len;
        uint32_t next_topic_id;
    };

    struct entry_head {
        uint32_t crc32;
        uint32_t topic_id;
        uint16_t t_len;
    };
#pragma pack(pop)

    // O pack so mata o padding DENTRO da struct; o campo cair em endereco impar
    // dentro do buffer quem resolve e' o memcpy da struct inteira, que e' como
    // toda leitura e escrita daqui trafega (decidido na 3.2).
    static_assert(sizeof(meta_header) == 12, "header do .meta tem 12 bytes (FORMATO.md secao 5)");
    static_assert(sizeof(entry_head) == 10, "parte fixa da entrada tem 10 bytes");

    constexpr uint16_t meta_header_len = sizeof(meta_header);
    constexpr size_t entry_head_len = sizeof(entry_head);

    // O CRC da entrada cobre do byte 4 ate o fim do topico.
    constexpr size_t entry_crc_skip = sizeof(uint32_t);

    const char* why() { return std::strerror(errno); }

    bool has_segments(const fs::path& dir) {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (e.is_regular_file(ec) && e.path().extension() == ".log") {
                return true;
            }
        }
        return false;
    }
}

namespace storage {

    void meta::close() {
        file_.close();
        by_topic_.clear();
        next_topic_id_ = 0;
        end_ = 0;
        failed_ = false;
    }

    bool meta::open(const fs::path& data_dir, const std::string& stream) {
        close();
        stream_ = stream;

        const fs::path dir = data_dir / stream;
        const fs::path path = dir / (stream + ".meta");

        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) {
            logging::error("[meta/{}] nao criou \"{}\": {}", stream_, dir.string(), ec.message());
            return false;
        }

        // .meta ausente COM segmentos presentes nao e' stream nova: e' arquivo
        // apagado a mao ou perdido. Recriar vazio faria o proximo topico
        // receber id 0, 1, 2... que ja sao de OUTROS topicos nos segmentos
        // velhos, e um registro do mes passado passaria a resolver pro nome
        // errado, em silencio - exatamente o que a regra de nunca reusar id
        // existe pra impedir. Descobrir o id certo exigiria varrer todos os
        // segmentos da stream, e a secao 8 so varre o ultimo. Entao para aqui.
        if (!fs::exists(path, ec) && has_segments(dir)) {
            logging::error("[meta/{}] \"{}\" nao existe mas ha segmentos na pasta - os topic_id "
                           "dos registros ficariam orfaos e ids novos colidiriam com os antigos. "
                           "Esta stream nao grava nesta execucao; restaure o arquivo ou mova a "
                           "pasta",
                           stream_, path.string());
            return false;
        }

        if (!file_.open(path)) {
            logging::error("[meta/{}] nao abriu \"{}\": {}", stream_, path.string(), why());
            return false;
        }

        const int64_t size = file_.size();
        if (size < 0) {
            logging::error("[meta/{}] nao leu o tamanho de \"{}\": {}", stream_, path.string(),
                           why());
            file_.close();
            return false;
        }

        if (size == 0) {
            if (!write_header(0) || !file_.sync()) {
                logging::error("[meta/{}] nao gravou o header de \"{}\": {}", stream_,
                               path.string(), why());
                file_.close();
                return false;
            }
            end_ = meta_header_len;
            logging::debug("[meta/{}] criado", stream_);
            return true;
        }

        if (!load()) {
            file_.close();
            return false;
        }

        logging::info("[meta/{}] {} topicos conhecidos, proximo id {}", stream_, by_topic_.size(),
                      next_topic_id_);
        return true;
    }

    bool meta::write_header(uint32_t next_id) {
        meta_header h{};
        std::memcpy(h.magic, meta_magic, sizeof h.magic);
        h.format_version = format_version;
        h.header_len = meta_header_len;
        h.next_topic_id = next_id;

        unsigned char buf[meta_header_len];
        std::memcpy(buf, &h, sizeof h);
        return file_.seek(0) && file_.write(buf, sizeof buf);
    }

    bool meta::load() {
        const int64_t size = file_.size();

        unsigned char head[meta_header_len];
        if (size < static_cast<int64_t>(meta_header_len) || !file_.seek(0) ||
            !file_.read(head, sizeof head)) {
            logging::error("[meta/{}] header incompleto ou ilegivel", stream_);
            return false;
        }

        meta_header h{};
        std::memcpy(&h, head, sizeof h);

        if (std::memcmp(h.magic, meta_magic, sizeof h.magic) != 0) {
            logging::error("[meta/{}] magic errado - o arquivo nao e' um .meta do IoTrail",
                           stream_);
            return false;
        }
        if (h.format_version != format_version) {
            logging::error("[meta/{}] format_version {} desconhecida (este binario le {})",
                           stream_, h.format_version, format_version);
            return false;
        }
        if (h.header_len != meta_header_len) {
            logging::error("[meta/{}] header_len {} invalido na v1 (esperado {})", stream_,
                           h.header_len, meta_header_len);
            return false;
        }

        next_topic_id_ = h.next_topic_id;

        // Buffer unico pra entrada inteira: o CRC cobre uma faixa contigua e a
        // struct empacotada nao pode ter o endereco de um campo tomado.
        std::vector<unsigned char> buf(entry_head_len + max_topic_len);
        std::unordered_set<uint32_t> seen_ids;

        int64_t pos = meta_header_len;
        if (!file_.seek(pos)) {
            logging::error("[meta/{}] nao posicionou no inicio da tabela: {}", stream_, why());
            return false;
        }

        while (true) {
            if (pos + static_cast<int64_t>(entry_head_len) > size) {
                break;  // falha de tamanho: nao cabe nem a parte fixa
            }
            if (!file_.read(buf.data(), entry_head_len)) {
                break;
            }

            entry_head e{};
            std::memcpy(&e, buf.data(), sizeof e);

            // t_len == 0 e' corrupcao: MQTT 3.1.1 exige topico com pelo menos um
            // caractere, entao zero no disco nao pode ter vindo de um broker.
            if (e.t_len == 0 || e.t_len > max_topic_len) {
                logging::warn("[meta/{}] entrada em {} com t_len {} invalido", stream_, pos,
                              e.t_len);
                break;
            }
            if (pos + static_cast<int64_t>(entry_head_len + e.t_len) > size) {
                break;  // falha de tamanho: o topico nao cabe no que resta
            }
            if (!file_.read(buf.data() + entry_head_len, e.t_len)) {
                break;
            }

            const uint32_t crc =
                crc32(buf.data() + entry_crc_skip, entry_head_len - entry_crc_skip + e.t_len);
            if (crc != e.crc32) {
                logging::warn("[meta/{}] entrada em {} com CRC ruim", stream_, pos);
                break;
            }

            std::string topic(reinterpret_cast<const char*>(buf.data() + entry_head_len), e.t_len);

            // Duplicata nao deveria existir num arquivo append-only, mas a
            // ferramenta de limpeza reescreve o arquivo inteiro. Vence a
            // primeira: e' contra ela que os registros mais antigos foram
            // gravados. Nao para a stream - nao afeta a leitura dos dados.
            if (!seen_ids.insert(e.topic_id).second) {
                logging::warn("[meta/{}] topic_id {} repetido em {} - vale a primeira entrada",
                              stream_, e.topic_id, pos);
            } else if (!by_topic_.emplace(topic, e.topic_id).second) {
                logging::warn("[meta/{}] topico \"{}\" repetido em {} - vale a primeira entrada",
                              stream_, topic, pos);
            }

            // max(header, maior id + 1): nenhum dos dois sozinho basta. O header
            // segura o valor quando uma limpeza removeu os ids do topo; as
            // entradas puxam pra cima quando o header ficou atrasado por uma
            // queda entre gravar a entrada e atualizar o header.
            const uint64_t after = static_cast<uint64_t>(e.topic_id) + 1;
            if (after > next_topic_id_ && after <= UINT32_MAX) {
                next_topic_id_ = static_cast<uint32_t>(after);
            }

            pos += static_cast<int64_t>(entry_head_len + e.t_len);
        }

        end_ = pos;

        // Aqui trunca nos DOIS tipos de falha, ao contrario do segmento (secao
        // 8, que preserva como .corrupt quando o conteudo e' que esta ruim). O
        // que torna seguro e' a regra de nunca reusar id: um topico cuja
        // entrada se perdeu volta a ser topico novo e recebe id NOVO, e os
        // registros que usavam o id antigo caem no caso "topic_id desconhecido"
        // da secao 4 - mantidos, so com o topico irresolvivel. A entrada
        // cortada tambem nunca teve registro apontando pra ela: o id so e'
        // devolvido depois do sync.
        if (pos < size) {
            logging::warn("[meta/{}] {} bytes ilegiveis no fim - truncando em {}", stream_,
                          size - pos, pos);
            if (!file_.truncate(pos) || !file_.sync()) {
                logging::error("[meta/{}] nao truncou: {}", stream_, why());
                return false;
            }
        }

        // O header nao e' reescrito quando o next_topic_id em memoria ficou
        // acima do gravado: o max() acima chega no mesmo valor em todo boot, e
        // a proxima insercao grava o header de qualquer forma.
        return true;
    }

    std::optional<uint32_t> meta::id_for(const std::string& topic) {
        if (failed_) {
            return std::nullopt;
        }

        const auto it = by_topic_.find(topic);
        if (it != by_topic_.end()) {
            return it->second;
        }

        if (topic.empty() || topic.size() > max_topic_len) {
            logging::error("[meta/{}] topico com {} bytes fora da faixa 1..{}", stream_,
                           topic.size(), max_topic_len);
            failed_ = true;
            return std::nullopt;
        }
        if (next_topic_id_ == UINT32_MAX) {
            logging::error("[meta/{}] ids de topico esgotados", stream_);
            failed_ = true;
            return std::nullopt;
        }

        const uint32_t id = next_topic_id_;
        const auto t_len = static_cast<uint16_t>(topic.size());

        entry_head e{};
        e.topic_id = id;
        e.t_len = t_len;

        std::vector<unsigned char> buf(entry_head_len + t_len);
        std::memcpy(buf.data(), &e, sizeof e);
        std::memcpy(buf.data() + entry_head_len, topic.data(), t_len);

        const uint32_t crc = crc32(buf.data() + entry_crc_skip, buf.size() - entry_crc_skip);
        std::memcpy(buf.data(), &crc, sizeof crc);

        // Duas escritas e UM sync. Toda combinacao de queda e' segura por causa
        // do max() da leitura: se so a entrada sobreviver, ela mesma puxa o
        // next_topic_id pra cima; se so o header sobreviver, sobra um buraco na
        // numeracao, que ja e' esperado. O que nao pode acontecer - registro
        // apontando pra id que nao existe - esta coberto porque o id so sai
        // daqui depois do sync.
        if (!file_.seek(end_) || !file_.write(buf.data(), buf.size()) || !write_header(id + 1) ||
            !file_.sync()) {
            logging::error("[meta/{}] nao gravou o topico \"{}\": {}", stream_, topic, why());
            failed_ = true;
            return std::nullopt;
        }

        end_ += static_cast<int64_t>(buf.size());
        next_topic_id_ = id + 1;
        by_topic_.emplace(topic, id);

        logging::debug("[meta/{}] topico novo \"{}\" -> id {}", stream_, topic, id);
        return id;
    }
}

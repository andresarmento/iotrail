#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// CRC-32 IEEE 802.3 - o mesmo do zlib e do gzip. Usado pra detectar registro
// corrompido na varredura de recuperacao do boot (ver docs/formato_segmento.md,
// secoes 4 e 7).
//
// Parametros: polinomio refletido 0xEDB88320, valor inicial 0xFFFFFFFF,
// resultado invertido no fim. Sao os mesmos do zlib.crc32() do Python, de
// proposito - o leitor de referencia em Python precisa produzir exatamente
// este valor sem reimplementar nada.
//
// Header-only e sem dependencia externa: o zlib e' dependencia de *build* do
// pacote mosquitto do MSYS2, mas nao entra no link do cliente (ver
// claude_memory/CLAUDE.md, secao "Prototipo test_3") - nao vale puxar uma lib
// pra isso.

namespace crc32_detail {

// Tabela de 256 entradas gerada em tempo de compilacao. Sendo constexpr, nao
// ha custo de inicializacao em runtime nem necessidade de um .cpp separado.
constexpr std::array<uint32_t, 256> makeTable() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int bit = 0; bit < 8; ++bit) {
        c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        table[i] = c;
    }
    return table;
}

inline constexpr std::array<uint32_t, 256> kTable = makeTable();

}  // namespace crc32_detail

// Continua um CRC ja iniciado. Passar 0 no primeiro pedaco.
//
// Existe na forma incremental porque o CRC de um registro cobre uma faixa que
// esta em buffers separados na hora de gravar: os 22 bytes da parte fixa depois
// do campo crc32, mais o topico, mais o payload (ver docs/formato_segmento.md
// secao 4). Encadear tres chamadas evita ter que concatenar tudo antes.
//
// Mesma convencao do zlib: zlib.crc32(dados) equivale a crc32Update(0, ...) e
// zlib.crc32(dados, anterior) equivale a crc32Update(anterior, ...).
inline uint32_t crc32Update(uint32_t crc, const void* data, size_t len) {
    const auto* p = static_cast<const unsigned char*>(data);
    crc = ~crc;
    while (len-- > 0) {
        crc = crc32_detail::kTable[(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
    }
    return ~crc;
}

// CRC de um buffer contiguo unico.
inline uint32_t crc32(const void* data, size_t len) {
    return crc32Update(0u, data, len);
}

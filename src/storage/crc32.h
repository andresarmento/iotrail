/*
 *  IoTrail - sistema leve de persistencia e streaming de eventos para IoT.
 *  Copyright Andre Sarmento - 2026
 *
 *  CRC-32 IEEE 802.3 - o mesmo do zlib e do gzip
 */
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

// Polinomio refletido 0xEDB88320, valor inicial 0xFFFFFFFF, resultado invertido
// no fim. Sao os parametros do zlib.crc32() de proposito: o leitor de
// referencia em Python (docs/FORMATO.md secao 10) precisa chegar no mesmo valor
// sem reimplementar nada.
//
// Header-only e sem dependencia externa. A zlib e' dependencia de build do
// pacote mosquitto do MSYS2, mas nao entra no nosso link - nao vale puxar uma
// lib inteira por 20 linhas.

namespace storage {

    // Tabela gerada em tempo de compilacao: sem custo de inicializacao no boot e
    // sem .cpp separado so pra hospedar um array.
    constexpr std::array<uint32_t, 256> make_crc32_table() {
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

    inline constexpr std::array<uint32_t, 256> crc32_table = make_crc32_table();

    // Continua um CRC ja iniciado - 0 no primeiro pedaco. Mesma convencao do
    // zlib: crc32_update(0, d, n) == zlib.crc32(d), e crc32_update(anterior,
    // ...) == zlib.crc32(d, anterior).
    //
    // A forma incremental e' pra varredura de recuperacao (3.8): ela le a parte
    // fixa do registro primeiro, porque e' de la que sai o payload_len, e so
    // depois le o payload. Encadear as duas evita concatenar ate 1 MiB num
    // buffer novo so pra conferir o CRC.
    constexpr uint32_t crc32_update(uint32_t crc, const unsigned char* data, size_t len) {
        crc = ~crc;
        for (size_t i = 0; i < len; ++i) {
            crc = crc32_table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
        }
        return ~crc;
    }

    inline uint32_t crc32_update(uint32_t crc, const void* data, size_t len) {
        return crc32_update(crc, static_cast<const unsigned char*>(data), len);
    }

    // Buffer contiguo unico - o caso do writer, que monta o registro inteiro na
    // memoria antes de gravar (o CRC fica no byte 0, entao tem que ser
    // calculado antes da escrita).
    inline uint32_t crc32(const void* data, size_t len) {
        return crc32_update(0u, data, len);
    }

    // Valor de conferencia canonico do CRC-32/ISO-HDLC: crc de "123456789" e'
    // 0xCBF43926. Sendo constexpr, isto e' o teste da 3.2 - roda no compilador,
    // em todo build, sem framework e sem executavel de teste. Errar um parametro
    // (polinomio, inicial, inversao) reprova aqui. O array existe porque
    // string literal e' const char* e o cast pra unsigned char nao vale em
    // expressao constante.
    constexpr unsigned char crc32_check_input[9] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    static_assert(crc32_update(0u, crc32_check_input, 9) == 0xCBF43926u,
                  "CRC-32 nao bate com o valor de conferencia do zlib");
}

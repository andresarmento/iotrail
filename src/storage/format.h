/*
 *  IoTrail - sistema leve de persistencia e streaming de eventos para IoT.
 *  Copyright Andre Sarmento - 2026
 *
 *  Constantes do formato em disco - ver docs/FORMATO.md
 */
#pragma once
#include <cstdint>

// docs/FORMATO.md secao 1 declara little-endian para todos os campos inteiros.
// Quem implementa essa declaracao seria uma camada de put/get por deslocamento;
// a 3.2 decidiu memcpy de struct empacotada no lugar dela, e memcpy escreve na
// ordem do host. Em x86 e ARM as duas coisas coincidem e nao ha conversao
// nenhuma - que e' o motivo da escolha.
//
// Num host big-endian nao coincidem, e o efeito seria segmento gravado em BE
// com o header dizendo que esta em LE: ilegivel para qualquer outro leitor, sem
// nenhum aviso. Entao o build para aqui. O conserto, no dia em que isso
// acontecer, e' trocar os memcpy por put/get de byte - meia hora, e localizada.
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "IoTrail assume host little-endian - ver docs/FORMATO.md secao 1"
#endif

namespace storage {

    // Um so para os dois arquivos: sao um formato em duas partes (FORMATO.md
    // secao 5). O magic diferente existe so pro arquivo se identificar sozinho.
    inline constexpr uint16_t format_version = 1;

    inline constexpr char segment_magic[4] = {'I', 'O', 'T', 'R'};
    inline constexpr char meta_magic[4]    = {'I', 'O', 'T', 'M'};

    // Limites DO FORMATO (FORMATO.md secao 6), nao do writer. Servem para
    // validar t_len e payload_len na varredura de recuperacao, antes que o CRC
    // possa valida-los - por isso sao constantes e mudam so com bump de
    // format_version. Os limites de admissao (max_payload_len,
    // segment_max_bytes) sao config, e nao moram aqui de proposito: misturar os
    // dois papeis num valor configuravel abre caminho de perda de dados.
    inline constexpr uint16_t max_topic_len    = 1024;
    inline constexpr uint32_t payload_hard_max = 1024 * 1024;
}

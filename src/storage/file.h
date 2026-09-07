/*
 *  IoTrail - sistema leve de persistencia e streaming de eventos para IoT.
 *  Copyright Andre Sarmento - 2026
 *
 *  Camada de plataforma: descritor de arquivo com sync e truncate
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>

// Todo o #ifdef de arquivo do projeto mora aqui (docs/TODO.md 3.3). Do que os
// consumidores pedem - abrir, ler, escrever, sincronizar, truncar, tamanho,
// renomear, criar diretorio - so os quatro primeiros precisam de ramo por
// plataforma; rename, exists e create_directories saem de std::filesystem sem
// #ifdef nenhum e nao entram nesta classe.

namespace storage {

    // Descritor cru, nao FILE*. O stdio poria um buffer proprio na frente do
    // page cache e durabilidade viraria fflush() seguido de sync() - esquecer o
    // primeiro nao da erro nem aviso, so aparece como perda de dados depois de
    // uma queda de energia. E o buffer nao compraria nada aqui: o writer ja
    // monta o registro inteiro na memoria antes de gravar, porque o CRC fica no
    // byte 0 e precisa existir antes da escrita.
    //
    // Todo metodo devolve false em falha e deixa o motivo em errno - vale nos
    // dois ramos, o CRT do Windows tambem preenche errno. Quem loga e' o
    // chamador, que sabe de qual stream e de qual arquivo se trata.
    class file {
    public:
        file() = default;
        ~file();

        file(file&& other) noexcept;
        file& operator=(file&& other) noexcept;
        file(const file&) = delete;
        file& operator=(const file&) = delete;

        // Leitura e escrita, criando se nao existir, posicao no byte 0. Quando
        // o arquivo e' criado de fato, sincroniza tambem o diretorio pai.
        bool open(const std::filesystem::path& path);
        bool is_open() const { return fd_ >= 0; }
        void close();

        int64_t size() const;                      // -1 em falha
        bool seek(int64_t pos);
        bool read(void* buf, size_t len);          // exatamente len bytes
        bool write(const void* data, size_t len);  // exatamente len bytes
        bool sync();
        bool truncate(int64_t len);

    private:
        int fd_ = -1;
    };

    // fsync do diretorio: e' o que persiste a ENTRADA do arquivo, que e' outra
    // coisa que o conteudo dele. Sem isto, uma queda logo depois de um rollover
    // pode voltar sem o segmento novo, mesmo com os bytes ja sincronizados.
    // No Windows e' no-op - o NTFS journala metadado sozinho.
    bool sync_dir(const std::filesystem::path& dir);
}

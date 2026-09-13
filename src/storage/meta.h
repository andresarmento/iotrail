/*
 *  IoTrail - sistema leve de persistencia e streaming de eventos para IoT.
 *  Copyright Andre Sarmento - 2026
 *
 *  Metadados da stream: o dicionario topico -> id (docs/FORMATO.md secao 5)
 */
#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

#include "file.h"

namespace storage {

    // Um arquivo <stream>.meta por stream, ao lado dos segmentos. O registro
    // guarda topic_id em vez do topico (decidido na 3.1: o topico era 37% do
    // registro e se repetia em toda mensagem), e e' esta tabela que da sentido
    // ao id.
    //
    // SEM LOCK, de proposito. Cada stream pertence a exatamente uma thread: a
    // config amarra a stream a um unico broker (config.h:29) e o main entrega
    // cada stream ao cliente daquele broker (main.cpp:95-96), entao a tabela
    // tem dono unico - a thread da lib mosquitto. Nao e' o argumento da 2.5 (la
    // a estrutura e' imutavel); aqui ela muda, so nao ha segundo escritor. Se a
    // Fase 6/7 puser um leitor em outra thread, e' ali que a decisao se reabre.
    class meta {
      public:
        meta() = default;

        // Abre <data_dir>/<stream>/<stream>.meta, criando pasta e arquivo se
        // preciso, e recupera a tabela. Chamado no boot para TODA stream da
        // config: num edge desatendido, descobrir no boot que o disco nao deixa
        // escrever vale mais que descobrir as 3h quando a primeira mensagem
        // chega, e deixa a varredura sem ramo "existe ou nao".
        //
        // false = esta stream nao grava nesta execucao; o motivo ja foi logado.
        bool open(const std::filesystem::path& data_dir, const std::string& stream);

        bool is_open() const { return file_.is_open(); }
        void close();

        // Resolve, inserindo se for a primeira vez. A insercao grava a entrada
        // E SINCRONIZA antes de devolver o id (FORMATO.md secao 5): na ordem
        // inversa, uma queda entre as duas escritas deixaria registro apontando
        // pra id inexistente. E' o unico fsync fora do sync_interval, e so
        // acontece na primeira vez que cada topico aparece - em regime, nunca.
        //
        // nullopt = a stream tem que parar. O motivo ja foi logado.
        std::optional<uint32_t> id_for(const std::string& topic);

        size_t topic_count() const { return by_topic_.size(); }
        uint32_t next_topic_id() const { return next_topic_id_; }

      private:
        bool write_header(uint32_t next_id);
        bool load();

        file file_;
        std::string stream_;

        // So a direcao que o caminho de recebimento usa. O id -> topico e' do
        // leitor da Fase 6/7 e nao tem consumidor nenhum agora; quando tiver,
        // e' um unordered_map<uint32_t, const std::string*> apontando pras
        // chaves deste aqui - referencia a elemento de unordered_map sobrevive
        // a rehash, entao nao ha segunda copia do texto pra divergir.
        std::unordered_map<std::string, uint32_t> by_topic_;

        uint32_t next_topic_id_ = 0;
        int64_t end_ = 0;  // fim da ultima entrada integra, onde a proxima entra

        // Falha de escrita e' grudenta: depois dela nada mais e' resolvido. A
        // politica de parar a stream e' da 3.7, mas deixar o chamador seguir
        // mancando dependeria de ele lembrar de checar.
        bool failed_ = false;
    };
}

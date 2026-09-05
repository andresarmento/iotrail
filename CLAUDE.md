# IoTrail

Sistema leve de persistência e streaming de eventos para IoT, em C++17. Conecta-se
como cliente a brokers MQTT existentes, subscreve tópicos e grava as mensagens em
log append-only (segmentos + índices próprios), sem banco de dados externo.
Inspirado no Kafka (offset por consumidor, replay, retenção), mas para redes IoT
pequenas/médias e edge — baixo consumo de RAM, CPU e disco.

## Regra 1 — knowledge_base/ é imutável

`knowledge_base/` contém o projeto anterior (código, docs, decisões, protótipos).
É **somente leitura**: fonte de consulta, nunca destino de edição. Nada dentro dela
pode ser criado, alterado, movido ou apagado. O software novo é escrito do zero
fora dela, na raiz do projeto.

Consultar antes de decidir. Pontos de entrada:
- `knowledge_base/claude_memory/CLAUDE.md` — memória do projeto anterior, decisões
- `knowledge_base/docs/motivacao.txt` — por que o projeto existe
- `knowledge_base/docs/formato_segmento.md` — layout do registro em disco
- `knowledge_base/docs/decisao_sync_write.txt` — write_interval vs sync_interval
- `knowledge_base/docs/DONE.txt` — o que os protótipos test_0..test_3 provaram
- `knowledge_base/docs/ROADMAP.txt`, `IDEIAS*.txt` — direção pós-MVP
- `knowledge_base/src/`, `knowledge_base/iotrail_refactory/src/` — código anterior

## Estrutura nova

```
docs/          DESIGN.md, ROADMAP.md, TODO.md   (documentação viva do projeto novo)
src/           código novo
tools/         binários mosquitto para teste local (mosquitto_pub/sub)
.vscode/       IntelliSense apontado para o compile_commands.json do build
iotrail.conf   configuração-fonte, copiada para o lado do .exe no build
build/         gerado, fora do git
```

## Ambiente

- Windows, toolchain MSYS2 ucrt64 (`C:/msys64/ucrt64`)
- CMake ≥ 3.21 (por `TARGET_RUNTIME_DLLS`) + Ninja,
  `CMAKE_EXPORT_COMPILE_COMMANDS ON`
- C++17 + STL; mosquitto (MQTT 3.1.1) e spdlog vindos do MSYS2
- **Runtime C++ dinâmico**, spdlog compartilhada — fechado na 1.1: a
  `libspdlog` já é compilada contra a `libstdc++` dinâmica, então linkar
  estático não eliminaria DLL nenhuma, só duplicaria o runtime no processo
- DLLs de terceiros copiadas para junto do executável a cada build:
  `TARGET_RUNTIME_DLLS` resolve as de alvo importado, o runtime do compilador
  vai numa lista à mão (`CMakeLists.txt`)
- **Build e execução pelo PowerShell.** Pelo shell POSIX sandboxed o `g++`
  morre com *exit 1 e nenhum diagnóstico* — nem erro de sintaxe aparece, e o
  erro passa a parecer do código. Editar arquivo pelo shell POSIX é normal.

## Como trabalhar aqui

- Um módulo por vez, parar para revisão. Nunca despejar vários arquivos de uma vez.
- Explicações ancoradas em `arquivo:linha`.
- Estilo: `snake_case` para constantes (sem prefixo `k`), poucos comentários,
  sem cerimônia.
- Respostas em PT-BR.

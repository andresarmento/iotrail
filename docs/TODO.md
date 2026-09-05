# IoTrail — TODO

Backlog derivado de `docs/ROADMAP.md`. Uma tarefa por vez: eu apresento as
decisões em aberto → você decide → escrevo aquele pedaço → paro. Nada de
escrever a tarefa seguinte na mesma leva.

Cada tarefa fechada vira registro: o que ficou decidido e por quê. O raciocínio
longo mora em comentário junto da linha que o implementa; aqui fica o resumo.

**Estado:** Fase 1 desmembrada em 2026-09-05, nada implementado ainda.

---

## Decisões já fechadas — herdadas da base de conhecimento

Não reabrir sem motivo novo. Todas foram tomadas e validadas no projeto
anterior; a reescrita é de organização do código, não de rumo.

- **C++17 + STL.** C++20 foi avaliado e descartado nesta rodada — o ganho real
  seria `std::jthread`/`std::stop_token` e `std::span`. GCC 16.2.0 compila os
  dois, então subir depois continua possível. Sem extensões GNU.
- **Lib MQTT: mosquitto (`libmosquittopp`)**, MQTT 3.1.1. Validada contra broker
  real em `knowledge_base/src/test_3/`. Paho C++ nunca chegou a ser avaliado —
  não houve motivo.
- **Lib de logging: spdlog**, em modo assíncrono. Motivo em
  `knowledge_base/docs/decisao_sync_write.txt`: logar de forma síncrona no
  caminho de recebimento reintroduz a variância de latência que o projeto
  inteiro existe pra evitar.
- **Toolchain MSYS2 ucrt64**, CMake + Ninja, tudo vindo do `pacman`.
- **Arquitetura de ingestão:** fila em RAM → uma writer thread por stream, com
  `write_interval` (eficiência do batch) e `sync_interval` (durabilidade do
  fsync) separados.
- **Uma stream = uma fila = uma thread = uma pasta = seu próprio offset.**
- **Um broker por stream** (N:1, não N:M). Fan-in de vários brokers numa stream
  exigiria decidir dedup e ordenação entre origens, e perderia a proveniência.
- **Config em INI com seções tipadas** `[broker:nome]` / `[stream:nome]`.

---

## Fase 1 — Fundação do projeto

Termina na config. **Fora desta fase:** cliente MQTT, formato de registro,
writer/segmentos, camada de plataforma (fsync/truncate — entra junto com o
writer), roteamento tópico → stream, e **framework de teste** (retirado da fase
em 2026-09-05; era o item 1.4, também adiado na rodada anterior).

### 1.1 — Esqueleto de build
`CMakeLists.txt` que compila um `main.cpp` vazio. Sem lógica; o objetivo é ter o
terreno em que as próximas tarefas caem.

Decisões a tomar:
- Um alvo só (fontes direto no `add_executable`) ou biblioteca + executável fino?
  Sem framework de teste na fase, some a razão da segunda forma — reabrir quando
  o teste entrar.
- Layout de pastas: `src/` plano, ou subpasta quando um assunto tiver mais de um
  par `.h`/`.cpp`?
- `-Wall -Wextra -Wpedantic` com ou sem `-Werror`?
- Runtime C++ estático ou dinâmico? **Atenção:** o projeto antigo linkava
  estático e a reescrita reverteu isso com medição — a `libspdlog-1.17.dll` é
  compilada contra `libstdc++` dinâmica, então linkar estático faria existirem
  dois runtimes C++ no mesmo processo com `std::string` cruzando a fronteira.
  A decisão depende de spdlog compartilhada vs. header-only (468 KB contra
  5,6 MB, medido).

### 1.2 — Logging
Setup do spdlog em `logging.h`/`.cpp`.

Decisões a tomar:
- `spdlog::` direto nos pontos de chamada, wrapper próprio, ou `using`-declarations
  sob um namespace `logging`?
- Só console agora (arquivo rotativo fica pra Fase 9)?
- Assíncrono com thread pool própria: tamanho de fila e política de overflow.
- `init()`/`shutdown()` explícitos — com logger assíncrono, sair sem shutdown
  perde as últimas linhas da fila.

### 1.3 — Parada ordenada e `main` mínimo
Handler de sinal e um `main` que sobe, espera e encerra limpo.

Decisões a tomar:
- `SIGINT` + `SIGTERM` bastam? **Não no Windows:** `SIGINT` cobre só Ctrl+C —
  fechar a janela, Ctrl+Break, logoff e shutdown chegam por
  `SetConsoleCtrlHandler`. Achado registrado da rodada anterior.
- `std::atomic<bool>` ou `volatile sig_atomic_t`? (No Windows o handler roda em
  thread criada pelo SO — é comunicação entre threads, `volatile` não garante nada.)
- Espera do `main`: polling ou `condition_variable`?
- Restrição a carregar: em `CTRL_CLOSE_EVENT` o Windows mata o processo depois de
  poucos segundos — vira limite real quando houver fila pra drenar e fsync final.

### 1.4 — Localização de arquivos
Descobrir o diretório do executável, pra achar o `iotrail.conf`.

Decisões a tomar:
- O `.conf` mora ao lado do `.exe`, no diretório de trabalho, ou vem por
  argumento de linha de comando?
- Isolar o código específico de SO agora (`GetModuleFileNameW` no Windows,
  `/proc/self/exe` no Linux) ou deixar pra camada de plataforma da Fase 3?

### 1.5 — Config: leitura do arquivo
Ler o `iotrail.conf` e transformá-lo em estrutura de dados, sem interpretar.

Decisões a tomar:
- Parser genérico separado da interpretação (custa um par de arquivos a mais) ou
  um parser que já conhece broker e stream?
- Erro de sintaxe: para no primeiro ou lê o arquivo até o fim e reporta tudo de
  uma vez? (O segundo evita "um erro por boot".)
- Erros logados de dentro do parser ou devolvidos ao chamador? Logar por dentro
  é mais simples agora; devolver é o que torna a config testável depois.
- Comportamentos a cobrir: comentários `#`/`;`, trim, `=` obrigatório, cabeçalho
  sem `]`, seção sem nome, par antes de qualquer seção, chave repetida.
- **Dois achados a não perder** da rodada anterior: BOM UTF-8 no início do arquivo
  faz a primeira seção virar lixo; e `std::atoi` na porta aceita `"1883x"` como
  `1883` e `"abc"` como `0`, em silêncio.

### 1.6 — Config: validação e regras do domínio
Dar significado ao que 1.6 leu: o que é um broker válido, o que é uma stream válida.

Decisões a tomar:
- Config inválida derruba o boot ou cai em default? (O projeto antigo tinha
  fallback `127.0.0.1:1883`, descartado na reescrita: 1883 é convenção IANA, mas
  `127.0.0.1` não é convenção nenhuma pra "onde está meu broker".)
- Severidade de cada erro: o que é fatal, o que é só aviso.
- Uma seção ruim interrompe as outras ou o arquivo é lido até o fim?
- Onde validar nome de stream — aqui ou no writer? (Aqui falha no boot nomeando
  a seção culpada; lá falha num `fopen` obscuro depois.)
- A config devolve um grafo stream→broker já resolvido, ou uma lista de seções
  pra alguém interpretar depois?

### 1.7 — Fechamento da fase
- Atualizar `docs/DESIGN.md` com o que a fase fechou (build, logging, parada
  ordenada, config).
- Validar ponta a ponta: build limpo, boot com config válida, boot com config
  inválida, Ctrl+C e fechar a janela.
- Publicador de teste (`mosquitto_pub` de `tools/`, ou script Python) pronto pra
  Fase 2.

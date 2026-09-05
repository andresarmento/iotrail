# IoTrail — TODO

Backlog derivado de `docs/ROADMAP.md`. Uma tarefa por vez: eu apresento as
decisões em aberto → você decide → escrevo aquele pedaço → paro. Nada de
escrever a tarefa seguinte na mesma leva.

Cada tarefa fechada vira registro: o que ficou decidido e por quê. O raciocínio
longo mora em comentário junto da linha que o implementa; aqui fica o resumo.

**Estado:** Fase 1 desmembrada em 2026-09-05. Fechadas 1.1, 1.2 e 1.3.

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

### 1.1 — Esqueleto de build — FECHADO (2026-09-05)

`CMakeLists.txt` + `src/main.cpp` vazio. Compila, linka e roda.

**Decisões tomadas:**

- **Um alvo só**, fontes direto no `add_executable`. Biblioteca + executável
  fino só se pagaria pra um teste linkar o código, e o framework de teste ficou
  fora da fase. Reabrir quando entrar.
- **Layout plano** em `src/`, com subpasta quando um assunto tiver mais de um
  par `.h`/`.cpp`. Todo diretório de fonte entra no include path: include é
  sempre pelo nome do arquivo, nunca relativo atravessando pasta.
- **spdlog compartilhada + runtime C++ dinâmico**, sem `-static-libgcc
  -static-libstdc++`. A `libspdlog-1.17.dll` é compilada contra `libstdc++`
  dinâmica, então a DLL acompanha o programa de qualquer forma — linkar estático
  não eliminaria DLL nenhuma, só faria existir dois runtimes C++ no mesmo
  processo com `std::string` cruzando a fronteira. `.exe` de ~468 KB contra
  5,6 MB da rota header-only.
- **`-Wall -Wextra -Wpedantic -Werror`.** Verificado que dispara de verdade
  (variável não usada quebra o build). Escape se um upgrade do GCC quebrar:
  `-DCMAKE_CXX_FLAGS=-Wno-error`.
  **Só o nosso código**, e por dois motivos que se somam: a flag é por alvo,
  então entra só na linha dos nossos `.cpp` (spdlog e mosquitto vêm compiladas
  em DLL pelo `pacman`, nunca passam por este build); e o header de terceiro
  dentro do nosso `.cpp` já vem como `-isystem`, porque o CMake trata assim o
  include de alvo importado. **Não adicionar `-isystem` na mão** para
  `ucrt64/include`: reordena a busca e quebra o `#include_next` da libstdc++
  (testado — vira `fatal error: stdlib.h: No such file or directory`).
- **`CMAKE_BUILD_TYPE` default `RelWithDebInfo`** (`-O2 -g`). Ninja é
  single-config: sem isso o build sai sem `-O` e sem `-g`, e nada avisa — gap
  que o projeto anterior tinha. O default importa porque o IoTrail existe pra
  medir latência em edge, e número medido em `-O0` não vale nada. Override
  verificado: `-DCMAKE_BUILD_TYPE=Debug` dá `-g` sem otimização.
- **Saiu junto:** `cmake_minimum_required(3.21)` por `TARGET_RUNTIME_DLLS`;
  `CMAKE_CXX_EXTENSIONS OFF` (`-std=c++17`, não `gnu++17`);
  `CMAKE_EXPORT_COMPILE_COMMANDS ON`; e `IOTRAIL_MSYS2_UCRT64` como variável de
  cache, pra apontar pra outra instalação sem editar o arquivo.

**Nenhuma DLL é copiada ainda.** Conferido com `objdump -p` que o `.exe` com
`main.cpp` vazio depende só das `api-ms-win-crt-*` e `KERNEL32` do próprio
Windows. `libstdc++-6.dll` e `libgcc_s_seh-1.dll` passam a ser necessárias na
1.2; a lista sai de `TARGET_RUNTIME_DLLS` mais o que o `objdump` apontar, não de
lista copiada de outro projeto. `TARGET_RUNTIME_DLLS` também só pode entrar na
1.2 — com nenhum alvo importado, o `copy` do CMake falha com a lista vazia.

**Ambiente confirmado nesta máquina:** GCC 16.2.0, CMake 4.4.2, Ninja 1.13.2.

### 1.2 — Logging — FECHADO (2026-09-05)

Setup do spdlog em `src/logging.h`/`.cpp`. A lib já estava decidida; o que
faltava era como ela entra no código.

**Decisões tomadas:**

- **Nem `spdlog::` direto nem wrapper: `using`-declarations** (`logging.h:25-30`).
  `logging.h` reexporta `trace`/`debug`/`info`/`warn`/`error`/`critical` sob o
  namespace `logging`. Não é wrapper — sem indireção, sem sobrecarga, API `{}`
  idêntica — mas todo ponto de chamada escreve `logging::info`. Se a lib trocar,
  as `using` viram funções de verdade e nenhum ponto de chamada muda.
- **Só console** (`stdout_color_sink_mt`). Arquivo rotativo continua Fase 9.
- **Logger assíncrono**, thread pool própria: fila de 8192, **1 worker** — mais
  de um embaralharia a ordem das linhas, que é metade do valor de um log.
- **Overflow `overrun_oldest`, não `block`** (`logging.cpp:19-21`) — **diverge da
  rodada anterior, que usava `block`.** Com `block`, fila cheia devolve o console
  ao caminho de recebimento/gravação, que é exatamente o acoplamento que o logger
  assíncrono existe pra cortar. E no Windows isso não é hipotético: clicar dentro
  da janela do console liga o *QuickEdit selection* e **congela o stdout** até um
  Esc — com `block`, um clique acidental na janela para a ingestão MQTT. Preço
  aceito: o descarte é silencioso, só aparece como buraco na sequência. Perder
  linha de log é menos grave que perder dado.
- **Flush por mensagem** (`flush_on(trace)`). Com o logger assíncrono o flush
  roda na thread do pool, não custa latência a quem chamou, e garante que a
  última linha antes de um crash saiu.
- **Nível `info` fixo + `SPDLOG_LEVEL` do ambiente**
  (`spdlog::cfg::load_env_levels()`, `logging.cpp:28`). `set SPDLOG_LEVEL=debug`
  liga debug/trace sem recompilar. Chave própria de config ficou fora de
  propósito: o logger sobe **antes** da config ser lida (senão erro de config não
  teria onde sair), então uma chave exigiria um `set_level()` posterior de
  qualquer forma — decidir isso é assunto de 1.6, não daqui. Verificado:
  `SPDLOG_LEVEL=off` silencia, `=debug` mostra a linha de debug.
- **Padrão com milissegundos**: `[%H:%M:%S.%e] [%^%l%$] %v`. O projeto existe pra
  medir latência; timestamp com resolução de segundo não correlaciona log com o
  que o writer fez. `%t` (thread id) entra quando houver mais de uma thread.
- **`init()`/`shutdown()` explícitos**, chamados em toda saída do `main()`. Sem o
  `shutdown()` o processo sai com mensagens ainda na fila — inclusive a que
  explica por que ele está saindo.

**Saiu junto (a parte de build que a 1.1 deixou marcada):**
`find_package(spdlog CONFIG REQUIRED)` + `spdlog::spdlog` (a compartilhada, não
`spdlog::spdlog_header_only`), e a **cópia de DLL**, agora que existe alvo
importado pra `TARGET_RUNTIME_DLLS` resolver. Duas listas por terem origens
diferentes: `libspdlog-1.17.dll` sai do `TARGET_RUNTIME_DLLS` (sem número de
versão em texto, que quebraria calado no próximo upgrade); `libstdc++-6.dll`,
`libgcc_s_seh-1.dll` e `libwinpthread-1.dll` ficam na mão porque não vêm de alvo
importado nenhum. A lista é o que o `objdump -p build/iotrail.exe` aponta hoje,
tirando `api-ms-win-crt-*` e `KERNEL32`, que são do Windows. **Não há
`libfmt-12.dll`**: o pacote do MSYS2 compila spdlog com `SPDLOG_FMT_EXTERNAL`,
mas o fmt entra estático dentro da `libspdlog` (conferido com `objdump -p` na
própria DLL). `.exe` de 738 KB.

**Validado:** build limpo (zero aviso com `-Werror`), roda e imprime; e roda com
o `PATH` sem o MSYS2 — ou seja, a cópia de DLL é o que sustenta rodar de dentro
do `build/`, não o `PATH` da máquina.

**Ressalva de ambiente desta máquina:** compilar pelo shell POSIX sandboxed
falha com *exit 1 e nenhuma mensagem* (o GCC morre sem conseguir escrever os
temporários). Build e execução vão pelo PowerShell.

**IntelliSense (saiu junto, `.vscode/`):** não havia `.vscode/` nenhum, e a
extensão C/C++ rodava no default — sem o include path do projeto e tentando o
MSVC, o que fazia `#include "logging.h"` e os headers do spdlog aparecerem como
não encontrados. `c_cpp_properties.json` passa a apontar para o
`build/compile_commands.json`, que o `CMAKE_EXPORT_COMPILE_COMMANDS` já gerava e
ninguém consumia — assim flag ou include novo no `CMakeLists.txt` chega ao
IntelliSense no próximo configure, sem manutenção. Mais `includePath` com `src`
como fallback (o `compile_commands.json` mora em `build/`, que não vai pro git —
num clone novo o IntelliSense ficaria cego até alguém buildar) e o
`compilerPath` do `g++` do ucrt64, de onde saem os headers de sistema.
O `settings.json` exclui `knowledge_base/` do banco de símbolos: ela tem um
`src/logging.h` de mesmo nome, e "ir para definição" caía no código velho.
**Ressalva:** o `compilerPath` é caminho fixo, ao contrário do `CMakeLists.txt`,
que usa a variável de cache `IOTRAIL_MSYS2_UCRT64` — a extensão não lê variável
do CMake, não há como parametrizar.

### 1.3 — Parada ordenada e `main` mínimo — FECHADO (2026-09-05)

`src/signals.h`/`.cpp` e o laço de espera do `main`.

**Decisões tomadas:**

- **`SIGINT` + `SIGTERM` + `SetConsoleCtrlHandler`** (`signals.cpp:61-70`).
  `SIGINT` no Windows cobre só Ctrl+C; fechar a janela, Ctrl+Break, logoff e
  shutdown do sistema chegam pelo handler do console e de outra forma matariam o
  processo sem parada ordenada. `SIGTERM` no Windows **não é entregue por
  ninguém** — está ali pelo porte pra Linux, onde é o sinal que um supervisor
  manda.
- **`std::atomic<bool>`, não `volatile sig_atomic_t`** (`signals.cpp:20`). No
  Windows os dois handlers rodam em thread criada pelo SO: é comunicação entre
  threads, não interrupção de sinal, e `volatile` não garante nada aí.
- **Espera do `main` em polling de 200 ms** (`main.cpp:19`), não
  `condition_variable`. O `main` não tem trabalho pendurado na espera, e
  notificar uma cv de dentro do handler de `SIGINT` não é async-signal-safe — se
  um dia precisar acordar na hora, o caminho é um evento do SO, não cv. O custo
  não é só latência de saída: esses 200 ms saem do prazo de fechamento abaixo.
- **Handler não loga** (`signals.cpp:29-31`): logar chamaria `malloc` e travaria
  o mutex do spdlog em contexto de sinal. Quem anuncia a parada é o `main`,
  depois do laço. Preço aceito: o log não diz *qual* evento pediu pra parar.
- **`#ifdef _WIN32` inline no `signals.cpp`**, sem `platform/`. A camada de
  plataforma é da Fase 3 (junto com fsync/truncate) e um `#ifdef` num arquivo só
  não é o que vai doer. A 1.4 responde igual pro `GetModuleFileNameW`.
- **`request_stop()` público** (`signals.cpp:73`) — **diverge da rodada
  anterior**, que adiou pra Fase 6/7. Não custa nada e os próprios handlers
  passam a escrever a flag por ele, então há um ponto de escrita só.
- **Nomes em `snake_case`** (`stop_requested()`, não o `stopRequested()` do
  código anterior). Fixado aqui como convenção do projeto — o `CLAUDE.md` só
  falava de constantes, e `logging::init()`/`shutdown()` não desempatavam.

**O achado da tarefa — retornar `TRUE` não segura o processo:** o registro
herdado (`knowledge_base/iotrail_refactory/TODO.md:124-127`) tratava o `TRUE` do
handler como suficiente, com a ressalva de que o Windows mata "depois de poucos
segundos". **Medido aqui, é mais estreito que isso:** em
`CLOSE`/`LOGOFF`/`SHUTDOWN` o Windows mata o processo assim que o handler
**retorna** — os poucos segundos são o prazo pra trabalhar *dentro* dele. Com o
handler só setando a flag e voltando, fechar a janela matava em **~2 ms**, exit
code `0xC000013A`, antes de o `main` sequer acordar dos 200 ms: não havia parada
ordenada nenhuma. Em `CTRL_C`/`CTRL_BREAK` o `TRUE` continua bastando.

Por isso o handler **bloqueia** nesses três eventos até `shutdown_done()`
(`signals.cpp:81`, chamado em `main.cpp:24` como última linha do `main`), com
teto de **3 s** (`signals.cpp:24`). O teto não é gosto: **o prazo real desta
máquina foi medido em 5013 ms** (probe com handler que nunca retorna), e o valor
sai do registro do Windows, mudando de máquina pra máquina. 3 s deixa margem —
estourar o nosso teto ainda sai pelo caminho normal, estourar o do SO é morte no
meio do trabalho.

**Validado** (scripts de teste fora do repo):

| evento | como | resultado |
|---|---|---|
| Ctrl+C | à mão, no terminal | encerra limpo |
| Ctrl+Break | `GenerateConsoleCtrlEvent` | exit 0, log de encerramento |
| fechar a janela | `WM_CLOSE` na janela do console | exit 0 em 171 ms, log de encerramento |
| fechar a janela, *antes* da espera | idem | exit `0xC000013A` em 2 ms, sem log |

Detalhe de teste que custou tempo: `GenerateConsoleCtrlEvent` só alcança
processos que **compartilham o console do chamador** — com o filho em console
novo o evento não chega, e parece bug do programa.

**Restrição carregada para a Fase 3:** o encerramento de verdade (drenar M filas,
`fsync` final de M streams) roda **dentro** do handler, e o orçamento é o prazo
do SO (~5 s aqui) menos os 200 ms do polling. Se não couber: limitar a drenagem e
aceitar perda, ou tentar até o fim e arriscar ser morto no meio.

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
- Atualizar `docs/DESIGN.md` com o que a fase fechou (build, logging, config).
  A parada ordenada já entrou junto da 1.3 (`DESIGN.md` §6), porque a restrição
  de prazo do encerramento condiciona o writer da Fase 3.
- Validar ponta a ponta: build limpo, boot com config válida, boot com config
  inválida, Ctrl+C e fechar a janela.
- Publicador de teste (`mosquitto_pub` de `tools/`, ou script Python) pronto pra
  Fase 2.

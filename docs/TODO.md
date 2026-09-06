# IoTrail — TODO

Backlog derivado de `docs/ROADMAP.md`. Uma tarefa por vez: eu apresento as
decisões em aberto → você decide → escrevo aquele pedaço → paro. Nada de
escrever a tarefa seguinte na mesma leva.

Cada tarefa fechada vira registro: o que ficou decidido e por quê. O raciocínio
longo mora em comentário junto da linha que o implementa; aqui fica o resumo.

**Estado:** Fase 1 desmembrada em 2026-09-05. Fechadas 1.1 a 1.5.

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
  (`spdlog::cfg::load_env_levels()`, `logging.cpp:28`).
  **No PowerShell é `$env:SPDLOG_LEVEL="debug"`** (ou `"trace"`), não
  `set SPDLOG_LEVEL=debug` — este é sintaxe do `cmd`, e no PowerShell o `set` é
  apelido de `Set-Variable`: cria variável de sessão, o ambiente fica vazio e o
  programa continua em `info` sem nenhum aviso. Vale só no terminal onde foi
  setada. Liga debug/trace sem recompilar. Chave própria de config ficou fora de
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
(`signals.cpp:81`, chamado em `main.cpp:37` como última linha do `main`), com
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

### 1.4 — Localização de arquivos — FECHADO (2026-09-05)

`src/paths.h`/`.cpp`: descobrir o diretório do executável pra achar o
`iotrail.conf`, mais a linha de comando do `main`.

**Decisões tomadas:**

- **Diretório do executável, não o de trabalho** (`paths.cpp:109-116`). O
  diretório de trabalho é de quem chama — atalho, serviço, tarefa agendada — e
  não tem relação com onde o programa foi instalado. O contrato já estava
  escrito no cabeçalho do `iotrail.conf:6-9` e na cópia do build
  (`CMakeLists.txt:121-127`); agora o código cumpre.
- **`-c <arquivo>` sobrepõe** (`paths.cpp:89-98`). Uma flag, não argumento
  posicional: posicional envelhece mal quando entrar o segundo (`--data-dir`).
  Vale pra rodar duas instâncias do mesmo binário com configs diferentes, caso
  que a própria config já prevê (`iotrail.conf:16-19`, client_id coincidente).
- **Argumento desconhecido ou `-c` sem valor derrubam o boot** com código 1 e a
  linha de uso. Ignorar argumento errado faz o programa subir com config
  diferente da que a pessoa pediu, e ela só descobre pelo dado que não chegou.
- **Resolvido antes de `signals::init()`** (`main.cpp:20-24`). Nesse ponto não
  há fila nem writer, então sair é só `logging::shutdown()` e `return 1` — sem
  passar pelo `shutdown_done()`, que ninguém está esperando ainda.
- **Só resolve o caminho, não abre nem confere existência.** Quem abre é a 1.5,
  e é lá que o erro "não achei em X" tem o `errno` pra dizer *por quê* (não
  existe, sem permissão, é um diretório). Conferir aqui daria duas mensagens
  pro mesmo problema e uma janela entre o teste e o `fopen`.
- **`std::filesystem::path` como moeda, não `std::string`** (`paths.h:12-19`).
  No Windows o `path` guarda `wchar_t` nativo, então acento no caminho
  atravessa sem passar pela codepage; e o `data/<stream>/` da Fase 3 vai
  precisar da mesma coisa. C++17 já traz, sem `-lstdc++fs` no GCC atual.
- **`#ifdef _WIN32` inline, sem `platform/`** — igual à 1.3. A camada de
  plataforma continua Fase 3, junto do `fsync`/`truncate`. O ramo POSIX
  (`/proc/self/exe`) está escrito mas **não testado**; existe pelo porte.

**O detalhe que dá trabalho:** `GetModuleFileNameW` **não avisa por retorno**
quando o buffer é pequeno — devolve o tamanho do próprio buffer e o "não coube"
fica só no `GetLastError`. Daí o laço comparar `n` com o tamanho e dobrar o
buffer (`paths.cpp:41-58`), com teto de 32768 (`paths.cpp:29`). `readlink` tem o
mesmo formato de armadilha, mais o fato de não terminar em `\0`. E é `W` de
propósito: a versão `A` converte pra codepage ANSI e caminho acentuado vira `?`.

**Limite conhecido:** `argv` é narrow e no Windows vem na codepage ANSI, então
caminho acentuado passado em `-c` pode não sobreviver — o default (diretório do
exe) não tem esse problema. A saída, se doer, é `CommandLineToArgvW`
(`paths.cpp:94-97`).

**Validado** (build limpo com `-Werror`, exe rodado de `%TEMP%`):

| caso | resultado |
|---|---|
| sem argumento, cwd em outra pasta | `config: <build>\iotrail.conf` |
| `-c <caminho absoluto>` | usa o caminho dado |
| `-c` sem valor / `-x` / `--nope` | erro + linha de uso, exit 1 |
| exe copiado pra pasta com acentos | caminho correto, sem `?` |

O log do caminho sai em UTF-8 (`path::string()`); console em codepage 850/1252
mostra acento embaralhado — é display do terminal, o caminho aberto é o certo.

### 1.5 — Config: leitura do arquivo — FECHADO (2026-09-05)

`src/config/ini.h`/`.cpp`: quebra o arquivo em seções e pares `chave=valor`, sem
interpretar nada. O desenho reaproveita o parser da rodada anterior
(`knowledge_base/iotrail_refactory/src/config/ini.cpp`).

**Decisões tomadas:**

- **Parser genérico, separado do domínio** (`ini.h:27-35`). O cabeçalho vira
  `type`/`name` (`[broker:casa]` → `"broker"`/`"casa"`), e *exigir* o tipo, ou
  saber que broker precisa de `host`, é regra da 1.6. Isso já são dois pares
  `.h`/`.cpp` no mesmo assunto (`ini.*` agora, `config.*` na 1.6), então
  disparou a regra de layout da 1.1 e nasceu a subpasta `src/config/` — a
  primeira do projeto. Ela entra no include path (`CMakeLists.txt:65`), então o
  include continua sendo `"ini.h"`, sem caminho relativo.
- **Erros logados de dentro do parser**, `std::optional` de volta —
  **eu tinha recomendado o contrário** (devolver um vetor de erros pro chamador
  logar) e a recomendação não se sustentou: a 1.6 não faria nada com esses erros
  além de logar, porque config inválida derruba o boot de qualquer jeito
  (`DESIGN.md:116`); "reportar tudo de uma vez" já sai de graça logando por
  dentro; e testar continua possível pendurando um sink no spdlog. Reabrir se
  aparecer `--check-config` ou reload a quente (Fase 9), que vão querer
  severidade diferente da fixada aqui.
- **Lê até o fim e conta** (`ini.cpp:121-124`): cada problema vira uma linha
  `arquivo:linha: mensagem`, e no fim uma linha com o total. Sem isso, corrigir
  uma config ruim custa um boot por erro.
- **`parse(istream)` + `parse_file(path)`** (`ini.h:43-44`). O miolo não conhece
  arquivo: com o framework de teste, os casos ruins entram por `istringstream`,
  sem espalhar fixture pelo disco. O `parse_file` passa o `fs::path` direto pro
  `ifstream` (`ini.cpp:130`), mantendo o caminho nativo que a 1.4 preservou.
- **Sem comentário de fim de linha** (`ini.cpp:47-51`) — só `#`/`;` abrindo a
  linha. Não é preguiça: `#` é o wildcard multinível do MQTT, e cortar dali pra
  frente transformaria `topics=umidade/#` (`iotrail.conf:57`) em
  `topics=umidade/`. A stream subscreveria outro tópico, calada. O parser
  anterior se comportava assim por omissão; aqui é decisão.
- **Valor vazio (`host=`) passa** (`ini.cpp:93-95`): a chave foi escrita, existe.
  Se vazio é aceitável depende da chave — domínio, 1.6. Mesma lógica pro `atoi`
  da porta: aqui tudo é string, a conversão com `std::from_chars` (que rejeita
  `"1883x"`, ao contrário do `atoi`) é 1.6.
- **Falha ao abrir diz o motivo** (`ini.cpp:131-139`) — a dívida que a 1.4
  deixou. `errno` zerado antes do `ifstream` e `strerror` depois: "No such file
  or directory" e "Permission denied" (o que o Windows devolve quando o caminho
  é um diretório) pedem correções diferentes.
- **Tipos em `snake_case`** (`entry`, `section`, `sections`) — o código anterior
  usava `Entry`/`Section`. Convenção nova, fixada aqui: segue a STL e o resto do
  projeto, que já é `snake_case` desde a 1.3.

**Fora do escopo, de propósito:** nome de seção repetido (`[broker:casa1]` duas
vezes) é domínio, 1.6; aspas em valor, continuação de linha e `include` não têm
caso de uso.

**Validado** (`SPDLOG_LEVEL=trace`, arquivos de teste no scratchpad):

| caso | resultado |
|---|---|
| `iotrail.conf` real | 5 seções, `topics=umidade/#` chega inteiro |
| BOM UTF-8 + CRLF | seção 1 encontrada, valor sem `\r` |
| 7 erros num arquivo só | 7 linhas + total, exit 1, nenhum erro engolido |
| `#` no meio do valor | preservado, não vira comentário |
| arquivo inexistente | `No such file or directory` |
| `-c <um diretório>` | `Permission denied` |

O `main` passou a listar as seções em `debug` e os pares em `trace`
(`main.cpp:33-39`) — é como se confere o que o parser leu sem depurador.

### 1.6 — Config: validação e regras do domínio
Dar significado ao que a 1.5 leu: o que é um broker válido, o que é uma stream válida.

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

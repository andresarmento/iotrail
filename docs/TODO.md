# IoTrail — TODO

Backlog derivado de `docs/ROADMAP.md`. Uma tarefa por vez: eu apresento as
decisões em aberto → você decide → escrevo aquele pedaço → paro. Nada de
escrever a tarefa seguinte na mesma leva.

Cada tarefa fechada vira registro: o que ficou decidido e por quê. O raciocínio
longo mora em comentário junto da linha que o implementa; aqui fica o resumo.

**Estado:** Fase 1 **fechada** em 2026-09-05 (1.1 a 1.7). Fase 2 desmembrada na
mesma data e **fechada** em 2026-09-06 (2.1 a 2.7). Em curso: Fase 3, formato de
registro e writer — desmembrada em 2026-09-06 (oito tarefas), 3.1 fechada na
mesma data e o desmembramento revisto para dez. 3.2 fechada em 2026-09-07,
próxima tarefa: 3.3.

---

## Decisões já fechadas — herdadas da base de conhecimento

Não reabrir sem motivo novo. Todas foram tomadas e validadas no projeto
anterior; a reescrita é de organização do código, não de rumo.

- **C++17 + STL.** C++20 foi avaliado e descartado nesta rodada — o ganho real
  seria `std::jthread`/`std::stop_token` e `std::span`. GCC 16.2.0 compila os
  dois, então subir depois continua possível. Sem extensões GNU.
- **Lib MQTT: mosquitto**, MQTT 3.1.1. Validada contra broker real em
  `knowledge_base/src/test_3/`. Paho C++ nunca chegou a ser avaliado — não houve
  motivo. **A API é a C (`libmosquitto`)**, decidido na 2.1: o wrapper
  `libmosquittopp`, usado nas rodadas anteriores, não expõe o `mosquitto*`
  interno e fecharia a porta pro MQTT 5.
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

- **Nem `spdlog::` direto nem wrapper: `using`-declarations** (`logging.h:14-19`).
  `logging.h` reexporta `trace`/`debug`/`info`/`warn`/`error`/`critical` sob o
  namespace `logging`. Não é wrapper — sem indireção, sem sobrecarga, API `{}`
  idêntica — mas todo ponto de chamada escreve `logging::info`. Se a lib trocar,
  as `using` viram funções de verdade e nenhum ponto de chamada muda.
- **Só console** (`stdout_color_sink_mt`). Arquivo rotativo continua Fase 9.
- **Logger assíncrono**, thread pool própria: fila de 8192, **1 worker** — mais
  de um embaralharia a ordem das linhas, que é metade do valor de um log.
- **Overflow `overrun_oldest`, não `block`** (`logging.cpp:18-20`) — **diverge da
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
- **Nível `info` fixo** (`logging.cpp:23`). Quem sobe pra debug/trace é o
  `-v`/`-vv` da linha de comando (adendo da 1.4). Chave própria de config ficou
  fora de propósito: o logger sobe **antes** da config ser lida (senão erro de
  config não teria onde sair), então uma chave exigiria um `set_level()`
  posterior de qualquer forma — que é exatamente o que o `-v` faz hoje.
  **A variável `SPDLOG_LEVEL` existiu aqui e foi removida em 2026-09-05**
  (`spdlog::cfg::load_env_levels()`): dois jeitos de fazer a mesma coisa, com
  regra de precedência pra lembrar, e o menos descobrível dos dois cobrou caro —
  no PowerShell a sintaxe do `cmd` (`set SPDLOG_LEVEL=debug`) falha **calada**,
  porque `set` ali é apelido de `Set-Variable` e cria variável de sessão, não de
  ambiente. Voltar custa uma linha mais o `#include <spdlog/cfg/env.h>`.
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
- **Espera do `main` em polling de 200 ms** (`main.cpp:57-59`), não
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
(`signals.cpp:81`, chamado em `main.cpp:63` como última linha do `main`), com
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
`iotrail.conf`, mais a linha de comando do `main`. O parsing de argumentos saiu
daqui no adendo do `-v` (abaixo) e hoje mora em `src/cmdline.*` — as âncoras deste
registro já apontam pra lá.

**Decisões tomadas:**

- **Diretório do executável, não o de trabalho** (`cmdline.cpp:42-50`). O
  diretório de trabalho é de quem chama — atalho, serviço, tarefa agendada — e
  não tem relação com onde o programa foi instalado. O contrato já estava
  escrito no cabeçalho do `iotrail.conf:6-9` e na cópia do build
  (`CMakeLists.txt:121-127`); agora o código cumpre.
- **`-c <arquivo>` sobrepõe** (`cmdline.cpp:25-32`). Uma flag, não argumento
  posicional: posicional envelhece mal quando entrar o segundo (`--data-dir`).
  Vale pra rodar duas instâncias do mesmo binário com configs diferentes, caso
  que a própria config já prevê (`iotrail.conf:16-19`, client_id coincidente).
- **Argumento desconhecido ou `-c` sem valor derrubam o boot** com código 1 e a
  linha de uso. Ignorar argumento errado faz o programa subir com config
  diferente da que a pessoa pediu, e ela só descobre pelo dado que não chegou.
- **Resolvido antes de `signals::init()`** (`main.cpp:18-22`). Nesse ponto não
  há fila nem writer, então sair é só `logging::shutdown()` e `return 1` — sem
  passar pelo `shutdown_done()`, que ninguém está esperando ainda.
- **Só resolve o caminho, não abre nem confere existência.** Quem abre é a 1.5,
  e é lá que o erro "não achei em X" tem o `errno` pra dizer *por quê* (não
  existe, sem permissão, é um diretório). Conferir aqui daria duas mensagens
  pro mesmo problema e uma janela entre o teste e o `fopen`.
- **`std::filesystem::path` como moeda, não `std::string`** (`paths.h:13`, `cmdline.h:13`).
  No Windows o `path` guarda `wchar_t` nativo, então acento no caminho
  atravessa sem passar pela codepage; e o `data/<stream>/` da Fase 3 vai
  precisar da mesma coisa. C++17 já traz, sem `-lstdc++fs` no GCC atual.
- **`#ifdef _WIN32` inline, sem `platform/`** — igual à 1.3. A camada de
  plataforma continua Fase 3, junto do `fsync`/`truncate`. O ramo POSIX
  (`/proc/self/exe`) está escrito mas **não testado**; existe pelo porte.

**O detalhe que dá trabalho:** `GetModuleFileNameW` **não avisa por retorno**
quando o buffer é pequeno — devolve o tamanho do próprio buffer e o "não coube"
fica só no `GetLastError`. Daí o laço comparar `n` com o tamanho e dobrar o
buffer (`paths.cpp:27-44`), com teto de 32768 (`paths.cpp:23`). `readlink` tem o
mesmo formato de armadilha, mais o fato de não terminar em `\0`. E é `W` de
propósito: a versão `A` converte pra codepage ANSI e caminho acentuado vira `?`.

**Limite conhecido:** `argv` é narrow e no Windows vem na codepage ANSI, então
caminho acentuado passado em `-c` pode não sobreviver — o default (diretório do
exe) não tem esse problema. A saída, se doer, é `CommandLineToArgvW`
(`cmdline.cpp:30-32`).

**Validado** (build limpo com `-Werror`, exe rodado de `%TEMP%`):

| caso | resultado |
|---|---|
| sem argumento, cwd em outra pasta | `config: <build>\iotrail.conf` |
| `-c <caminho absoluto>` | usa o caminho dado |
| `-c` sem valor / `-x` / `--nope` | erro + linha de uso, exit 1 |
| exe copiado pra pasta com acentos | caminho correto, sem `?` |

O log do caminho sai em UTF-8 (`path::string()`); console em codepage 850/1252
mostra acento embaralhado — é display do terminal, o caminho aberto é o certo.

**Adendo (2026-09-05): `-v`/`-vv` e a saída do `src/cmdline.*`.** Pedido depois
da 1.5, quando a variável de ambiente que fazia esse papel se mostrou pouco
descobrível — a flag aparece na linha de uso a cada erro de argumento, e vale
também quando o programa sobe sem ambiente nenhum (botão Run do VS Code).

- **`-v` = debug, `-vv` = trace** (`cmdline.cpp:33-36`). Contados: `-v -v` soma o
  mesmo que `-vv`, e `-vvv` satura em trace (`max_verbose`, `cmdline.cpp:12`) em vez
  de virar erro — recusar seria explicar um limite que não interessa a ninguém.
- **Sem flag, vale o `info` que o `logging::init()` montou** (`main.cpp:25-29`).
  A flag é a única forma de mudar o nível: a variável de ambiente que dividia
  esse papel foi removida junto (ver 1.2). Um jeito só, sem regra de
  precedência pra lembrar.
- **`set_level` aplicado depois do `init()`**, não antes: o logger precisa
  existir. É o `set_level()` posterior que o registro da 1.2 já previa como
  inevitável. Consequência: o `-v` não alcança `debug`/`trace` que venham a
  existir dentro do próprio `cmdline::parse()` ou do `paths::exe_dir()`. Hoje
  não há nenhum ali.
- **`logging.h:23-24` ganhou `namespace level = spdlog::level;` e
  `using spdlog::set_level;`** — mesma linha da decisão da 1.2 (reexportar em vez
  de embrulhar), então o ponto de chamada escreve
  `logging::set_level(logging::level::debug)` sem `spdlog::` aparecer no `main`.
- **O parsing de argumentos saiu do `paths` pro `src/cmdline.*`.** Com um segundo
  flag, `paths::config_from_args()` era um nome que mentia: `-v` não tem nada a
  ver com caminho. Hoje `paths` só exporta `exe_dir()` (`paths.h:13`) e o `cmdline`
  compõe o default a partir dele (`cmdline.cpp:42-50`). O `main` recebe um
  `cmdline::options` (`cmdline.h:11-16`), não um `path` solto.
- **Sem `-h`/`--help` por enquanto.** A linha de uso já sai em todo erro de
  argumento; um `-h` de verdade precisa sair com código 0, e o contrato de hoje
  é "`nullopt` = falhou". Quando entrar, é um campo a mais em `options`.

**Validado:**

| caso | resultado |
|---|---|
| sem flag | só `[info]` |
| `-v` | `[debug]` das seções |
| `-vv` / `-v -v` | `[debug]` + `[trace]` dos pares |
| `-vvv` | igual a `-vv` |
| `-z` | erro + uso (`[-c <arquivo.conf>] [-v\|-vv]`), exit 1 |

### 1.5 — Config: leitura do arquivo — FECHADO (2026-09-05)

`src/config/ini.h`/`.cpp`: quebra o arquivo em seções e pares `chave=valor`, sem
interpretar nada. O desenho reaproveita o parser da rodada anterior
(`knowledge_base/iotrail_refactory/src/config/ini.cpp`).

**Decisões tomadas:**

- **Parser genérico, separado do domínio** (`ini.h:24-32`). O cabeçalho vira
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
- **Lê até o fim e conta** (`ini.cpp:111-114`): cada problema vira uma linha
  `arquivo:linha: mensagem`, e no fim uma linha com o total. Sem isso, corrigir
  uma config ruim custa um boot por erro.
- **`parse(istream)` + `parse_file(path)`** (`ini.h:35-36`). O miolo não conhece
  arquivo: com o framework de teste, os casos ruins entram por `istringstream`,
  sem espalhar fixture pelo disco. O `parse_file` passa o `fs::path` direto pro
  `ifstream` (`ini.cpp:118`), mantendo o caminho nativo que a 1.4 preservou.
- **Sem comentário de fim de linha** (`ini.cpp:45`) — só `#`/`;` abrindo a
  linha. Não é preguiça: `#` é o wildcard multinível do MQTT, e cortar dali pra
  frente transformaria `topics=umidade/#` (`iotrail.conf:57`) em
  `topics=umidade/`. A stream subscreveria outro tópico, calada. O parser
  anterior se comportava assim por omissão; aqui é decisão.
- **Valor vazio (`host=`) passa** (`ini.cpp:87-91`): a chave foi escrita, existe.
  Se vazio é aceitável depende da chave — domínio, 1.6. Mesma lógica pro `atoi`
  da porta: aqui tudo é string, a conversão com `std::from_chars` (que rejeita
  `"1883x"`, ao contrário do `atoi`) é 1.6.
- **Falha ao abrir diz o motivo** (`ini.cpp:118-127`) — a dívida que a 1.4
  deixou. `errno` zerado antes do `ifstream` e `strerror` depois: "No such file
  or directory" e "Permission denied" (o que o Windows devolve quando o caminho
  é um diretório) pedem correções diferentes.
- **Tipos em `snake_case`** (`entry`, `section`, `sections`) — o código anterior
  usava `Entry`/`Section`. Convenção nova, fixada aqui: segue a STL e o resto do
  projeto, que já é `snake_case` desde a 1.3.

**Fora do escopo, de propósito:** nome de seção repetido (`[broker:casa1]` duas
vezes) é domínio, 1.6; aspas em valor, continuação de linha e `include` não têm
caso de uso.

**Validado** (com `-vv`, arquivos de teste no scratchpad):

| caso | resultado |
|---|---|
| `iotrail.conf` real | 5 seções, `topics=umidade/#` chega inteiro |
| BOM UTF-8 + CRLF | seção 1 encontrada, valor sem `\r` |
| 7 erros num arquivo só | 7 linhas + total, exit 1, nenhum erro engolido |
| `#` no meio do valor | preservado, não vira comentário |
| arquivo inexistente | `No such file or directory` |
| `-c <um diretório>` | `Permission denied` |

Na época o `main` listava as seções cruas em `debug` e os pares em `trace`; a 1.6
trocou esse despejo pelo resumo da config já validada.

### 1.6 — Config: validação e regras do domínio — FECHADO (2026-09-05)

`src/config/config.h`/`.cpp`: dá significado ao que a 1.5 leu. Boa parte veio
pronta de `knowledge_base/iotrail_refactory/src/config/config.cpp` — a reescrita
aqui é de organização e de dois furos encontrados no teste (abaixo).

**Decisões tomadas:**

- **`load(path)` + `validate(sections)` separados** (`config.h:43-44`), como na
  1.5: `load` só encadeia `ini::parse_file` e `validate` (`config.cpp:347-351`).
- **Reflete o arquivo e valida; não deriva nada.** A união dos `topics=` por
  broker é o cliente MQTT que monta a partir das streams (Fase 2) — uma
  representação derivada em vez de duas que podem divergir.
- **A linha entre fatal e aviso:** fatal quando o programa não teria o que
  fazer, ou faria a coisa errada calado; aviso quando o operador provavelmente
  errou mas o comportamento resultante ainda é definido. Fatais: `host` ausente
  ou vazio, porta não numérica ou fora de 1–65535, `type != mqtt`, broker ou
  stream declarado duas vezes, nome de stream inválido, `broker=` ausente/vazio/
  com lista, `broker=` citando broker não declarado, `topics=` ausente ou vazio,
  seção sem tipo, zero brokers, zero streams. Avisos: chave desconhecida
  (`config.cpp:53-61`), tipo de seção desconhecido, `client_id` acima de 23
  caracteres, `client_id` repetido no mesmo `host:porta`, broker sem stream.
- **Nada de `std::atoi` na porta** (`config.cpp:19-29`): o `strtol` só vale se
  consumiu a string inteira — `atoi` aceitaria `"1883x"` como 1883 e `"abc"`
  como 0, calado. Era um dos dois achados que a 1.5 mandou não perder.
- **Padrão de tópico não é validado aqui** (`config.cpp:238`). Quem diz se um
  filtro MQTT é legal é o broker, na subscrição (Fase 2); duplicar essa regra
  aqui seria manter duas versões dela.
- **Aviso de tópico repetido entre streams ficou de fora** — gravar o mesmo
  tópico em duas streams é decisão legítima (retenções diferentes).
- **`broker=` aceita um nome só, e a lista é rejeitada explicitamente**
  (`config.cpp:207-219`): sem isso `casa,fabrica` viraria um nome literal e o
  erro sairia como "broker não declarado", que não diz o que a pessoa fez de
  errado.
- **Nome de stream validado aqui, não no writer** (`config.cpp:63-83`): só
  `[A-Za-z0-9_-]+`, mais os reservados do DOS (`CON`, `NUL`, `COM1`–`COM9`,
  `LPT1`–`LPT9`), que passam na regra de caracteres mas não são criáveis no
  Windows. Falhar aqui nomeia a seção culpada; falhar lá seria um `fopen`
  obscuro depois.
- **Broker citado é checado contra os *declarados*, não contra os carregados**
  (`config.cpp:224-231`): senão um broker com porta inválida geraria também um
  "broker desconhecido", culpando a stream por erro alheio.

**Seção `[general]`, nova nesta rodada** (não existia na base de conhecimento):

- **Sem tipo** — é a exceção à regra "seção precisa de tipo", porque não há o
  que nomear. Qualquer outra seção sem tipo continua fatal (`config.cpp:301-306`).
- **`data_dir`**, onde os segmentos serão gravados. Default `data`; caminho
  relativo resolve contra o **diretório do executável** (`config.cpp:98-105`),
  não contra o de trabalho nem contra o do `-c` — mover a config não move os
  dados. Guardado já absoluto e normalizado.
- **Não cria a pasta** — isso é do writer, Fase 3. Só falha se o caminho já
  existir e **não** for diretório (`config.cpp:107-113`), que é o erro que o
  `mkdir` daria bem mais tarde.
- Documentada no `iotrail.conf:11-19`, com a entrada comentada no arquivo pra
  valer o default.

**Os dois furos que o teste achou** (ambos herdados do código anterior):

1. **Duplicata invisível.** A checagem de "declarado mais de uma vez" olhava os
   brokers/streams **carregados com sucesso**. Com `[broker:b1]` duas vezes e a
   primeira falhando por outro motivo, a segunda entrava como se fosse única — a
   duplicata só apareceria no boot seguinte, depois de corrigir o primeiro erro.
   Agora a checagem é por nome **já visto** (`config.cpp:266-281`), independente
   de ter carregado.
2. **Mensagens fora da ordem do arquivo.** As duas passadas (brokers, depois o
   resto) faziam os erros de broker saírem antes dos de linha menor. A primeira
   passada agora só coleta nomes, sem validar nem logar (`config.cpp:256-261`), e
   toda a validação acontece na segunda, em ordem de arquivo. As streams
   continuam podendo citar um broker declarado abaixo delas.

**Validado:**

| caso | resultado |
|---|---|
| `iotrail.conf` real | 2 brokers, 3 streams, `data_dir` default `<exe>/data` |
| 12 problemas num arquivo só | todos numa passada, em ordem de linha, exit 1 |
| `[broker:b1]` repetido, primeiro inválido | os **dois** erros aparecem |
| `data_dir=dados_teste` | vira `<dir do exe>\dados_teste` |
| `data_dir` apontando pra um arquivo | fatal, com o caminho na mensagem |
| `client_id` de 31 caracteres | aviso, boot segue |
| broker sem stream | aviso, boot segue |
| `[coisa:nova]` | aviso, ignorada |
| `[casa]` sem tipo | fatal, sugerindo `[broker:casa]`/`[stream:casa]` |

O `main` (`main.cpp:31-52`) passou a carregar a config pelo `config::load` e a
resumir o que entendeu: brokers e streams em `debug`, tópicos em `trace`.

### 1.7 — Fechamento da fase — FECHADO (2026-09-05)

**Publicador de teste saiu do escopo** por decisão sua — entra na Fase 2, junto
de quem vai consumir dele.

**Validação ponta a ponta, com `build/` apagado antes:**

| o que | resultado |
|---|---|
| configure + build do zero | 8 alvos, **zero aviso** com `-Werror` |
| conteúdo do `build/` | `.exe` + 4 DLLs + `iotrail.conf`, sem intervenção |
| `objdump -p` no `.exe` | só as 4 DLLs copiadas + `api-ms-win-crt-*`/`KERNEL32` |
| rodar com o `PATH` **sem** MSYS2 | sobe normal — a cópia de DLL é o que sustenta |
| boot com config válida | 2 brokers, 3 streams, `data_dir` resolvido |
| boot com config inválida | todos os problemas em ordem de linha, exit 1 |
| config inexistente | `No such file or directory`, exit 1 |
| argumento inválido | erro + linha de uso, exit 1 |
| Ctrl+C | encerra em **111 ms**, com "IoTrail encerrando" no log |
| fechar a janela (`WM_CLOSE`) | encerra em **156 ms**, log completo |

**Tamanho do binário:** 3,79 MB como sai do build (`RelWithDebInfo`, com `-g`) e
**267 KB** depois de `strip` — ou seja, ~93% é informação de depuração, não
código. Vale saber antes de comparar com o número da 1.1/1.2 e achar que o
programa inchou.

**O que o teste de sinais custou** (scripts no scratchpad, fora do repositório):

- `GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0)` mata o **próprio script** que
  manda: `SetConsoleCtrlHandler(NULL, TRUE)` só ignora Ctrl+C, não Ctrl+Break.
- A flag "ignorar Ctrl+C" é **herdada na criação do processo**, e herdada ela
  vence o handler que o programa instala depois. Com o filho nascendo antes do
  ignore, o Ctrl+C chega e a parada ordenada acontece; com o filho nascendo
  depois, o evento simplesmente não é entregue — e parece bug do programa.
  Este é o tipo de armadilha que faz um teste "provar" o contrário do que ocorre.

**Âncoras dos docs revisadas.** Onze `arquivo:linha` do `DESIGN.md`/`TODO.md`
apontavam pra fora ou pro lugar errado, porque comentários foram enxugados nos
fontes depois que os registros foram escritos. Corrigidas. **Conferir isto é
parte do fechamento de fase**, não tarefa avulsa: registro que aponta pra linha
errada é pior que registro sem âncora.

**`DESIGN.md` atualizado:** §4 (o `data/` agora é o `data_dir` da config), §5
(duas camadas, régua fatal/aviso, `[general]`), §6 (ordem de boot com
`cmdline::parse`, verbosidade) e §7 (flags de build, cópia de DLL, lista de
módulos, PowerShell).

**Fase 1 fechada.** O que ela entrega: build reproduzível, logging assíncrono,
parada ordenada com o prazo do SO medido, linha de comando, e config lida,
validada e resumida no boot. O que ela deliberadamente não tem: nenhum byte de
MQTT, nenhum byte em disco.

---

## Fase 2 — Cliente MQTT

Desmembrada em 2026-09-05, no mesmo formato da Fase 1: eu apresento as decisões
em aberto → você decide → escrevo aquele pedaço → paro.

**Termina quando** o processo conecta em N brokers, subscreve o que a config
manda, recebe mensagens e as roteia pra stream certa, aguentando broker fora do
ar e queda no meio. **Fora desta fase:** fila em RAM, writer, formato de
registro, disco — o "sink" aqui é contador e log.

### Herdado da base de conhecimento — medido, não reabrir sem motivo novo

Da rodada anterior (`knowledge_base/iotrail_refactory/src/mqtt_client.cpp`, 273
linhas). São medições, não preferências:

- **`connect_async` + `loop_start`, não `connect` síncrono.** Com host
  inalcançável o síncrono ficava preso no timeout do SYN: **19 s de boot
  travado**.
- **~~O auto-reconnect da lib só cobre queda DEPOIS de uma conexão
  estabelecida; com o broker fora do ar no boot, a lib manda um CONNECT e nunca
  mais tenta.~~ REFUTADO na 2.2 (2026-09-06).** O que a lib faz com
  `connect_async` é ficar **cega por um keepalive** — ela não percebe o TCP que
  falhou e só declara a conexão morta quando esse timer estoura (medido: 60,004 s
  com keepalive 60; 10,014 s com keepalive 10). A partir daí ela **retenta
  sozinha**, e reconecta quando o broker volta. O texto do header
  (`mosquitto.h:1657`, "unexpectedly disconnected") descreve a política, não o
  limite que se supunha. Isso muda o propósito da supervisão da 2.3: encurtar a
  janela cega e dar visibilidade, não suprir uma ausência.
- **`stop()` tem que distinguir disconnect de force.** `loop_stop(force=false)`
  bloqueia até a thread da lib terminar, e ela só termina se o disconnect tiver
  funcionado. Medido lá: **~11 s de atraso** no encerramento com broker
  inalcançável. **A explicação que vinha junto — "quando o cliente nunca
  conectou, o disconnect devolve `MOSQ_ERR_NO_CONN`" — foi REFUTADA na 2.2**
  (libmosquitto 2.0.22 devolve `SUCCESS`), e com ela a regra de decidir o
  `force` pelo retorno do disconnect. O sintoma é real, a causa era outra: ver o
  registro da 2.2.
- **`mosquitto_strerror` devolve "Unknown error" para `MOSQ_ERR_ERRNO`**, que é o
  código de quase toda falha de rede. Sem o `errno` do sistema junto, "recusado",
  "host inalcançável" e "DNS falhou" viram a mesma mensagem inútil. O `errno` é
  preenchido também no Windows.
- **Ambiente conferido nesta máquina:** `libmosquitto.dll` e `libmosquittopp.dll`
  no ucrt64, `libmosquitto.pc` no pkgconfig (não há pacote CMake), e
  `mosquitto.exe` + `mosquitto_pub/sub` já em `tools/` — dá pra testar com broker
  local, sem depender da rede.

### Decidido no planejamento (2026-09-05)

- **API C (`libmosquitto`)**, não o wrapper `libmosquittopp`: ele não expõe o
  `mosquitto*` interno, o que fecharia a porta pro MQTT 5 (`DESIGN.md:188-190`).
- **Os clientes vivem no `main`** por enquanto — sem supervisor.
- **O `tick()`, se entrar** (a premissa dele foi revista na 2.2 — ver 2.3),
  **pendura no laço de 200 ms que já existe** (`main.cpp:82-84`), sem
  thread nem timer novos. O ritmo real é o backoff, não o laço; os 200 ms só dão
  a granularidade do disparo, irrelevante contra 1 s. Diminuir gastaria CPU à toa
  (o alvo é edge); aumentar sairia do orçamento de encerramento da 1.3.
  **`steady_clock`, nunca `system_clock`** — ajuste de NTP moveria o próximo
  retry pra frente ou pra trás.
- **Backoff de 1 s a 60 s nos DOIS lados:** o nosso, da primeira conexão, e o da
  lib (`mosquitto_reconnect_delay_set`, que sai 30 s por padrão). Diferentes, o
  comportamento mudaria conforme o cliente já tivesse conectado alguma vez — o
  tipo de coisa que faz procurar problema no lugar errado.
- **SUBACK recusado (`0x80`) é aviso, não fatal.**
- **Fan-out: a mensagem vai para TODAS as streams cujo padrão casa**, não para a
  primeira. Com "primeira que casa", a ordem de declaração viraria roteamento
  invisível e uma stream pararia de receber calada; declarar duas streams
  sobrepostas (uma `#` de arquivo e uma específica) é escolha de quem
  configurou, e cada uma tem sua retenção e seu offset.
- **Tópico sem stream: aviso na primeira ocorrência (com o tópico) + contador
  reportado no encerramento** — e o enquadramento aqui **diverge da rodada
  anterior**, que tratou isso como "sensor não mapeado" e adiou a decisão com
  medo de inundar o log. Como as subscrições são exatamente a união dos padrões
  das streams (2.4), toda mensagem entregue casou com um padrão nosso, logo casa
  com alguma stream: cair aqui significa que o nosso matcher e o do broker
  discordam, ou seja, **bug nosso**, não erro de config. Custo: ~6 linhas, zero
  no caminho quente (o laço do fan-out já sabe se alguma casou), ~16 bytes por
  cliente, no máximo duas linhas de log por processo — e continua duas linhas se
  o raciocínio acima estiver errado e a coisa acontecer em volume.

### 2.1 — A lib entra no build — FECHADO (2026-09-06)

`src/mqtt/mqtt.h`/`.cpp` (só `init()`/`shutdown()`) mais a descoberta, o link e a
cópia de DLL no `CMakeLists.txt`. **libmosquitto 2.0.22**, do pacman.

**Decisões tomadas:**

- **pkg-config, não `find_library`** (`CMakeLists.txt:52-62`). Não há pacote
  CMake no MSYS2, só `libmosquitto.pc`; `pkg_check_modules(... IMPORTED_TARGET)`
  entrega include e link resolvidos e ainda dá a versão em tempo de configure,
  que aparece no `-- libmosquitto 2.0.22` do log do CMake. `find_library` exigiria
  nome de lib e include path escritos à mão.
- **`PKG_CONFIG_PATH` vem da variável de cache `IOTRAIL_MSYS2_UCRT64`**
  (`CMakeLists.txt:59`), não do ambiente: senão o resultado do configure depende
  do `PATH` de quem chamou, que é o tipo de dependência invisível que a 1.1
  evitou em todo o resto.
- **`mosquitto_lib_init()` depois da config** (`main.cpp:55-59`), não junto do
  logging. No Windows ele chama `WSAStartup`: não vale subir a pilha de rede num
  processo que sai por config inválida três linhas depois. O `cleanup` fica no
  encerramento (`main.cpp:69`), antes do `logging::shutdown()`.
- **Versão da lib em `debug`** (`mqtt.cpp:19`), não `info` — quem opera não
  precisa, quem depura precisa.
- **`src/mqtt/` como subpasta** já nesta tarefa: `client.*` entra ao lado na 2.2,
  e são dois pares `.h`/`.cpp` no mesmo assunto, que é o critério da 1.1.

**O que o probe mediu — e o que ele custou em MB:**

- **`PkgConfig::MOSQUITTO` não tem `IMPORTED_LOCATION`**, então
  `TARGET_RUNTIME_DLLS` volta **vazio** pra ele. A suspeita do planejamento se
  confirmou: a DLL não vem de graça, vai na lista manual
  (`CMakeLists.txt:126-145`).
- **A `libmosquitto.dll` (0,14 MB) importa `libssl-3-x64.dll` (0,95 MB) e
  `libcrypto-3-x64.dll` (5,24 MB)** — `objdump -p` na própria DLL —, **mesmo sem
  TLS nenhum**. E como o `TARGET_RUNTIME_DLLS` não varre imports de DLL, essas
  duas seriam manuais mesmo que o alvo do pkg-config fosse completo.
- **+6,3 MB no diretório de deploy** num projeto que se vende como leve, sem
  escapatória barata: o pacote do MSYS2 não tem variante sem TLS, e linkar
  estático (`libmosquitto.a`, 0,19 MB) só trocaria isso por `libcrypto.a` de
  9,3 MB dentro do `.exe`, contrariando o runtime dinâmico da 1.1.
- **Armadilha registrada no `CMakeLists.txt:135-137`:** o "3" de
  `libcrypto-3-x64.dll` é o major do OpenSSL — versão em texto exatamente do tipo
  que a lista do `TARGET_RUNTIME_DLLS` existe pra evitar. Se o MSYS2 subir pro 4,
  o build passa e o erro só aparece no boot, como DLL faltando.

**Validado:**

| caso | resultado |
|---|---|
| build | 9 alvos, **zero aviso** com `-Werror` |
| `build/` | 7 DLLs, **9,56 MB** (era 3,25 MB antes da lib) + `.exe` de 3,76 MB |
| `objdump -p` no `.exe` | ganhou `libmosquitto.dll`, nada mais |
| rodar com `PATH` **sem** MSYS2 | sobe e loga `libmosquitto 2.0.22` |
| config inválida | sai antes do `lib_init` — nenhum `WSAStartup` |

### 2.2 — Um cliente por broker: conectar — FECHADO (2026-09-06)

`src/mqtt/client.h`/`.cpp`: `mqtt::client`, um por broker, com `start()` e
`stop()`. Subscrição (2.4) e roteamento (2.5) ainda não entram — este cliente
conecta e fica conectado.

**Decisões tomadas:**

- **`class client` não-copiável, guardada em `vector<unique_ptr>`**
  (`main.cpp:66-68`). O `this` é registrado como userdata da lib e os callbacks
  voltam por ele: se o vector realocasse, o endereço mudaria debaixo da thread
  de rede. O construtor recebe **cópia** do `config::broker` — host, porta e
  client_id ficam autossuficientes.
- **`keepalive` constante de 60 s** (`client.cpp:13`), não chave de config.
  Vira chave quando aparecer rede que precise (NAT agressivo derrubando conexão
  ociosa); adiar custa mexer em três lugares depois — lista de chaves conhecidas
  do `config.cpp:119`, `iotrail.conf` e registro.
- **`clean_session=true`** (`client.cpp:41`), amarrado ao QoS 0 da 2.4: sem QoS
  > 0 não há sessão a guardar. Os dois mudam juntos se um dia mudarem.
- **Backoff da lib 1 s → 60 s** (`client.cpp:14-15,55`), igual ao que a
  supervisão da 2.3 vai usar.
- **Falha no `start()` derruba o boot** (`main.cpp:69-73`): `mosquitto_new` ou
  `loop_start` falhando é falta de recurso local, não broker fora do ar — este
  nem aparece aqui, porque o `connect_async` devolve sucesso com o host morto.
- **`on_log` mapeado por nível** (`client.cpp:236-250`): `MOSQ_LOG_ERR` e
  `WARNING` viram nosso `warn`; `MOSQ_LOG_DEBUG` vira **`trace`**, não `debug`,
  porque a lib loga cada PINGREQ/PINGRESP e a cada 60 s isso poluiria o `-v` de
  quem está depurando outra coisa.
- **Prefixo `[mqtt/<broker>]`** em toda linha, como o `[config]` da Fase 1. Com
  N clientes em N threads, sem o nome do broker o log vira adivinhação.
- **`error_text()`** (`client.cpp:21-27`): `mosquitto_strerror` mais o `errno`
  quando o código é `MOSQ_ERR_ERRNO`, senão "recusado", "host inalcançável" e
  "DNS falhou" saem com a mesma mensagem inútil.
- **O `stop()` foi puxado da 2.6 pra cá**, decidido na conversa: sem ele a 2.2
  não é validável — o processo não sairia, e o travamento apareceria como
  origem misteriosa. A 2.6 fica com a ordem do encerramento com N clientes e o
  orçamento do handler.

**O achado desta tarefa — o registro herdado estava errado, e do jeito pior:**

A rodada anterior mandava decidir o `force` do `loop_stop` **pelo retorno do
`mosquitto_disconnect`**, com a justificativa de que um cliente que nunca
conectou receberia `MOSQ_ERR_NO_CONN`. **Medido na 2.0.22 com `connect_async`:
o disconnect devolve `MOSQ_ERR_SUCCESS` mesmo sem nunca ter havido conexão.**
Seguindo o retorno, o `loop_stop` entrava sem `force` e ficava preso no join de
uma thread parada dentro do `connect()` do SO — com host que engole SYN
(192.0.2.1), o Ctrl+C **não encerrava em 5 s** e o fechar-janela morria no teto
de 3 s do handler da 1.3. Com `force`, **112 ms**.

Então quem decide é a **nossa** flag, e é o "conectado **agora**"
(`client.cpp:81`), não o "já conectou alguma vez": um cliente que conectou e
está no meio de uma retentativa tem a thread no mesmo `connect()` bloqueante.
Corrida aceita e anotada: se a conexão subir entre o `load()` e o `loop_stop`,
o broker vê um TCP fechado na marra em vez de um DISCONNECT — irrelevante num
processo que está saindo. E `force` é `pthread_cancel`: só vale porque isto roda
na saída; restart em execução (se algum dia existir) precisa de outra saída.

**O outro achado, e ele foi corrigido duas vezes — a versão final é esta:**
falha de conexão inicial fica **cega por exatamente um keepalive**, e depois a
própria lib passa a retentar.

Cronologia medida, broker desligado e religado no meio:

```
12:46:52.914  conectando em 127.0.0.1:1883     CONNECT enviado, TCP recusado, silêncio
12:47:52.918  [warning] conexao inicial falhou  60,004 s depois
12:48:12.982  conectado em 127.0.0.1:1883       broker voltou; a LIB reconectou sozinha
```

Os 60 s são o keepalive, não coincidência: refazendo com `keepalive_s = 10`, o
aviso saiu em **10,014 s**. Com `connect_async` a lib não percebe o TCP que
falhou; ela só declara a conexão morta quando o timer de keepalive estoura, e é
aí que chama `on_disconnect` (nossa linha `client.cpp:232`) e entra no ciclo de
reconexão. As tentativas seguintes são invisíveis — não há callback por
tentativa, e o `sending CONNECT` do log da lib só sai quando o TCP conecta —
mas estão acontecendo, tanto que a conexão subiu no instante em que o broker
voltou.

**Duas correções que isso obriga:**

1. **Minhas primeiras medições estavam curtas.** Janelas de 3 a 12 s, contra um
   sintoma que aparece em 60 — daí eu ter registrado "silêncio total" e "a lib
   não retenta". Achado do André, testando com o broker real e paciência maior.
2. **O registro herdado ("a lib manda um CONNECT e nunca mais tenta") está
   errado**, e provavelmente pelo mesmo motivo. Ver a correção na seção herdada
   da Fase 2.

**O que sobra pro `tick()` da 2.3** — e é diferente do que o plano dizia. Não é
mais "a lib não cobre a primeira conexão"; é:

- **encurtar a janela cega**, que hoje é o keepalive inteiro (60 s por padrão) e
  só existe na primeira conexão;
- **dar visibilidade**, porque hoje não há uma linha sequer entre a tentativa e
  a desistência, nem durante as retentativas da lib.

E abre uma alternativa que não existia no plano: **baixar o keepalive** encurta
a janela sem código nenhum — ao custo de PINGREQ mais frequente, o que em edge é
tráfego e energia. Comparar as duas saídas é decisão da 2.3.

**Validado** (broker local `tools/mosquitto.exe` 1.6.3, e `192.0.2.1` como host
que engole SYN):

| caso | resultado |
|---|---|
| broker no ar | `conectado em 127.0.0.1:1883`; broker vê `iotrail-local (p2, c1, k60)` |
| encerramento com conexão | DISCONNECT limpo no log do broker; Ctrl+C 113 ms, fechar janela 63 ms |
| broker inalcançável | boot não trava; Ctrl+C 112 ms, fechar janela 87 ms |
| dois brokers, um morto | os dois clientes sobem, o vivo conecta, encerra em 131 ms |
| porta fechada | conexão recusada **sem nenhuma linha de erro** (ver acima) |
| build | zero aviso com `-Werror` |

### 2.3 — Reconexão e supervisão — FECHADO (2026-09-06), **sem `tick`**

A premissa da tarefa caiu na 2.2: a lib **retenta a primeira conexão sozinha**.
O que restava era a janela cega de um keepalive inteiro antes de ela começar. A
decisão foi resolver isso **sem código de supervisão**.

**Decisões tomadas:**

- **Nenhum `tick`, nenhuma retentativa nossa.** Reconexão é 100% da lib. Some
  junto o risco que o `tick` planejado carregava: `mosquitto_reconnect_async`
  chama `getaddrinfo`, que é síncrono — com DNS inacessível ele travaria a
  thread do `main`, que é a mesma que checa a parada a cada 200 ms.
- **`keepalive` vira chave, default 30 s** (`config.h:25`). Cortar de 60 pra 30
  corta a janela cega pela metade sem escrever linha nenhuma de supervisão, e
  melhora também o regime normal: o keepalive é o que detecta conexão morta em
  silêncio (cabo arrancado, NAT expirado, Wi-Fi que caiu sem FIN). Preço: um par
  PINGREQ/PINGRESP a cada 30 s por broker em vez de 60 — bytes irrelevantes, mas
  o dobro de *acordares*, que é o que importaria em bateria ou link celular.
- **Chave em `[broker:*]`, não em `[general]`** (`config.cpp:149-164`). Entrou
  primeiro em `[general]` e foi movida no mesmo dia, a pedido do André: o
  keepalive é parâmetro **da conexão**, negociado em cada CONNECT, e dois
  brokers em links diferentes (LAN e celular, por exemplo) querem valores
  diferentes. Custo de ter movido: com N brokers iguais, o valor se repete N
  vezes. Se isso incomodar, o caminho é `[general]` virar o default de quem não
  declara — sem quebrar arquivo nenhum.
- **Faixa 0–65535**, que é a do campo de 16 bits do MQTT 3.1.1. Fora dela é
  fatal; **`keepalive=0` é aviso**, não erro: é legal na especificação (desliga
  o PINGREQ), mas desliga junto a detecção de conexão morta, e isso tem que
  aparecer no log de quem escolheu.
- **O `parse_port` virou `parse_int(text, min, max, out)`** (`config.cpp:19-29`),
  usado pela porta (1–65535) e pelo keepalive (0–65535). Mesma proteção contra o
  `atoi` que aceita `"1883x"`.

**O que fica em aberto de propósito:** durante a janela cega — e durante uma
queda longa — **não há uma linha sequer no log**. São duas linhas nas pontas
(caiu / voltou) e silêncio no meio. Uma supervisão só de log (sem reconectar)
resolveria, e é barata; ficou de fora por ora. Reabrir se, em uso, a falta
incomodar.

**Validado:**

| caso | resultado |
|---|---|
| sem a chave | aviso de falha em **30,002 s** (default aplicado) |
| `keepalive=10` | aviso em **10,003 s** — a chave chega ao cliente |
| `keepalive=abc` | fatal, "esperado 0-65535 segundos", exit 1 |
| `keepalive=99999` | fatal (fora dos 16 bits do campo) |
| `keepalive=0` | aviso de que desliga a detecção, boot segue |
| dois brokers, 10 e default | avisos em **10,015 s** e **30,007 s**, no mesmo processo |

**Saiu junto: a cópia do `iotrail.conf` estava ficando velha em silêncio.** Ela
era `POST_BUILD` do alvo `iotrail`, e `POST_BUILD` só roda quando o alvo
**relinca** — editar apenas o `.conf` nunca chegava ao `build/`. Descoberto ao
vivo: depois de reescrever o cabeçalho do arquivo, o `cmake --build` respondeu
`ninja: no work to do` e o programa continuou lendo a versão antiga. Agora é
`configure_file(... COPYONLY)` (`CMakeLists.txt:165`), que registra o arquivo
como dependência de configure: mudou, o próximo build reconfigura e recopia.
Verificado nos dois sentidos, inserindo e removendo um marcador. (Primeira
tentativa foi um alvo próprio com `add_custom_command`, que não compila:
`OUTPUT` não aceita `$<TARGET_FILE_DIR:...>`.)

### 2.4 — Subscrição e QoS — FECHADO (2026-09-06)

O cliente passa a pedir tópicos ao broker. Entrega de mensagem ainda não: o
`on_message` é a 2.5.

**Decisões tomadas:**

- **O cliente recebe as streams do seu broker** (`client.h:24`,
  `main.cpp:67-71`): `vector<const config::stream*>` apontando pra dentro do
  `settings`, que é declarado antes dos clientes no `main` e destruído depois
  deles. Ponteiro e não cópia porque a 2.5 vai precisar da identidade da stream,
  não só dos padrões — e porque ninguém muda esses dados depois do boot.
- **SUBSCRIBE dentro do `on_connect`** (`client.cpp:111-146`), não no `start()`:
  com `clean_session=true` o broker esquece as inscrições a cada queda, então
  toda reconexão precisa refazê-las. Verificado derrubando e subindo o broker:
  as três inscrições saem de novo sozinhas.
- **Um SUBSCRIBE por padrão**, não `subscribe_multiple`. O SUBACK volta com o
  `mid`, e um mapa `mid → padrão` (`client.h:48`) permite dizer **qual**
  inscrição o broker recusou. Com `subscribe_multiple` a mensagem seria "o
  broker recusou uma inscrição", sem dizer qual — o tipo de log inútil que o
  projeto vem evitando. Custo: N pacotes por conexão, uma vez.
- **Dedup dos padrões** (`client.cpp:119-129`): duas streams do mesmo broker
  podem declarar o mesmo tópico. Medido com 4 padrões declarados → 3 SUBSCRIBEs.
- **QoS 0, como constante** (`client.cpp:14`), sem chave de config. Discutido e
  adiado de propósito: com QoS 1 a lib manda o PUBACK sozinha quando o callback
  retorna, ou seja, **confirmaríamos a entrega de mensagens que jogamos fora** —
  pior que QoS 0, que não promete nada. E o QoS não viaja sozinho: pra valer
  precisa de `clean_session=false`, `client_id` estável e disco atrás. Entra como
  pacote na Fase 3/4, e provavelmente **por stream**, que é a granularidade do
  MQTT (o QoS é pedido por filtro no SUBSCRIBE), com default por broker se fizer
  falta.
- **O `mid → padrão` não tem lock** (`client.h:43-45`): `on_connect` e
  `on_subscribe` rodam na mesma thread da lib, uma por cliente.

**Bug encontrado no próprio teste:** o resumo contava os padrões *tentados*, não
os aceitos — com um padrão inválido, o log dizia "1 inscrição pedida" quando
nenhuma tinha saído. Agora são duas variáveis (`client.cpp:119-120`): a lista de
dedup, que inclui os que falharam, e o contador, que só sobe quando a lib aceita
enviar.

**Validado** (broker local `tools/mosquitto.exe`):

| caso | resultado |
|---|---|
| 4 padrões em 2 streams, um repetido | 3 SUBSCRIBEs; o broker registra 3 |
| publicação em `teste/x` | broker entrega ao cliente (que descarta — 2.5) |
| broker cai e volta | as 3 inscrições são refeitas na reconexão |
| padrão inválido `a/#/b` | rejeitado pela lib com o padrão no log; contagem 0 |
| **SUBACK `0x80`** | **sem teste** — o mosquitto 1.6.3 do `tools/` concede a inscrição mesmo com ACL negando (`iotrail-local 0 proibido/x` no log dele) e filtra só na entrega. Reproduzir precisaria de broker 2.x |

### 2.5 — Roteamento tópico → stream — FECHADO (2026-09-06)

`client::on_message` (`client.cpp:167-218`): a mensagem que chega vira "tópico X
→ stream Y". Contadores por stream e sink de verdade continuam na 2.6.

**Decisões tomadas:**

- **O roteamento mora no cliente**, na lista imutável `streams_` que a 2.4 já
  trouxe. A pergunta que estava aberta se fechou por construção: nada é
  compartilhado entre as N threads da lib, então não há lock no caminho da
  mensagem.
- **Callback concreta, sem interface de sink** — mantida a recomendação do
  planejamento. A assinatura certa depende do que o writer da Fase 3 vai querer
  (registro pronto? payload cru? slot pré-alocado?), e desfazer abstração errada
  custa mais que trocar o corpo de uma função.
- **Timestamp carimbado na chegada** (`client.cpp:170-172`), `system_clock` em
  ms, já nesta fase — mesmo sem ninguém consumir. Se ficasse pra Fase 3, o risco
  era alguém carimbar no writer, e aí o valor passaria a incluir o tempo de fila,
  contrariando o `DESIGN.md` §3 sem que nada acusasse.
- **`mosquitto_topic_matches_sub` da lib** (`client.cpp:194`), não matcher
  próprio: `+` no meio do nível, `#` só no fim e o fato de `#` não casar `$SYS`
  são cantos onde implementação à mão erra.
- **Fan-out para todas as streams que casam** (`client.cpp:188-206`), com um
  `break` no primeiro padrão que casa **dentro** de cada stream — um padrão já
  resolve a stream, os outros dela não mudam nada.
- **O payload não é impresso.** A linha de `trace` traz tópico, stream, tamanho
  em bytes e o timestamp de chegada — só metadado. Chegou a existir um preview
  truncado e sanitizado (64 bytes, byte fora de `0x20..0x7e` virando `.`), que o
  André pediu e depois retirou. O motivo de ele ser sanitizado, se um dia voltar:
  payload é opaco, então um dia vem binário, e **escape ANSI vindo do payload
  reconfigura o terminal de quem está lendo o log** — verificado na época
  publicando `ESC[31mX`, que saía como `a.[31mX`.
- **Tópico sem stream: aviso na primeira ocorrência + total no encerramento**
  (`client.cpp:208-217` e `client.cpp:87-92`). O contador é atômico porque é
  escrito na thread da lib e lido no `stop()`; a leitura acontece depois do join,
  mas o atômico dispensa raciocinar sobre isso.

**O caso "sem stream" deixou de ser hipotético.** No planejamento eu argumentei
que ele indica bug nosso, já que as inscrições são a união dos padrões das
streams. Consegui reproduzir de propósito com **subscrição compartilhada**:
`topics=$share/g1/casa/#` faz o broker entregar `casa/temp`, enquanto o padrão
guardado é a string `$share/...` — o matcher compara os dois e não casa. Ou seja,
o cenário existe de verdade e vale a pena ter o aviso. (Suporte a `$share` não é
decisão desta fase; virou caso de teste.)

**Validado** (broker local, publicando com `tools/mosquitto_pub.exe`):

| caso | resultado |
|---|---|
| `casa/temp` com streams `casa/#` e `casa/temp` | **duas** linhas, uma por stream |
| `casa/umidade` | só a stream `casa/#` |
| payload com TAB e ESC | testado na versão com preview, hoje removida |
| `$share/g1/casa/#` | aviso na 1ª mensagem, silêncio nas seguintes |
| encerramento após 3 órfãs | `3 mensagem(ns) sem stream no total`, antes do "encerrado" |

### 2.6 — Sink temporário e parada — FECHADO (2026-09-06), **sem código**

A tarefa esvaziou: o que ela previa ou já tinha saído antes, ou não se justifica.

- **O "sink de print" já existe.** O ROADMAP dizia que nesta fase o sink podia
  ser só um print, e a linha de `trace` da 2.5 (`tópico → stream, bytes, ts`) é
  exatamente isso. Não falta sink; falta writer, que é Fase 3.
- **O contador por stream foi descartado** — decisão do André, e o argumento é
  bom: ele morreria na Fase 3 **e nasceria duplicado**. Cada stream vai ter seu
  **offset** (`DESIGN.md:61-63`), monotônico, começando em 0 e incrementando de 1
  por registro. O offset *é* a contagem, e ainda por cima é persistente e
  sobrevive a reinício. Um contador em RAM ao lado seria uma segunda fonte para o
  mesmo número — o mesmo erro que a 1.6 evitou ao não derivar `topics` por broker
  na config. Sem contador, a pergunta "com que periodicidade ele aparece" some
  junto.
- **`stop()` e ordem de encerramento já estavam prontos:** o `stop()` com a
  regra de `force` foi puxado pra 2.2 (com o achado do `disconnect` que devolve
  SUCCESS mesmo sem conexão), e a ordem — parar e destruir os clientes antes do
  `lib_cleanup` — está no `main.cpp:93-94` desde então.

**O que sobrou de concreto:** um comentário no `on_message` (`client.cpp:174-178`)
registrando que `msg` e o payload pertencem à lib e morrem quando a callback
retorna — nesta fase ninguém guarda nada, mas o `push` da Fase 3 **tem** que
copiar, senão a writer thread lê memória que a lib já reaproveitou. É o tipo de
regra que, se não estiver escrita ao lado do código, vira ponteiro solto três
fases depois.

**A medição foi pra 2.7:** cronometrar o encerramento com clientes conectados e
tráfego, como linha de base pro dia em que houver fila e `fsync` dentro do mesmo
orçamento do handler (~5 s do SO menos os 200 ms do laço).

### 2.7 — Fechamento da fase — FECHADO (2026-09-06)

**Publicador de teste:** `tools/mosquitto.exe` subindo brokers locais em 1883 e
1884, e `tools/mosquitto_pub.exe` publicando — tudo sem depender da rede nem do
broker de casa. Foi o que permitiu testar duas conexões simultâneas, queda e
volta de broker, e tráfego durante o encerramento.

**Build do zero** (`build/` apagado antes): 10 alvos, **zero aviso** com
`-Werror`. O `objdump -p` no `.exe` mostra só as 6 DLLs esperadas; `build/` fecha
em **9,56 MB de DLL** mais um `.exe` de 4,58 MB (com `-g`; ~93% disso é
informação de depuração, medido na 1.7).

**Validação ponta a ponta**, com dois brokers e três streams (uma delas com
padrão sobreposto a outra):

| caso | resultado |
|---|---|
| dois brokers no ar | ambos conectam; 2 inscrições num, 1 no outro |
| 60 mensagens durante a execução | roteadas: `temperatura` 30, `temp_exata` 30, `vibracao` 30 |
| fan-out sob tráfego | as 30 de `casa/temp` entraram nas **duas** streams que casam |
| broker cai no meio | `conexao perdida - a lib reconecta` |
| broker volta | reconecta em **7 s** (backoff) e **refaz as 2 inscrições** |
| mensagem depois da volta | roteada de novo — a re-subscrição funcionou de fato, não só no log |
| broker fora do ar o tempo todo | `conexao inicial falhou` em **30,011 s** = o keepalive default |
| Ctrl+C com 2 clientes conectados | encerra em **172 ms** |
| fechar a janela com 2 conectados | encerra em **22 ms** |

**A medição que a 2.6 mandou pra cá:** com 2 clientes conectados e 60 mensagens
trafegadas, o `clients.clear()` — parar e destruir os dois clientes — roda em
**menos de 1 ms**: no log, `IoTrail encerrando` e os dois `encerrado` saem no
mesmo milissegundo. O total de 172 ms do Ctrl+C é quase inteiramente a latência
do polling de 200 ms do `main`, não trabalho de encerramento.

Isso é **linha de base, não conforto**: hoje não há nada pra drenar. Na Fase 3 o
mesmo ponto passa a ter M filas pra esvaziar e M `fsync` pra fazer, dentro do
mesmo orçamento do handler de console (~5 s do SO menos os 200 ms do laço,
medidos na 1.3). O número de hoje é contra o que comparar.

**Âncoras revisadas:** 14 `arquivo:linha` do `TODO.md` tinham andado — o
`client.cpp` cresceu ~50 linhas entre a 2.4 e a 2.6, e edições de comentário
deslocaram o resto. Conferidas uma a uma contra o conteúdo, não só contra o
tamanho do arquivo.

**Fase 2 fechada.** O que ela entrega: N clientes MQTT, um por broker, com
conexão assíncrona, reconexão da lib com backoff, subscrição da união dos padrões
das streams refeita a cada reconexão, roteamento tópico → stream com fan-out, e
encerramento ordenado com o binário rodando sem o MSYS2 no `PATH`. O que ela
deliberadamente não tem: **nenhum byte em disco** — a mensagem roteada vira uma
linha de log e é descartada.

**Dívidas registradas, que a Fase 3 herda:**

- O `push` na fila **tem que copiar** o payload (`client.cpp:174-178`).
- QoS, `clean_session` e sessão persistente entram juntos, e provavelmente por
  stream (2.4).
- Não há linha de log durante a janela cega nem durante queda longa — só nas
  pontas (2.3).
- SUBACK `0x80` continua **sem teste**: o broker do `tools/` é 1.6.3 e não recusa
  inscrição por ACL (2.4).

---

## Fase 3 — Formato de registro e writer

Desmembrada em 2026-09-06, mesmo formato das anteriores: eu apresento as
decisões em aberto → você decide → escrevo aquele pedaço → paro.

**A fronteira com a Fase 4 foi movida** (decidido no planejamento). O ROADMAP
separava "formato" de "escrita em segmentos + rollover + durabilidade"; agora:

- **Fase 3** termina quando *mensagem recebida vira byte durável e
  recuperável no disco*: formato, codificação, camada de plataforma, fila,
  writer thread, `fsync` e recuperação no boot.
- **Fase 4** fica com **rollover e ciclo de vida do segmento**, que é o assunto
  de que a Fase 5 (índice) e a Fase 8 (retenção) dependem.

O motivo: o formato **não se valida sem escrever e ler de verdade**, e um writer
sem `fsync` nem recuperação não é testável como durável — seria fechar a fase
mais cara do projeto sem evidência. O preço é uma fase maior, daí o número de
tarefas.

**Fora desta fase, de propósito:** rollover (4), índice (5), replay (6).

**Desmembrada de novo em 2026-09-06**, depois da 3.1: a tarefa do `.meta` entrou
como 3.4 e empurrou as seguintes. Motivo: o `.meta` é a menor fatia vertical do
sistema — grava, sincroniza, trunca e recupera — e prova CRC, plataforma e
recuperação antes de essas peças irem para o caminho quente do segmento. Dez
tarefas, mesma fronteira com a Fase 4.

### Herdado da base de conhecimento — não redecidir sem motivo

- **A especificação já existe e está madura.** Migrada e revista na 3.1: agora é
  `docs/FORMATO.md` (`format_version = 1`), e o original
  `knowledge_base/docs/formato_segmento.md` não é mais consultado. Header de
  segmento de 14 bytes, registro de 28 bytes fixos + payload, tópicos no `.meta`
  da stream, CRC-32 IEEE cobrindo do byte 4 ao fim, little-endian, sem padding,
  mais os algoritmos de recuperação (§5 e §8) e um **leitor de referência em
  Python** (§10).
- **`write()` é barato; o timer que importa é o do `fsync`**
  (`knowledge_base/docs/decisao_sync_write.txt`). Batelar escrita não compra
  throughput na escala do IoTrail — a fila não existe por desempenho, existe por
  **isolamento**: se uma escrita travar, quem recebe do broker não trava junto.
- **Os dois timers se dimensionam por coisas diferentes:** `write_interval` pelo
  intervalo do sensor mais rápido (menor que isso é só acordar à toa);
  `sync_interval` pela janela de perda aceitável numa queda de energia — cada
  `fsync` custa o mesmo independente de quantos registros acumularam.
- **Polling tem custo no encerramento:** com laço por `sleep`, o writer pode
  levar até ~2× `write_interval` para sair. Foi medido na rodada anterior e
  compete diretamente com o orçamento do handler de console (1.3).
- **Prototipado em `knowledge_base/src/test_0..test_2`:** escrita direta, fila +
  thread escritora com `write_interval`, e `sync_interval` independente com sync
  final no encerramento.
- **Writer da rodada anterior:** `knowledge_base/src/segment_writer.{h,cpp}`
  (324 linhas) — fila `deque` + mutex, uma thread, recuperação no construtor,
  registro montado em buffer e gravado num `fwrite` só.

### 3.1 — O formato: revisão e migração — FECHADO (2026-09-06), **sem código**

A spec virou `docs/FORMATO.md`, revista ponto a ponto antes de virar código. As
âncoras para o código da rodada anterior foram trocadas pelas do projeto novo
(`src/config/config.cpp:63` e `:72` para a validação de nome, `:330-336` para o
`data_dir`, `src/mqtt/client.cpp:170` para o `arrived_ms`), e os nomes de
constantes perderam o prefixo `k`. `docs/DESIGN.md` §4 aponta para o doc novo.

As decisões abaixo são quase todas de política de escrita e de recuperação — o
que valeu a conversa foi o §8, que tinha um caminho de perda total de dados. A
exceção é a última, que mudou o registro: o tópico saiu dele.

- **`format_version` continua 1.** Nada nunca foi gravado por este binário, então
  não existe arquivo v1 que precise continuar legível — mudança agora custa zero,
  e é por isso que a revisão aconteceu antes do writer e não depois.
- **Limites em duas categorias.** Do formato (constantes, só mudam com bump):
  `max_topic_len` 1024, `payload_hard_max` 1 MiB. Do writer (config `[general]`):
  `max_payload_len` 64 KiB, `segment_max_bytes` 8 MiB. **Juntar os dois papéis
  num valor configurável abria perda de dados:** sobe-se a chave, gravam-se
  registros grandes, baixa-se de volta, e a varredura chama registro legítimo de
  corrupção e trunca o segmento. Coerência entre as duas, validada no boot:
  `28 + max_payload_len <= segment_max_bytes`.
- **`segment_max_bytes` 8 MiB, não 64 MiB.** Segmento ativo não é apagável pela
  retenção; a 64 MiB uma stream lenta segura o mesmo arquivo aberto por mais de
  um ano. Rollover por idade fica marcado para a Fase 4 — é a solução de verdade.
- **`max_payload_len` 64 KiB**, e não 1 KiB, porque o limite é de rede MQTT e não
  de sensor: `bridge/devices` do zigbee2mqtt, discovery do Home Assistant e
  ESP32-CAM passam de 1 KiB com folga. Errar para cima custa RAM que ninguém usa;
  errar para baixo custa histórico que não volta. Acima do limite: descarta,
  `warn` com tópico/tamanho/teto, contador por stream, checado no `on_message`
  antes do fan-out.
- **`t_len == 0` é corrupção; `payload_len == 0` é gravado** (payload vazio é
  mensagem MQTT legítima — é como se apaga um retained).
- **O primeiro registro válido é a autoridade sobre o `base_offset`**, não o
  contrário. O `offset` do registro é coberto pelo CRC; os 8 bytes do header não
  são cobertos por nada. Na spec antiga, `base_offset` corrompido reprovava o
  primeiro registro, a varredura parava em `pos = 14` e **o segmento inteiro era
  truncado para o header**. Divergência agora é `warn` + header reescrito e
  sincronizado. `header_crc32` continua fora: ele detectaria sem resolver, e a
  resposta seria a mesma — perguntar ao primeiro registro.
- **Truncar só em falha de tamanho.** Falha de conteúdo (corpo completo, CRC ruim
  ou campos absurdos) preserva o arquivo como `.corrupt` e **para aquela stream**,
  as outras seguem. Abrir segmento novo reusaria os offsets que estão dentro do
  `.corrupt`, e a Fase 7 chaveia cursor de consumidor por offset — um consumidor
  parado em 500 leria outro registro sem nenhum sinal.
- **Fan-out documentado** no `iotrail.conf`, comentário de `topics`: streams com
  padrões sobrepostos gravam a mensagem uma vez cada.
- **O tópico saiu do registro** (decidido depois dos seis pontos, mesma data).
  Era 37% do registro na medição de 2026-08-29 (`topic_len` 19 contra
  `payload_len` 7) e se repetia em toda mensagem. Agora mora em
  `data/<stream>/<stream>.meta` e o registro guarda `topic_id`: de
  `26 + topic + payload` para `28 + payload`, -17% no caso do exemplo do doc.
  O `.meta` tem header próprio (`magic "IOTM"`, `format_version`, `header_len`,
  `next_topic_id`) seguido da tabela de tópicos append-only. A ideia estava em
  `knowledge_base/claude_memory/TODO.md:383-437` como candidata ao
  `format_version` 2 — entrou na v1 porque nada foi gravado ainda.
  **`topic_id` é explícito, `uint32` e nunca reusado**, senão uma limpeza que
  remova entradas faria registro antigo apontar para outro tópico; `header_len`
  é o que permite a v2 acrescentar campos de stream sem quebrar leitor antigo.
  Broker e padrões subscritos ficaram **fora**: vivem no `iotrail.conf`, que é
  fonte viva, e espelho de config no disco envelhece e passa a mentir.
- **Buraco ou sobreposição entre segmentos consecutivos** no catálogo do boot é
  `warn` e nada mais — o programa não tem como saber qual dos dois está certo.

### 3.2 — CRC-32 e primitivas de codificação — FECHADO (2026-09-07)
`src/storage/crc32.h` e `src/storage/format.h`, os dois header-only. Única
tarefa da fase que não toca em disco nem em thread. Entrou também
`src/storage` no include path (`CMakeLists.txt:81`).

- **`memcpy` de `struct` empacotada, não `put_u16/u32/u64` por deslocamento.**
  A pergunta era se o `#pragma pack` resolvia o desalinhamento. Resolve metade:
  mata o padding *dentro* da struct (é o que o `static_assert(sizeof(...))`
  prova), mas não muda o campo cair em endereço ímpar dentro do buffer — a
  entrada de tópico do `.meta` tem 10 bytes fixos, então a segunda entrada
  desalinha todos os `uint32` dela. **Quem resolve essa metade é o `memcpy` da
  struct inteira para uma local alinhada**, como em
  `knowledge_base/src/segment_writer.cpp:178`; nunca cast de ponteiro para
  dentro do buffer. O contra clássico — `&campo` de membro empacotado — não
  passa aqui: `-Wall -Wextra -Werror` (`CMakeLists.txt:96`) transforma
  `-Waddress-of-packed-member` em erro de compilação.
- **O custo aceito: a little-endian de `FORMATO.md` §1 deixa de ser
  implementada.** `memcpy` grava na ordem do host. Em x86 e ARM as duas
  coincidem — é o motivo da escolha, zero conversão — e num host big-endian
  sairia segmento em BE com header dizendo LE, ilegível sem nenhum aviso. Daí o
  `#error` de `src/storage/format.h`: o build para em vez de gravar arquivo que
  mente, e o conserto (trocar os `memcpy` por put/get de byte) fica localizado
  para o dia em que alguém compilar num MIPS de roteador. As structs em si são
  da 3.4/3.5 — aqui ficou só a constante e o guard.
- **Tabela de 256 entradas `constexpr`.** A nota que dizia "a rodada anterior
  usou tabela estática" estava errada: `knowledge_base/src/crc32.h:25-37` já
  gerava por `constexpr`. Sem custo de inicialização no boot, sem `.cpp` só pra
  hospedar um array. Nomes sem o prefixo `k` (`crc32_table`, `crc32_update`).
- **A forma incremental fica, e não é código morto.** O writer monta o registro
  inteiro num buffer antes de gravar (o CRC está no byte 0, tem que existir
  antes da escrita), então ele usa a forma de buffer único. Quem usa o
  encadeamento é a varredura da 3.8: lê a parte fixa primeiro, porque é de lá
  que sai o `payload_len`, e só depois o payload — encadear evita concatenar até
  1 MiB num buffer novo só pra conferir o CRC.
- **O teste é `static_assert`, não executável.** O valor de conferência canônico
  do CRC-32/ISO-HDLC (`crc32("123456789") == 0xCBF43926`) é avaliado pelo
  compilador em todo build (`src/storage/crc32.h:72-74`). Errar polinômio, valor
  inicial ou inversão reprova o build, sem framework de teste — que continua
  fora do projeto. Conferido também contra `zlib.crc32` do Python em quatro
  entradas, incluindo buffer vazio e o encadeamento `"1234"`+`"56789"`.
- **Sem `namespace detail`.** A tabela e o `make_crc32_table()` ficaram planos em
  `storage`. O `detail` veio junto da rodada anterior, onde `crc32_detail`
  (`knowledge_base/src/crc32.h:21`) existia por não haver namespace nenhum em
  volta; aqui já há, nenhum outro header do projeto usa `detail`, e ele obrigava
  a reabrir o namespace no fim do arquivo só para hospedar um `static_assert`.

Sobrou uma dívida de nomenclatura: `FORMATO.md` §1 ainda dizia "parte fixa do
registro (26)", de antes de o tópico sair do registro. Corrigido para 28 na
mesma leva.

### 3.3 — Camada de plataforma (`fsync`/`truncate`)
O que a 1.3 adiou nominalmente para esta fase. `#ifdef` em módulo próprio, não
espalhado.

Decisões a tomar:
- `FILE*` + `_commit(_fileno(f))` / `fsync(fileno(f))`, ou descritor cru?
- Criação de diretório fica com `std::filesystem::create_directories` (não
  precisa de `#ifdef`) — confirmar.
- **O que fazer quando o `fsync` falha.** `EIO` é o caso em que o dado já se
  perdeu e o SO está avisando uma vez só.

### 3.4 — O arquivo `.meta`: escrita, leitura e recuperação
Primeira coisa do projeto que grava e recupera de verdade. `src/storage/meta.*`:
header de 12 bytes, tabela de tópicos append-only, varredura no boot com
truncagem do rabo, e a tabela em memória (`FORMATO.md` §5).

**É uma fatia vertical de propósito** — exercita CRC-32 (3.2), `fsync` e
`truncate` (3.3) e recuperação de rabo num arquivo de dezenas de bytes, antes de
as mesmas peças irem para o caminho quente do segmento.

**Tem consumidor real desde o primeiro dia:** o `on_message` já casa tópico com
stream (`src/mqtt/client.cpp:188`) e hoje só loga. Ligando a tabela ali, cada
mensagem resolve ou insere o tópico e o `.meta` cresce contra o broker de casa.
Não é andaime: é a mesma tabela que o `push` vai consultar na 3.7.

Decisões a tomar:
- **Quando o `.meta` é criado:** no boot, para toda stream da config, ou na
  primeira mensagem que chega? Criar no boot deixa pasta e arquivo prontos e
  falha cedo se o disco não deixa escrever; criar sob demanda não cria lixo para
  stream que nunca recebe nada.
- **`.meta` ausente com segmentos presentes** — apagado à mão ou perdido: os
  `topic_id` viram órfãos (§4 diz que o registro sobrevive). Recriar vazio e
  seguir, ou parar a stream e deixar o operador decidir?
- **Concorrência:** a tabela é lida por N callbacks do MQTT e escrita quando
  aparece tópico novo. Lock por stream, ou estrutura imutável trocada por
  ponteiro? A `streams_` da 2.5 já resolveu um caso parecido sem lock por ser
  imutável desde a construção.
- Índice inverso `topic → id` para o caminho de recebimento, junto do
  `id → topic` que a leitura usa.

### 3.5 — Registro: structs e codificação (sem I/O)
`src/storage/record.*`, sobre as primitivas da 3.2. Deve ser possível gerar os
bytes de um registro e conferi-los contra o leitor Python sem tocar em disco. O
`topic_id` já vem resolvido pela tabela da 3.4.

Decisões a tomar:
- `#pragma pack` + `static_assert` (como a rodada anterior) ou serialização
  campo a campo? Packed struct é prática comum, mas ponteiro para membro
  desalinhado é UB — e o `-Werror` do projeto pode ter opinião.
- Onde vive o buffer de montagem: um por writer, reutilizado, pra não alocar por
  mensagem no caminho quente.
- Os limites do §5 são checados aqui ou no `push`?
- Fechado na 3.1: `payload_len == 0` é gravado, e com `payloadlen == 0` a
  mosquitto entrega `msg->payload` nulo — o `push` não pode fazer `memcpy` cego.
- A entrada da tabela de tópicos (10 B fixos + tópico) já foi escrita na 3.4:
  mesmo CRC, mesma regra de faixa. Se as duas não puderem compartilhar as mesmas
  primitivas, alguma das duas está torta.

### 3.6 — Escrita do segmento: header + append
Decisões a tomar (ficam para quando a tarefa chegar):
- `FILE*` com buffer da libc ou `write()` direto.
- Um `fwrite` por registro montado em buffer — a rodada anterior fez assim, e o
  motivo é bom: o CRC precisa do registro pronto antes de gravar, e uma escrita
  única cria menos fronteiras de escrita parcial.
- Quando o header de 14 bytes é sincronizado.
- **A entrada nova no `.meta` é gravada e sincronizada antes** do primeiro
  registro que usa o `topic_id` (`FORMATO.md` §5). Só quando aparece tópico
  novo; em regime, nunca. É o único `fsync` fora do `sync_interval`.
- Atualizar `next_topic_id` no header do `.meta` na mesma ocasião.
- **Falha de escrita (disco cheio):** parar aquela stream, derrubar o processo,
  ou contar e seguir perdendo? É decisão de perda de dado, não de código.

### 3.7 — Fila e writer thread por stream
Decisões a tomar (ficam para quando a tarefa chegar):
- Polling ou `condition_variable`: o polling custa até 2× `write_interval` no
  encerramento, o que compete com o orçamento do handler da 1.3.
- **Teto da fila e política quando encher** — descartar o mais novo, o mais
  velho, ou bloquear o recebimento. Hoje é item aberto no `DESIGN.md` §8.
- Defaults de `write_interval` e `sync_interval`, e se viram chave de config
  agora ou só na Fase 9.
- O `push` **copia** o payload: o buffer da mensagem pertence à lib e morre
  quando a callback do MQTT retorna (dívida registrada na 2.6,
  `client.cpp:174-178`).

### 3.8 — Recuperação do segmento no boot
Implementar o §8 da spec: varredura do último segmento, descarte do rabo
corrompido, truncagem, retomada do contador de offset. A recuperação do `.meta`
já saiu na 3.4 — aqui é a do segmento, com o mesmo critério de falha de tamanho
contra falha de conteúdo.

Decisões a tomar:
- `magic`/`format_version` errados derrubam **aquela stream** ou o processo?
- Ordem no boot: o `.meta` é lido antes do segmento (o segmento sozinho não
  resolve `topic_id`), mas registro com id desconhecido **não** é descartado
  (§4).
- Quanto vai pro log: bytes truncados, offset retomado, tempo da varredura.

### 3.9 — Encerramento com writers, dentro do orçamento
A restrição que a 1.3 deixou anotada: drenar M filas e fazer M `fsync` **dentro
do handler de console**, com ~5 s do SO menos os 200 ms do laço.

Decisões a tomar:
- Limitar a drenagem (perda conhecida e registrada no log) ou tentar até o fim
  (risco de ser morto no meio da escrita)?
- Ordem: parar os clientes MQTT primeiro, depois drenar — senão a fila recebe
  enquanto se tenta esvaziá-la.
- Medir contra a linha de base da 2.7: hoje o encerramento com 2 clientes
  conectados leva **menos de 1 ms**.

### 3.10 — Fechamento da fase
- **Validação cruzada com o leitor Python** (§10 da spec) sobre `.log` e `.meta`
  gerados pelo C++. É o teste que prova que o formato é o que o documento diz, e
  não "o que o writer faz".
- **Teste de queda:** matar o processo à força durante escrita e conferir
  recuperação e truncagem no boot seguinte.
- **Medições:** latência chegada→disco, custo do `fsync`, e **bytes escritos por
  mensagem** — amplificação de escrita é a métrica de desgaste de flash, que é o
  que importa em edge.
- Docs, `DESIGN.md` §4 (que hoje aponta para a spec na `knowledge_base`) e
  revisão de âncoras.

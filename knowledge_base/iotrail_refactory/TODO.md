# IoTrail — base reescrita: plano

Reescrita estrutural iniciada em 2026-09-02, em `iotrail_refactory/`, convivendo
com o `src/` antigo até alcançar paridade.

**Estado:** Fase 1 fechada em 2026-09-03 (1.1, 1.2, 1.3, 1.5 e 1.6 fechados;
1.4 adiado). Fase 2 planejada em 2026-09-03, ainda sem código.

**As decisões de design do projeto não mudam.** Formato de segmento
(`docs/formato_segmento.md`), multi-stream, fila + writer thread por stream,
acesso de consumidores via API HTTP + token — tudo continua valendo como está em
`claude_memory/CLAUDE.md`. O que se refaz é a organização do código.

## Escopo da Fase 1 — FECHADA (2026-09-03)

Fundação do projeto, **terminando na config**. Fora desta fase: cliente MQTT,
formato de registro, `SegmentWriter`, camada `platform/` (fsync/truncate — é
dependência do writer, entra com ele), roteamento tópico → stream.

## Modo de trabalho

Um item por vez. Para cada um: **eu apresento as decisões com opções e
trade-offs → você decide → eu escrevo aquele pedaço → paro.** Nada de escrever
o item seguinte na mesma leva.

Cada item abaixo lista as decisões que precisam sair antes do código. Elas estão
em aberto de propósito — nenhuma está pré-decidida.

---

## 1.1 — Esqueleto de build — FECHADO (2026-09-02)

`CMakeLists.txt` que compila um `main.cpp` vazio. Sem lógica ainda; o objetivo é
ter o terreno em que os próximos itens caem. O raciocínio completo de cada
decisão está **em comentário no próprio `CMakeLists.txt`**, junto da linha que a
implementa; aqui fica o registro do que ficou decidido.

**Decisões tomadas:**

- **Um alvo só**, fontes direto em `add_executable()`. A alternativa
  (biblioteca + executável fino) só se pagava pra deixar um teste linkar o
  código, e 1.4 foi adiado — some a razão, some a estrutura. Reabrir quando o
  framework de teste entrar.
- **Headers junto dos `.cpp`**, sem `include/` separado — mantida a decisão já
  registrada no `CLAUDE.md`, não reaberta.
- **Layout quase plano:** `src/` direto, com uma exceção — `src/config/`, que
  ganhou pasta por ter dois pares de arquivos (`ini` e `config`). `mqtt/` e
  `storage/` seguem o mesmo critério quando existirem. Os dois diretórios estão
  no include path, então todo include é pelo nome do arquivo (`"ini.h"`,
  `"logging.h"`): não há include relativo atravessando pasta.
- **C++17 mantido.** C++20 foi cogitado e descartado nesta rodada, com a análise
  preservada no `CMakeLists.txt` pra não se refazer: o ganho concreto seria
  `std::jthread` + `std::stop_token` no lugar do par `atomic<bool>` + `thread` +
  join manual (padrão que vai se repetir uma vez por stream) e `std::span` no
  lugar do par `(const void*, size_t)` do `crc32.h`. Confirmado por teste que o
  GCC 16.2.0 compila e roda os dois — a limitação não é do toolchain, subir
  depois continua possível. **Sem extensões GNU** (`-std=c++17`, não `gnu++17`).
- **`-Wall -Wextra -Wpedantic`, sem `-Werror`** — de propósito: aviso novo
  (compilador atualizado, header de terceiro) não deve quebrar o build no meio
  do trabalho. Adicionar depois é uma linha.
- **Runtime C++ dinâmico**, sem `-static-libgcc -static-libstdc++` — ao
  contrário do `CMakeLists.txt` antigo. Confirmado com `objdump -p` que a
  `libspdlog-1.17.dll` é compilada contra a `libstdc++` dinâmica: a DLL
  acompanha o programa de qualquer jeito, e linkar estático só faria existirem
  dois runtimes C++ no mesmo processo com `std::string` cruzando a fronteira. O
  raciocínio depende de a spdlog ser compartilhada (ver 1.2) — na rota
  header-only, `-static` volta a fazer sentido.
- **Saiu junto:** `cmake_minimum_required(3.21)` por causa de
  `TARGET_RUNTIME_DLLS` (que resolve as DLLs importadas sozinho, sem lista com
  número de versão fixo em texto como no `CMakeLists.txt` antigo);
  `CMAKE_EXPORT_COMPILE_COMMANDS` pro IntelliSense acompanhar o build sem lista
  de includes mantida à mão; e `IOTRAIL_MSYS2_UCRT64` como variável de cache,
  pra apontar pra outra instalação por linha de comando sem editar o arquivo.

---

## 1.2 — Logging — FECHADO (2026-09-02)

Setup do spdlog. Já estava decidido como lib (`CLAUDE.md`, 2026-08-29); o que
faltava era como ele entra no código. Implementado em `src/logging.h`/`.cpp`.

**Decisões tomadas:**

- **Nem `spdlog::` direto nem wrapper: `using`-declarations.** O `logging.h`
  reexporta `trace`/`debug`/`info`/`warn`/`error`/`critical` sob o namespace
  `logging`. Não é wrapper — não há indireção nem sobrecarga, e a API de
  formatação `{}` continua idêntica —, mas todo ponto de chamada escreve
  `logging::info`. Ficou o meio-termo útil: se a lib trocar um dia, as `using`
  viram funções de verdade e **nenhum ponto de chamada muda**.
- **Só console** (`stdout_color_sink_mt`). Arquivo rotativo continua Fase 9,
  como o `CLAUDE.md` já planejava — não foi antecipado.
- **Logger assíncrono**, thread pool própria (fila de 8192, 1 worker,
  `overflow_policy::block`). É o modo que a escolha da lib pressupunha em
  `CLAUDE.md`: logar de forma síncrona no caminho de recebimento/escrita
  reintroduziria a variância de latência que `docs/decisao_sync_write.txt`
  existe pra evitar.
- **Flush por mensagem** (`flush_on(trace)`) mantido. Com o logger assíncrono
  isso não bloqueia quem chama — o flush acontece na thread do pool.
- **Setup em função própria**, `logging::init()`, fora do `main()`. E
  **`logging::shutdown()` explícito**, que não existia antes: com logger
  assíncrono há mensagens na fila no momento de encerrar, e sem o shutdown as
  últimas linhas se perdem. O `main()` chama nas duas saídas, a normal e a de
  config inválida.
- **spdlog compartilhada, não header-only** (`spdlog::spdlog`, não
  `spdlog::spdlog_header_only`). Medido: 468 KB com DLL contra 5,6 MB
  header-only + link estático. É esta escolha que sustenta a decisão de runtime
  dinâmico em 1.1.

---

## 1.3 — Parada ordenada e `main` mínimo — FECHADO (2026-09-02)

Handler de sinal e um `main` que sobe, espera e encerra limpo. Implementado em
`src/signals.h`/`.cpp`.

**Decisões tomadas:**

- **`SIGINT` e `SIGTERM`** — `SIGTERM` é o que um serviço/supervisor manda.
- **Mais o console control handler do Windows**, que não estava previsto na
  pergunta e é o achado do item: `SIGINT` no Windows cobre **só Ctrl+C**.
  Fechar a janela do console, Ctrl+Break, logoff e shutdown do sistema chegam
  por `SetConsoleCtrlHandler` e de outra forma matariam o processo sem parada
  ordenada. Tratados `CTRL_C`/`BREAK`/`CLOSE`/`LOGOFF`/`SHUTDOWN`, retornando
  `TRUE` pra não cair no handler default. Ressalva registrada no código: em
  `CTRL_CLOSE_EVENT` o Windows mata assim mesmo depois de poucos segundos — o
  encerramento tem que ser rápido, o que vira restrição real quando houver fila
  pra drenar e `fsync` final por stream.
- **Separado do logging.** O `init()` único de `src/init.cpp:8-13` juntava duas
  coisas sem relação; agora são `logging::init()` e `signals::init()`, chamados
  em sequência pelo `main()`.
- **`std::atomic<bool>`, não `volatile sig_atomic_t`** (que era o do
  `src/init.cpp`). No Windows tanto o handler de `SIGINT` quanto o do console
  rodam numa thread criada pelo SO — isso é comunicação entre threads, não
  interrupção de sinal, e `volatile` não dá garantia nenhuma nesse caso.
- **Parada sem sinal do SO: adiada**, como a pergunta previa. Não há
  `requestStop()` público hoje; a API de gerência da Fase 6/7 acrescenta uma
  linha quando precisar, e `stopRequested()` já é o ponto de leitura que todo o
  resto consulta.
- **`main()` espera em polling de 200 ms.** Suficiente enquanto ele só espera;
  vira `condition_variable` se a espera passar a ter trabalho pendurado nela.

---

## 1.4 — Esqueleto de testes — ADIADO (2026-09-02)

Decidido não introduzir framework de teste nesta fase. Fica para depois da
config, sem data marcada. A ordem passa direto de 1.3 para 1.5.

Consequência a não esquecer: a config (1.5/1.6) vai nascer sem teste. Se a
decisão de "erros voltam pro chamador em vez de irem pro log" for tomada lá,
ela continua valendo — é o que torna a config testável quando o framework
entrar, mesmo que não haja teste nenhum no dia.

**Decisões, quando o item voltar:**

- **Framework:** Catch2 v3, doctest ou GoogleTest — os três estão disponíveis no
  MSYS2 (`pacman`).
- **Integração com CTest** (`ctest` roda a suíte) ou basta um executável de teste
  rodado na mão?

---

## 1.5 — Config: leitura do arquivo — FECHADO (2026-09-03)

Ler o `iotrail.conf` e transformá-lo em estrutura de dados. Implementado em
`src/config/ini.h`/`.cpp`.

**Decisões tomadas:**

- **Parser genérico separado da interpretação.** `ini.h`/`.cpp` só quebra o
  arquivo em seções e pares `chave=valor`; `config/config.cpp` dá significado.
  O parser não conhece broker nem stream. Custou o arquivo a mais previsto.
- **Seção tipada `[tipo:nome]` — mas quem exige o tipo é o domínio.** O parser
  aceita as duas formas (`[casa]` vira `type` vazio, `ini.cpp:59-65`); é o
  `config.cpp` que rejeita seção sem tipo, com erro sugerindo `[broker:x]` ou
  `[stream:x]`. Assim a regra de domínio não vaza pra camada genérica.
- **Erros são logados de dentro do parser**, não devolvidos ao chamador.
  `parseFile()` devolve `optional<Sections>` — `nullopt` em qualquer erro de
  sintaxe, mas o arquivo é lido **até o fim** e todos os problemas vão pro log
  de uma vez, em vez de um por boot. **Consequência assumida:** é exatamente a
  opção que o item 1.4 avisou que dificulta teste — verificar os erros exigiria
  capturar saída de log. Decisão consciente pelo caminho mais simples agora,
  revisitável quando o framework de teste entrar.

**Comportamentos que saíram junto, no parser:** comentários `#` e `;`, trim em
chave e valor, `=` obrigatório, e três erros fatais — cabeçalho sem `]`, seção
sem nome, e par `chave=valor` antes de qualquer seção (silenciar seria perder
config que o operador escreveu achando que valia).

**Chave repetida na mesma seção é erro fatal** (`ini.cpp:106-111`). Vale
destacar porque virou load-bearing depois: é essa checagem, genérica e de graça
para toda chave, que sustenta a decisão de manter `topics=` como lista separada
por vírgula em vez de chave repetida (2026-09-03) — multi-linha exigiria
enfraquecê-la ou empurrá-la pro domínio.

---

## 1.6 — Config: validação e regras do domínio — FECHADO (2026-09-03)

Dar significado ao que 1.5 leu: o que é um broker válido, o que é uma stream
válida. Implementado em `src/config/config.h`/`.cpp`.

**Decisões tomadas:**

- **Config inválida derruba o boot.** `load()` devolve `nullopt` e o `main()`
  encerra com `return 1`. O fallback `127.0.0.1:1883` do `src/main.cpp` antigo
  foi descartado: 1883 é convenção IANA, mas `127.0.0.1` não é convenção
  nenhuma pra "onde está meu broker" — seria escolher um destino que ninguém
  escreveu.
- **Severidade:** erro fatal para tudo que muda o que o programa grava ou de
  onde ele lê (protocolo não suportado, `host=` ausente, porta inválida, seção
  duplicada, nome de stream inválido, broker não declarado). Aviso apenas para
  chave desconhecida e tipo de seção desconhecido — ali o que o operador
  escreveu é ignorado, mas nada do que sobra fica errado. Uma seção ruim **não**
  interrompe as outras: o arquivo é lido até o fim e todos os problemas vão pro
  log de uma vez, em vez de um por boot.
- **Nome de stream é validado aqui**, na config (`validStreamName`), não no
  writer. Nome inválido tem que falhar no boot nomeando a seção culpada, não
  num `fopen` obscuro depois.
- **Multi-broker: já modelado, não só lido.** `Config::brokers` é um
  `vector<Broker>` e a aresta stream→broker é campo validado — a config devolve
  um **grafo já resolvido**, não uma lista de seções pra alguém interpretar
  depois. A decisão de **um broker por stream** (2026-09-03, ver
  `claude_memory/CLAUDE.md`) é o que fechou esta pergunta: enquanto uma stream
  podia nomear N brokers isso era um grafo N:M, e modelá-lo exigia decidir
  semântica de fan-in (dedup, ordenação entre origens, subscribe por broker);
  com 1:1 virou árvore N:1, que não tem o que decidir.
  **Garantia que a Fase 2 herda:** numa `load()` bem-sucedida, todo
  `Stream::broker` nomeia um `Broker` presente em `Config::brokers` — qualquer
  seção de broker que falhe derruba a config inteira. Não existe caminho de
  runtime "broker não encontrado"; o `map<broker → cliente>` monta direto.
  Detalhe deliberado: a stream é checada contra os brokers **declarados**, não
  os carregados com sucesso (`config.cpp:167-169`), pra que um `port=` inválido
  culpe a seção do broker e não a stream. A garantia acima continua valendo
  porque nos dois casos a config toda é rejeitada.
- **Conectar** em um ou em vários continua fora da Fase 1 — é Fase 2, um
  `IotrailClient` por broker com `loop_start()`.

---

## Achados a carregar para a Fase 1 — AMBOS RESOLVIDOS (2026-09-03)

Duas coisas que apareceram validando o código antigo, para não se perderem:

- ~~**`std::atoi` na porta** (`src/config.cpp:52`) aceita `"1883x"` como `1883`
  e `"abc"` como `0`, em silêncio.~~ Resolvido no item 1.6: `parsePort()`
  (`src/config/config.cpp:13-22`) usa `strtol` exigindo consumo total da string
  e faixa 1–65535; os dois casos viram erro fatal com a porta no texto.
- ~~**BOM UTF-8** no início do `.conf` faz a primeira seção do arquivo virar
  lixo para o parser.~~ Resolvido no item 1.5: `ini.cpp:41-43` remove o BOM na
  linha 1 antes de qualquer parsing.

---

## Escopo da Fase 2 (definido 2026-09-03)

**MQTT ponta a ponta, sem disco.** Conectar em **N brokers** simultâneos,
subscrever o que a config manda, rotear tópico → stream e **logar**. O log é o
sink desta fase inteira.

Fora desta fase, explicitamente: `SegmentWriter`, camada `platform/`
(fsync/truncate), fila, rollover, índice. **Nada toca o disco na Fase 2.**

**Isto reverte o adiamento do multi-broker**, que estava marcado como pós-MVP no
`claude_memory/TODO.md` depois de ser adiado duas vezes. O motivo do adiamento
era que trocar o `loop()` manual por `loop_start()` por instância mexe no modelo
de threading do `main()`. Numa base que está sendo reescrita e que ainda **não
tem writer nem fila**, esse modelo ainda não existe pra ser mexido — o custo da
mudança nunca vai estar mais baixo. Fazer depois significaria mexer nele já com
writer e fila montados em cima.

---

## Fase 2 — lista de tarefas

Ordem de execução. Cada tarefa nomeia a decisão que a bloqueia, quando há uma —
as decisões estão nos itens 2.1–2.3 abaixo e saem antes do código, no modo de
trabalho já usado na Fase 1.

**Config — feito**

- [x] **`client_id` na seção `[broker:*]`** (2026-09-03). Opcional, default
      `iotrail-<nome da seção>` — o mesmo que o `src/main.cpp:31` antigo já
      derivava. Vazio é erro; acima de 23 caracteres é aviso (limite garantido
      pelo MQTT 3.1.1, que o mosquitto ignora); id repetido **no mesmo
      `host:port`** é aviso, porque é o único caso em que dois clientes se
      derrubam — id igual em brokers diferentes convive sem problema. Validado
      nos cinco casos, incluindo a topologia de teste do 2.3.

**2.1 — Clientes MQTT** — FECHADO (2026-09-04)

Decidido: **API C (`libmosquitto`)**, não o wrapper C++ — nem herança nem
composição, o que também mantém o MQTT5 alcançável (`m_mosq` é `private` no
`mosquittopp.h:84`, e o wrapper não tem `int_option`, callbacks v5 nem
properties). Um cliente por broker, `loop_start()` por instância,
`connect_async()`, `clean_session=true`.

- [x] Módulo `mqtt_client.h`/`.cpp` — `struct mosquitto;` no header, lib
      confinada ao `.cpp`
- [x] `mosquitto_lib_init()`/`lib_cleanup()` uma vez por processo —
      `mqtt::init()`/`mqtt::shutdown()`, funções de módulo (não RAII), no mesmo
      estilo de `logging::init()`
- [x] Um cliente por entrada de `Config::brokers`, `loop_start()` por instância
- [x] Conectar com `connect_async()` — "processo sobe sempre"
- [x] `clean_session = true` — sem QoS > 0 a sessão persistente não guardaria
      nada; revisitar junto com QoS
- [x] `subscribe()` = união dos `topics=` daquele broker, campo derivado na
      config — **verificado 2026-09-04**: `casa1` assinou só `temp`, `casa2` só
      `umidade`, e uma publicação em `pressao` não gerou linha nenhuma
- [x] `on_message` → log `[mqtt/<broker>]` com tópico + tamanho do payload —
      **verificado 2026-09-04** contra o broker real, tamanhos batendo com o
      publicado, cada mensagem chegando a exatamente um cliente
- [x] Encerramento: `disconnect()` + `loop_stop(force)`, com o `force` decidido
      pelo **retorno do `disconnect`** — `loop_stop(false)` bloqueia até a
      thread da lib terminar, e ela só termina se o disconnect funcionou
      (`mosquitto.h:1279-1288`); num cliente que nunca conectou o disconnect
      devolve `MOSQ_ERR_NO_CONN` e a parada limpa travaria.
      **Verificado à mão pelo André em 2026-09-04: Ctrl+C encerra rápido e
      limpo.** Nenhum teste automatizado consegue cobrir isto — o `timeout` do
      MSYS não entrega sinal a executável Windows nativo, então em cinco
      rodadas o caminho de parada nunca chegou a rodar. Só Ctrl+C real no
      terminal verifica.

**2.1 — descobertas no caminho, não previstas na lista**

- [x] **`Client::tick()`**, chamado pelo laço do `main()`. O auto-reconnect da
      lib (`reconnect_delay_set`) só cobre queda **depois** de uma conexão
      estabelecida — `mosquitto.h:1657` diz "unexpectedly disconnected".
      Medido: com o broker fora do ar no boot, a lib manda **um** CONNECT e
      nunca mais tenta. Sem o `tick()`, "processo sobe sempre" entregaria um
      cliente morto calado. A divisão ficou: nunca conectou → o `tick()`
      retenta (backoff 1s→30s, sem dormir, por deadline em `steady_clock`); já
      conectou alguma vez → a lib retenta. `ever_connected_` separa os dois, e
      governa também a mensagem de log, pra não mandar procurar problema no
      lugar errado.
- [x] **`errorText()`** — `mosquitto_strerror()` devolve "Unknown error" para
      `MOSQ_ERR_ERRNO`, que é o código de quase toda falha de rede; sem o
      `errno` junto, recusa, host inalcançável e falha de DNS viravam a mesma
      mensagem inútil.
- [x] **Callback de log da lib** (`mosquitto_log_callback_set`), em `debug`.
      Entrou porque as falhas de `connect_async` pareciam mudas; depois se
      descobriu que `on_disconnect` **dispara** nesse caso e teria bastado.
      Ficou por utilidade de diagnóstico, não por necessidade.

**Registro de um erro de método, pra não repetir:** durante este item eu afirmei
três vezes coisas não medidas — que o `join()` travava o encerramento (o sinal
nunca chegava ao processo), que trocar para `loop_start()` resolveria o
encerramento (não resolvia; a correção era o `force`, válida nas duas rotas) e
que o backoff da lib cobriria a conexão inicial (não cobre). O que de fato
sustentou a troca foi uma medição só: `connect()` síncrono trava o boot por
~20s por broker inalcançável, `connect_async()` não.

**2.2 — Roteamento**

- [x] Roteamento **dentro do `mqtt_client.cpp`**, sem módulo próprio. Chegou a
      existir um `src/routing.h`/`.cpp` com funções livres, justificado por
      testabilidade; foi **apagado em 2026-09-04** por decisão do André. O
      argumento não se sustentava: um módulo de 12 linhas cuja razão de ser é um
      teste que não existe e cujo framework está adiado sem data é abstração
      antecipada, não separação de camada. Se o teste chegar, extrair a função
      naquele momento é refatoração de minutos.
- [x] Pré-computar a lista de streams por broker — feita **no construtor do
      `Client`**, que recebe a `Config` inteira. Guarda ponteiros pra dentro
      dela, então a `Config` só precisa sobreviver à construção, não ao cliente.
      Cada cliente carrega só a própria lista: não há estrutura compartilhada
      entre as N threads da lib, e o caminho da mensagem não tem lock nenhum.
- [x] Casar com `mosquitto_topic_matches_sub()` — **todas as streams que casam,
      não a primeira** (ver decisão de fan-out abaixo).
- [ ] **Tópico sem match — ADIADO (2026-09-04).** Hoje é **descartado em
      silêncio** (`Client::onMessage` retorna sem logar). Isto contraria o
      `claude_memory/CLAUDE.md`, que manda `warn` e descartar — a divergência é
      consciente e provisória, não descuido.
      **Por que foi adiado:** `warn` por mensagem inunda o log quando um sensor
      não mapeado publica rápido, e a alternativa mais útil de ler é a única que
      cobra um preço estrutural.
      **As opções, com o custo de cada uma:**
      (a) *volume cru* — um `warn` por mensagem; mantém tudo imutável, zero
      estado, zero lock, mas um sensor a 10 Hz enche o log sozinho;
      (b) *primeira ocorrência por tópico* — responde a pergunta real do
      operador ("qual tópico está caindo fora?"), mas exige um conjunto mutável
      escrito de dentro do `on_message`. **Verificar antes de assumir o pior:**
      os callbacks de um mesmo cliente parecem rodar só na thread daquele
      cliente, e se isso se confirmar o conjunto é dado privado do cliente e
      **não** precisa de lock;
      (c) *contador com resumo periódico* — o `tick()` já roda no `main()` e
      teria onde imprimir, mas o contador seria escrito na thread da lib e lido
      na do `main()`, exigindo `atomic`;
      (d) *silencioso com contador exposto depois* — descarta informação que o
      `CLAUDE.md` diz que tem que aparecer no log.
      **Quando isto voltar, reconciliar com o `CLAUDE.md`** — ou implementando o
      `warn`, ou registrando lá a mudança de regra.

**2.3 — Teste**

- [x] `iotrail.conf` de teste: duas seções `[broker:*]` no mesmo IP, nomes
      diferentes, **tópicos disjuntos** — feito 2026-09-04: `casa1`/`temp` e
      `casa2`/`umidade`, sem catch-all. Com padrões disjuntos não há
      sobreposição, então nem a entrega dobrada nem a colisão de `client_id`
      chegam a acontecer.
- [x] *(decisão)* subir uma segunda instância do mosquitto na porta 1884 —
      **decidido não subir** (2026-09-04). Falha parcial fica sem cobertura.
- [x] Cenário: os dois conectam, cada tópico cai na stream certa, log
      identificando o broker de origem — **verificado 2026-09-04** contra o
      broker real, incluindo o caso negativo: tópico fora da união não é
      entregue.
- [x] Cenário: reconexão depois de conexão estabelecida (`ever_connected_ ==
      true`, quem reconecta é a lib, não o `tick()`) — **verificado à mão pelo
      André em 2026-09-04**, parando e religando o mosquitto do
      `192.168.0.115` com o programa no ar. Era o último ramo do 2.1 sem
      nenhuma cobertura. Exercita junto o re-`subscribe` no `onConnect`, que
      só existe porque `clean_session=true` faz o broker esquecer as
      inscrições quando a conexão cai.
- [x] Cenário: falha parcial com brokers independentes — **não será testado**
      (decisão de 2026-09-04, não subir segunda instância). É a única lacuna
      que resta, e ficou por avaliação de que o comportamento é previsível a
      partir do que já foi medido.

**Fechamento da fase**

- [x] Registrar as decisões tomadas nos itens 2.1–2.3, no padrão `FECHADO` da
      Fase 1 — feito 2026-09-04.
- [x] Marcar o multi-broker como revertido em `claude_memory/TODO.md` — feito
      2026-09-04.

---

## 2.1 — Clientes MQTT: N brokers desde o início — FECHADO (2026-09-04)

Um cliente por broker, todos ativos ao mesmo tempo, conectando, subscrevendo e
logando. **Não há etapa intermediária de uma conexão só** (decidido 2026-09-03):
a config já modela N brokers com a garantia de que todo `Stream::broker`
resolve, e o teste de dois brokers apontando pro mesmo IP (ver 2.3) está
disponível desde o primeiro dia — não há motivo pra validar o caminho único
primeiro.

**Decisões:**

- **Client id: um por seção `[broker:*]`, não um por processo.** Esta deixou de
  ser escolha livre e virou requisito de correção por causa da topologia de
  teste do 2.3. MQTT 3.1.1 §3.1.4 obriga o broker a **derrubar a conexão
  existente** quando outra chega com o mesmo ClientId (`[MQTT-3.1.4-2]`) — com
  dois clientes no mesmo broker e id igual, os dois entram em laço de
  reconexão se derrubando mutuamente, e o sintoma não parece um problema de id.
  Falta decidir a **forma**: derivar do nome da seção (`iotrail-casa`), do nome
  mais um sufixo aleatório, ou id configurável.
- **Herança ou composição?** O `src/iotrail_client.h:7` antigo faz
  `class IotrailClient : public mosqpp::mosquittopp`. Herdar obriga o header a
  incluir `<mosquittopp.h>` e arrasta a limitação do `m_mosq` privado já
  documentada no `CLAUDE.md`. Compor esconde a lib inteira no `.cpp`.
- **`loop_start()` por instância** (thread criada pela lib) **ou um `loop()`
  multiplexando?** O `loop_start()` é o caminho que o `claude_memory/TODO.md` já
  apontava; fica registrado pra decisão sair explícita e não por inércia.
- **Onde ficam `mosqpp::lib_init()` / `lib_cleanup()`?** Função de módulo no
  mesmo estilo de `logging::init()`, ou um objeto RAII que faz o par sozinho.
  Com N instâncias, o par tem que acontecer **uma vez**, não por cliente.
- **Broker fora do ar no boot: aborta tudo, ou sobe o que dá e insiste?**
  `connect()` falha na hora; `connect_async()` + `reconnect_delay_set()` deixa a
  lib insistir sozinha em background. Pesa a favor da segunda: um gateway de
  borda normalmente liga **antes** da rede estar pronta.
- **E se nenhum broker conectar?** Processo segue vivo tentando, ou encerra com
  erro? É pergunta diferente da anterior — uma coisa é tolerar um de dois, outra
  é tolerar zero.
- **`clean_session` true ou false?** Com `false` (+ QoS > 0, que é fase futura)
  o broker enfileira o que chegou enquanto o processo esteve fora. Decidir ao
  menos a intenção agora, porque amarra com o client id acima.
- **Quem calcula a união dos `topics=`:** a config, como campo derivado, ou o
  cliente, na hora de subscrever. Com N brokers a união é **por broker** — só as
  streams ligadas àquele cliente.
- **Encerramento com N conexões:** `disconnect()` + `loop_stop()` por instância,
  e `loop_stop()` com ou sem `force`. Lembrar da restrição registrada no item
  1.3: em `CTRL_CLOSE_EVENT` o Windows mata o processo depois de poucos
  segundos — o encerramento tem que caber nessa janela.
- **Identidade no log:** prefixo por broker (`[mqtt/casa]`) desde a primeira
  linha. Com N conexões, log sem origem vira ilegível — e esta fase entrega log.
- **O que a linha de log traz por mensagem:** tópico + tamanho do payload, ou o
  payload em si? Payload é opaco e pode ser binário — despejar bytes crus no
  console é ruído, e esta fase inteira depende do log ser legível.
- **Onde para o escopo:** backoff configurável e QoS continuam fora (item
  próprio no `claude_memory/TODO.md`). Aqui entra só o que multi-broker
  **obriga** a decidir, que é o comportamento de boot acima.

---

## 2.2 — Roteamento tópico → stream, só com log — FECHADO (2026-09-04)

Casar a mensagem com a stream e logar qual levou. Sem escrever nada.
Implementado inteiramente em `src/mqtt_client.cpp`.

**Decisões tomadas:**

- **Sem módulo de roteamento: tudo no `mqtt_client.cpp`** (decidido pelo
  André, 2026-09-04). Um `src/routing.h`/`.cpp` chegou a existir e foi apagado
  no mesmo dia — a justificativa era testabilidade, mas o framework de teste
  está adiado sem data (item 1.4), então era abstração antecipada. `streamsOf`
  e `matchAll` são funções comuns do `.cpp`, ao lado do `errorText`.
- **Cada `Client` filtra as próprias streams no construtor**, recebendo a
  `Config` inteira e guardando ponteiros pra dentro dela — a `Config` só
  precisa sobreviver à construção, não ao cliente. Isso resolveu de graça o
  assunto mais chato da fase: **não existe estrutura compartilhada entre as N
  threads da lib, então o caminho da mensagem não tem lock nenhum e nem
  precisa**.
- **`Broker::topics` removido da config** (decidido pelo André, 2026-09-04). A
  config voltou a ser reflexo do arquivo mais validação: não deriva nada da
  relação broker/stream. A união de padrões para o `subscribe()` é montada no
  `onConnect`, a partir das streams do próprio cliente, com dedup local — duas
  streams do mesmo broker podem declarar o mesmo padrão, e o SUBSCRIBE repetido
  seria inofensivo mas apareceria duas vezes no log. Motivo: **uma
  representação derivada em vez de duas**, que era o tipo de duplicação que sai
  de sincronia quando alguém mexe numa só. Removido e revalidado sem mudança de
  comportamento — sinal de que era mesmo duplicação.
- **Fan-out: a mensagem vai para TODAS as streams que casam**, não só a
  primeira (decidido pelo André, 2026-09-04). Duas streams do mesmo broker
  cobrindo o mesmo tópico é escolha de quem configurou, e a intenção provável é
  guardar nas duas — com retenção e replay independentes por stream, isso faz
  sentido. **Isto reverte o "descartado fan-out (gravar nas duas)" registrado
  no `claude_memory/CLAUDE.md`.**
  **Consequência: a ordem de declaração deixa de decidir qualquer coisa.** Ela
  existia só para resolver a disputa entre duas streams que casavam com o mesmo
  tópico; sem disputa, some a regra. A lista continua na ordem do arquivo, mas
  agora só para o log sair determinístico. Uma regra a menos para o operador
  precisar saber.
  **O que isso custa:** um `topics=#` convivendo com uma stream específica no
  mesmo broker faz o tráfego do sensor rápido ser gravado nas duas. O
  isolamento que motivou o multi-stream passa a depender de **padrões
  disjuntos**, não de precedência — mais explícito, e sem a surpresa de um
  catch-all mal posicionado engolir tudo.
- **Tópico sem match: ADIADO.** Ver a tarefa correspondente na lista, com as
  quatro opções e o custo de cada uma. Hoje é descartado em silêncio, o que
  **diverge do `claude_memory/CLAUDE.md`** — divergência consciente e
  provisória, sinalizada por comentário no próprio `onMessage`.
- Confirmado na implementação: o casamento é sempre restrito às streams do
  broker de onde a mensagem veio, e o matcher é
  `mosquitto_topic_matches_sub()` (`mosquitto.h:2451`), que já trata `+` no
  meio, `#` só no fim e `#` não casando com `$SYS`.
- **Uma alocação por mensagem:** o `matchAll` devolve um `vector`. Irrelevante
  enquanto só se loga; no caminho do writer, a milhares de mensagens por
  segundo, vale trocar por um buffer reaproveitado por cliente. Anotado para a
  fase do writer, não otimizado agora.

**Validado 2026-09-04** contra o broker real, em duas topologias. Streams
disjuntas em brokers distintos: `temp -> "temperatura"`, `umidade ->
"umidade"`, cada mensagem num cliente só, e `pressao` (fora de qualquer
assinatura) sem gerar linha. Fan-out num broker só, com `[stream:vibracao]
topics=sensores/vibracao/#` e `[stream:tudo] topics=#`:
`sensores/vibracao/eixo_x -> "vibracao", "tudo"` e `sensores/temp/sala ->
"tudo"`. Confirmado de passagem que assinaturas sobrepostas **não** duplicam a
entrega do broker — a duplicação é só nossa, na gravação, que é o pretendido.

---

## 2.3 — Topologia de teste — FECHADO (2026-09-04)

Item de preparação, não de código, mas é o que torna o 2.1 verificável.

**Abordagem escolhida (2026-09-03): duas seções `[broker:*]` com nomes
diferentes apontando pro mesmo IP.** Não exige segundo broker instalado e já
exercita o que a fase entrega — N clientes, N threads, união de `topics=` por
broker, roteamento por broker e identidade no log.

**Duas consequências esperadas, pra não serem lidas como bug:**

- **Entrega dobrada.** As duas conexões assinam o mesmo broker real. Uma
  mensagem publicada uma vez chega **duas vezes**, uma por conexão, e é roteada
  para as streams de cada broker independentemente. Isso é a prova de que as
  duas conexões estão vivas e roteando em separado — mas o log vai mostrar
  linhas em dobro, e isso precisa estar claro antes de assustar.
- **Client id igual derruba tudo.** Se os dois clientes usarem o mesmo id, o
  broker derruba um a cada conexão do outro (MQTT 3.1.1 `[MQTT-3.1.4-2]`) e o
  resultado é um laço de reconexão que não se parece com um problema de id. Ver
  a primeira decisão do 2.1.

**Limite honesto desta topologia:** ela valida o encanamento, **não a
independência**. Como só existe um broker real, derrubá-lo mata as duas
conexões juntas — os cenários "um fora no boot" e "um caindo depois de
conectado", que são as decisões de boot do 2.1, **não são testáveis assim**.

**Decidido (2026-09-04): não subir a segunda instância.** A falha parcial fica
sem cobertura de teste nesta fase, por avaliação do André de que o
comportamento é previsível a partir do que já foi verificado. O raciocínio se
sustenta: cada cliente é independente — instância, thread, `client_id` e lista
de streams próprios —, então "um fora, um no ar" é a união de dois
comportamentos já medidos separadamente (retentativa com backoff no recusado,
boot não bloqueante no inalcançável).

**Coberto em 2026-09-04, sem segundo broker:** o ramo de **reconexão depois de
uma conexão estabelecida** — o caminho `ever_connected_ == true`, em que quem
reconecta é a lib e não o `tick()`. Era o último sem nenhum teste, porque exige
uma conexão que sobe e depois cai. Verificado à mão parando e religando o
mosquitto do `192.168.0.115` com o programa no ar. Exercita junto o
re-`subscribe` do `onConnect`, necessário porque `clean_session=true` faz o
broker esquecer as inscrições quando a conexão cai.

**A única lacuna que resta** é falha parcial entre brokers *independentes* — um
fora, outro no ar —, que exigiria a segunda instância recusada. Cada cliente é
independente (instância, thread, `client_id` e streams próprios), então esse
caso é a união de comportamentos já medidos em separado.

**Não transformar host:port duplicado em erro de config.** Hoje `loadBroker`
rejeita **nome** duplicado, não endereço duplicado — e é isso que permite esta
topologia. Se um dia parecer boa ideia validar endereço repetido, no máximo
`warn`, nunca erro.

**Publicador:** `src/publish_test.py` tem host e porta fixos no topo do arquivo
(decisão explícita de 2026-08-29, script independente do programa principal).
Com um broker só atendendo as duas conexões, ele serve como está — mexer nele
só é necessário se a segunda instância na porta 1884 entrar.

---

## Escopo da Fase 3 (definido 2026-09-04)

**Persistência em disco.** Portar o `SegmentWriter` para a base reescrita, com o
**formato v1 intacto** (`docs/formato_segmento.md` continua sendo a fonte de
verdade), e passar a ter **um writer por stream**. O sink deixa de ser o log.

Fora desta fase, explicitamente: **rollover**, **limite de fila** e **índice** —
os três estão na Fase 4/5 do `claude_memory/TODO.md` e assentam em cima do que
esta fase entrega.

O formato não muda. Isso é o que torna a fase barata de verificar: o
`src/read_segment.py` continua valendo byte a byte, e o ciclo de corrupção já
validado em 2026-08-29 (5 mensagens → reinício → 30 bytes de lixo → truncagem →
retomada no offset 8) vira teste de regressão pronto.

---

## Fase 3 — lista de tarefas

**3.1 — Camada de plataforma**

- [ ] `fsync` e `truncate` portáveis, hoje inline em `src/segment_writer.cpp:61-75`
      *(decisão: arquivo plano vs pasta; assinatura; o que fazer com falha)*

**3.2 — Porte do `SegmentWriter` (uma stream fixa)**

- [ ] Trazer `src/crc32.h` — dependência do writer, header-only
- [ ] Portar a classe sem redesenho: fila + thread, `push()`, `openSegment()`,
      `recoverExisting()`, `drainAndWrite()`, `maybeSync()`
      *(decisão: `write_interval`/`sync_interval` ficam constantes ou vão pro
      `iotrail.conf`)*
- [ ] Encerramento: drenar a fila e `fsync` final antes de sair
      *(decisão: e se não couber na janela do `CTRL_CLOSE_EVENT`)*
- [ ] Validar com `read_segment.py` e reproduzir o ciclo de recuperação

**3.3 — Um writer por stream** *(desenho decidido em 2026-09-04 — ver a seção)*

- [ ] Registro `nome da stream → SegmentWriter` no `main()`, criado **antes** dos
      clientes e destruído **depois** deles
- [ ] Seção `[global]` no `iotrail.conf`: `config.cpp:269-273` passa a tratar
      seção sem tipo como **singular** em vez de erro (nome desconhecido →
      aviso). O `ini.cpp` não muda.
- [ ] `data_dir` como chave de `[global]`, default `<exe_dir>/data`; relativo
      ancora no executável, absoluto passa direto
- [ ] `--config <caminho>` no `main(argc, argv)`, default
      `<exe_dir>/iotrail.conf`, relativo ancora no cwd, inexistente é fatal
      *(independe do writer — pode entrar junto do 3.1)*
- [ ] `Client` recebe pares `(const Stream*, SegmentWriter*)` no construtor;
      `matchAll` devolve pares e o `onMessage` chama `->writer->push(...)`
- [ ] Validar fan-out em disco: mesma mensagem em duas streams, cada uma com seu
      offset começando em 0
- [ ] Validar que uma stream cujo broker está fora do ar tem pasta e segmento
      criados no boot mesmo assim

**Fechamento**

- [ ] Registrar as decisões no padrão `FECHADO`
- [ ] Atualizar `claude_memory/CLAUDE.md` e o `TODO.md` do projeto

---

## 3.1 — Camada de plataforma

`fsync` e `truncate` são as duas operações que não existem em forma portável.
Hoje são dois `#ifdef _WIN32` inline no `segment_writer.cpp:61-75`
(`_commit`/`fsync`, `_chsize_s`/`ftruncate`), e o writer é o único consumidor.

Entra **antes** do porte de propósito: fazer depois significa editar o writer
duas vezes.

**Decisões:**

- **Arquivo plano `src/platform.h`/`.cpp` ou pasta `src/platform/`?** Pela regra
  fixada no item 1.1, uma dupla só fica plana — `config/` ganhou pasta por ter
  duas.
- **Assinatura: recebe `FILE*` ou um descritor inteiro?** Com `FILE*` o
  `_fileno`/`fileno` fica escondido dentro do módulo, que é o que o chamador
  quer. Com descritor, quem chama passa a precisar saber disso.
- **O que fazer quando falha?** Hoje os dois são inconsistentes: o truncate
  devolve `bool` e é checado, o `syncToDisk` **ignora o resultado**. Falha de
  `fsync` é perda de durabilidade silenciosa — exatamente o que a arquitetura
  de `sync_interval` existe pra controlar. Propagar, logar, ou continuar
  ignorando?
- **Só estas duas operações, ou já abstrair abrir/criar arquivo?** Abrir é
  portável (`std::fopen`), então incluir seria antecipar.

---

## 3.2 — Porte do `SegmentWriter`, uma stream fixa

Porte sem redesenho, formato v1 inalterado. A classe antiga
(`src/segment_writer.h`/`.cpp`) já está validada ponta a ponta; o que muda é a
organização em volta.

**Decisões:**

- **Confirmar `std::FILE*` em vez de `std::ofstream`.** A escolha original foi
  essa justamente pra conseguir o descritor e chamar `fsync`/`truncate`. Com a
  camada 3.1 no meio, a razão continua valendo — mas vale dizer isso
  explicitamente em vez de herdar por inércia.
- **`write_interval` (300 ms) e `sync_interval` (1000 ms): constantes no código
  ou no `iotrail.conf`?** O `claude_memory/CLAUDE.md` descreve os dois como os
  controles de latência e de durabilidade do sistema — `sync_interval` define a
  janela máxima de dados não sincronizados numa queda de energia. Isso soa como
  config, mas é superfície nova e por stream ou global é outra pergunta.
- **A classe continua criando a própria thread no construtor?** É o que faz
  hoje. A alternativa é o `main()` ser dono das threads, o que deixaria o
  encerramento centralizado.
- **`push(std::string topic, std::string payload)` copia.** Com fan-out, a mesma
  mensagem casando em N streams vira N cópias do payload. Manter (simples,
  previsível) ou compartilhar (`shared_ptr<const std::string>`, uma alocação e
  N referências)? Só vale medir depois; a decisão aqui é se o formato do `push`
  já nasce preparado.
- **Encerramento com trabalho pendente.** Esta é a decisão mais séria da fase.
  Até agora parar era instantâneo — não havia nada a salvar. Agora `stop()`
  precisa drenar a fila e fazer o `fsync` final, e o item 1.3 registrou que no
  `CTRL_CLOSE_EVENT` o Windows mata o processo depois de **poucos segundos**.
  Com M streams são M filas e M `fsync`. O que fazer se não couber: limitar o
  tempo de drenagem e aceitar perda, ou tentar até o fim e arriscar ser morto no
  meio?

---

## 3.3 — Um writer por stream

**Decidido em 2026-09-04 (pelo André), antes do código: o writer não é membro do
`Client`.** O `main()` mantém um registro `nome da stream → SegmentWriter`,
criado **antes** dos clientes e destruído **depois** deles. O `Client` recebe
ponteiro, nunca posse.

O desenho oposto é o que sai por inércia — é o `Client` que já recebe as streams
dele (`src/main.cpp:39-44`), então o writer viraria membro dele. Quatro razões
para não:

- **Boot em fases.** O construtor do writer faz a varredura de recuperação
  (`src/segment_writer.h:14-17`, classe antiga): abre o último segmento, procura
  registro parcial no fim, retoma o contador de offset. É I/O de disco, e é onde
  aparecem "sem permissão em `data/`", "disco cheio" e "arquivo corrompido".
  Dentro do `Client`, esse trabalho cai no meio do `start()`, intercalado com
  `connect_async` e log de rede. Com o registro, o boot fica em três etapas
  limpas: ler config → abrir armazenamento (reportando **todas** as falhas de
  disco de uma vez, como o `config.cpp:264` já faz com as seções) → conectar.
- **A stream existe mesmo com o broker fora do ar.** Em `src/main.cpp:44-47`, um
  `start()` que falha destrói o `Client` ali mesmo — e levaria junto os writers
  dele, deixando `data/<stream>/` sem criar. Mas a existência da stream é fato da
  config, não da conectividade: `docs/formato_segmento.md:48-50` diz que a pasta
  é a autoridade, e ela deve estar em disco desde o boot, vazia, esperando o
  broker voltar.
- **A stream é a unidade de escrita; o broker é transporte.** Uma pasta, um
  contador de offset (`formato_segmento.md:139-141`), uma retenção (Fase 8) e um
  índice (Fase 5) — tudo por stream, nada por broker. Prender o tempo de vida do
  writer ao `Client` inventa uma dependência que o formato em disco não tem.
- **Ordem de encerramento explícita.** Os writers têm que morrer depois dos
  clientes, senão um callback da lib empurra numa fila já destruída.
  `src/main.cpp:67-70` já para os clientes antes de limpar; o registro torna essa
  ordem obrigatória em vez de emergente.

**Forma que isso impõe:** o `Client` deixa de receber
`vector<const config::Stream*>` e passa a receber a stream **já pareada** com o
writer — um `struct { const config::Stream* stream; SegmentWriter* writer; }`. O
`matchAll` (`src/mqtt_client.cpp:44-65`) devolve esses pares e o `onMessage`
chama `->writer->push(...)` direto. Descarta as outras duas opções que estavam na
mesa: o sink `std::function` (desacopla MQTT de armazenamento, mas o 2.2 ensinou
que a opção mais desacoplada não é automaticamente a certa) e a busca por nome
num mapa (custo por mensagem, no caminho quente, com N threads de rede).

**Efeito colateral que vale registrar:** com o registro no lugar, *uma stream
alimentada por dois brokers* deixa de ser mudança estrutural. Os dois `Client`
apontariam para o mesmo `SegmentWriter`, e o `push` já é multi-produtor pelo
mutex de `src/segment_writer.cpp:244` (classe antiga) — o consumidor é uma
thread só, então a fila já nasce MPSC. **Não é para implementar agora**: a config
segue com `broker=` único e a rejeição de lista em `config.cpp:208-213` fica.
Mas a porta fica aberta de graça, em vez de exigir esta mesma refatoração depois,
com código rodando em cima. Discutido em 2026-09-04: descartada a proveniência
por registro (o admin da rede garante que os tópicos não colidem), o que sobrava
do multi-broker por stream era exatamente esta mudança de posse.

**Cuidado a carregar para o porte (3.2):** em `src/segment_writer.cpp:245`
(classe antiga) o `nowMillis()` é avaliado **dentro** do `lock_guard` da linha
244, na mesma expressão do `push_back`. Com um produtor por fila isso é
indiferente; com dois, é o que garante que a ordem dos offsets bata com a ordem
dos timestamps. Encurtar a região crítica tirando o carimbo de lá — reflexo
natural de quem otimiza — reintroduz reordenação silenciosa: timestamp andando
para trás enquanto o offset anda para frente. Manter, com comentário dizendo por
quê.

**Resolvida por consequência:** *uma stream sem broker vivo ainda cria pasta e
segmento no boot?* Sim — o registro cria todos os writers a partir de
`cfg.streams`, antes de qualquer cliente, e criar o writer cria a pasta e grava
o header de 14 bytes. Era decisão em aberto aqui; virou consequência do desenho.

**Decidido em 2026-09-04 (pelo André): `data_dir` é chave global do
`iotrail.conf`, e o próprio `iotrail.conf` ganha `--config`.** Hoje o `main()`
antigo passa `"data"` fixo (`src/main.cpp:29`, árvore antiga) e o `.conf` tem
local fixo (`src/main.cpp:25`).

| entrada | resolve para |
|---|---|
| `data_dir` ausente | `<exe_dir>/data` |
| `data_dir=logs` | `<exe_dir>/logs` — **nunca** o `./logs` do cwd |
| `data_dir=D:/iot` | `D:/iot` (absoluto passa direto) |
| `iotrail.exe` | `<exe_dir>/iotrail.conf` |
| `iotrail.exe --config D:/a.conf` | `D:/a.conf` |

A regra atrás das duas metades é a mesma — *configurável, com default ao lado do
executável* — aplicada em dois níveis. A assimetria de âncora é deliberada:
**valor que vem do arquivo ancora no executável; valor que vem da linha de
comando ancora no cwd.** O `.conf` é artefato de instalação e não sabe de onde o
processo foi lançado — é o problema que `paths::executableDir()` resolve em
`src/main.cpp:25`, e o mesmo que um `data_dir` relativo ao cwd reintroduziria
(rodar o `.exe` de outra pasta criaria um `data/` novo e o programa pareceria
vazio). Já `--config` é digitado num shell, onde caminho relativo significa "a
partir daqui" e qualquer outra âncora surpreenderia.

Consequências:

- **`main()` passa a receber `argc`/`argv`** — superfície nova; hoje é `main()`
  sem argumentos (`src/main.cpp:19`). Parsing mínimo, só `--config`; resistir a
  virar CLI genérica.
- **`--config` apontando para arquivo inexistente é erro fatal**, ao contrário
  do default ausente. Quem digitou o caminho quis aquele arquivo; cair no
  default em silêncio esconderia o erro de digitação.
- **A árvore do `data_dir` nasce sozinha:** o `create_directories` de
  `src/segment_writer.cpp:84` (classe antiga) já cria os pais junto.

**Onde mora a chave global — decidido em 2026-09-04 (pelo André): seção
`[global]`.**

```ini
[global]
data_dir=data

[broker:casa1]
...
```

Isto **não é exceção à regra do tipo**, e a primeira leitura de que era estava
errada. O `iotrail.conf:3-4` diz para que o tipo existe: desambiguar **famílias**
— há muitos brokers e muitas streams, e sem `broker:`/`stream:` um `[casa1]` não
diz de qual é. Uma seção global não tem essa ambiguidade: existe uma só. A regra
simplesmente não alcança ela. O que sai disso é uma gramática de duas formas, no
lugar de uma regra com asterisco:

```
[tipo:nome]   → uma instância de uma família   ([broker:casa1], [stream:temp])
[nome]        → seção singular, no máximo uma  ([global])
```

**O `ini.cpp` não muda.** Ele já devolve `type=""`, `name="global"`
(`ini.cpp:61-67`), e o erro de par fora de seção (`ini.cpp:98-103`) fica de pé,
intocado. Muda só o `config.cpp:269-273`: seção sem tipo deixa de ser erro e
passa a ser seção singular — `[global]` é conhecida, e qualquer outra vira
**aviso de nome desconhecido**, o mesmo tratamento que `config.cpp:274-277` já dá
a tipo desconhecido.

**Descartado: pares soltos antes da primeira seção** (o INI clássico). Defeito
que o `[global]` não tem: chave global escrita **depois** de uma seção entra
calada na seção errada — um `data_dir=` no fim do arquivo viraria chave de
`[stream:umidade_todos]`, e o único sinal seria o aviso de chave desconhecida, se
alguém estiver olhando. Com seção explícita, tirar a chave do bloco é uma edição
visível. **Descartado também `[global:iotrail]`**: não mexe em regra nenhuma, mas
o `nome` deixa de carregar informação.

**Ganho de lado:** a forma singular é o lugar pronto para o que a Fase 6/7 vai
pedir — `[http]` com porta e token, `[logging]` com nível. Sem ela, ou vira tudo
`[global]` (gaveta de bagunça) ou a mesma pergunta volta.

---

## 3.4 — Validação

- [ ] `read_segment.py` — copiar pra base nova ou continuar usando o de `src/`?
      Ele lê o formato v1 e não precisa mudar.
- [ ] Reproduzir o ciclo de recuperação já validado em 2026-08-29, agora com o
      writer portado
- [ ] **Novo:** fan-out em disco — publicar num tópico que casa com duas streams
      e conferir os dois arquivos, cada um com offset próprio começando em 0
- [ ] **Novo:** encerrar com Ctrl+C sob carga e conferir que a fila foi drenada
      (o último offset gravado bate com a última mensagem publicada)

---

## Riscos e efeitos que esta fase introduz

Coisas que passam a valer a partir do writer, para não aparecerem como surpresa:

- **Contagem de threads.** Hoje são N (rede, uma por broker) + a do spdlog + a
  do `main()`. Passa a ter mais M, uma por stream. Na config de exemplo com 2
  brokers e 2 streams, seis threads no total.
- **O encerramento deixa de ser barato.** Primeira vez que parar tem trabalho a
  fazer, e a janela do `CTRL_CLOSE_EVENT` vira restrição real e não teórica.
- **Fila sem teto.** O `std::deque` cresce enquanto a produção superar a
  escrita. O limite é Fase 4, mas o risco de memória nasce aqui — vale decidir
  se um teto mínimo vem junto.
- **Fan-out multiplica escrita.** Uma mensagem casando em duas streams é gravada
  duas vezes, com dois `fsync` no ciclo. É o comportamento pretendido, mas o
  custo em disco e IOPS passa a ser real.
- **`matchAll` aloca um `vector` por mensagem** e agora está no caminho quente.
  Trocar por um buffer reaproveitado por cliente é candidato natural desta fase.

---

## Fora desta fase (para não esquecer a ordem)

Depois que a Fase 2 fechar: camada `platform/` (fsync/truncate) → porte do
`SegmentWriter` com formato v1 intacto → uma `SegmentWriter` por stream →
limite de fila → rollover → índice.

**Marco de paridade:** com o writer por stream funcionando, a base nova passa a
fazer estritamente mais que o `src/` antigo. É o momento de decidir se o `src/`
é aposentado — duas árvores convivendo é o risco de manutenção real desta
reescrita.

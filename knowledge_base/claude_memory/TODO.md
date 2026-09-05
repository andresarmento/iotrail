# IoTrail — TODO

Backlog derivado do roadmap em `README.md`. Fase 0 e Fase 1 concluídas
(programa principal `src/main.cpp` buildando via CMake, toolchain unificado,
lib MQTT e lib de logging escolhidas). Fase 2 parcial (conecta em broker
configurável, sink de print). Fase 4 iniciada — fila+writer real
(`SegmentWriter`) já gravando em disco as mensagens recebidas.

**Reordenação decidida em conversa (2026-08-29).** O formato do registro e o
header de segmento saíram da Fase 3 e viraram as **primeiras tarefas da Fase
2**, em versão provisória mas madura (com `offset` e `crc32`, que hoje não
existem). Motivo: rollover, limite de fila e índice (Fases 4/5) assentam
todos em cima do layout, e trocá-lo depois é a coisa mais cara de reverter.
**Multi-broker adiado pra pós-MVP** (segunda vez que é adiado). Na mesma
conversa, **multi-stream subiu da Fase 3 pra Fase 2** — apareceu a razão
concreta de particionamento que faltava (sensor de alta frequência isolado na
própria stream). Ordem de trabalho: formato/header + multi-stream (Fase 2) →
limite de fila → rollover (Fase 4) → índice (Fase 5).

## Fase 0 — Programas de teste iniciais
- [x] Escrever protótipo simples de escrita em segmentos sequenciais (formato bruto).
      `src/test_0/test_write_segment.cpp` — layout binário mínimo (timestamp,
      tópico, payload), lista fixa de mensagens simuladas, escrita direta sem
      flush/fsync. Validado com `Format-Hex`/`xxd`. Leitor em
      `src/test_0/read_test_segment.py`.
- [x] Evoluir o protótipo introduzindo a fila em memória + thread consumidora.
      `src/test_1/test_write_segment.cpp` — thread produtora simula o cliente
      MQTT alimentando uma fila (`std::deque`) em ritmo fixo (`kProduceInterval`);
      thread consumidora acorda a cada `kWriteInterval`, drena a fila e grava
      (polling, sem condition_variable — decisão consciente). Timestamp é
      capturado na produção (não na escrita), validado no leitor Python
      (`src/test_1/read_test_segment.py`) — registros saem espaçados no tempo.
- [ ] Evoluir o protótipo para um formato de armazenamento mais próximo do layout
      final em disco (ver `docs/motivacao.txt`).
- [x] Protótipo de cliente MQTT real (test_3) — teste **isolado** de conexão +
      subscribe com `libmosquittopp` (MSYS2 ucrt64), validado contra o broker
      mosquitto real na rede local (`192.168.0.115:1883`), MQTT 3.1.1
      (default da lib). `src/test_3/test_mqtt_connect.cpp` — connect/CONNACK,
      subscribe/SUBACK e `on_message` confirmados recebendo mensagem real
      publicada via `mosquitto_pub`. Ainda **não** integra com a fila/writer
      thread dos testes 1/2 — isso é passo seguinte, fora do escopo deste
      protótipo. Detalhes de build/link em `CLAUDE.md`.
      **Próximo passo:** parar de criar novos `test_N` e migrar pra codar a
      solução definitiva (Fase 1 em diante)** — rollover e limite de fila (ver
      notas na Fase 4) ficam pra essa fase de código real, não pra mais um
      protótipo.

## Fase 1 — Fundação do projeto
- [x] Escolher lib MQTT: **Mosquitto** (`libmosquittopp`), validada isolada no
      `test_3`. Descartado Eclipse Paho C++ (não chegou a ser avaliado — o
      teste com mosquitto já validou conexão real e não houve motivo pra
      comparar).
- [x] **Unificar toolchain em MSYS2 `ucrt64`** (decidido 2026-08-28, feito
      2026-08-28, PATH ajustado 2026-08-29): instalado
      `mingw-w64-ucrt-x86_64-gcc` completo via `pacman` (o pacote do
      mosquitto só trouxe `gcc-libs`/runtime, não o compilador — precisou
      instalar à parte). `CMAKE_CXX_COMPILER` era passado via `-D` na hora de
      configurar (não está fixo no `CMakeLists.txt`) — deixou de ser
      necessário em 2026-08-29 depois de instalar `mingw-w64-ucrt-x86_64-cmake`
      e `mingw-w64-ucrt-x86_64-ninja` via `pacman` e **atualizar o PATH
      persistente do usuário**: `C:\msys64\ucrt64\bin` adicionado,
      `C:\Program Files\mingw64\bin` (WinLibs) removido — agora `g++`/
      `cmake`/`ninja` resolvem pro MSYS2 automaticamente em qualquer terminal
      novo, sem flags. WinLibs continua instalado em disco (não
      desinstalado, só fora do PATH). `test_0`/`_1`/`_2`/`_3` continuam como
      estão (compilados originalmente com WinLibs) — não recompilados, já
      cumpriram o papel de protótipo; confirmado por sanity check (recompilar
      `test_2` com o `g++` do MSYS2 + `-static`) que o comando de build
      manual documentado no `CLAUDE.md` continua funcionando idêntico com o
      novo toolchain, caso precise recompilar algum no futuro.
- [x] **Padrão de linkagem** (decidido 2026-08-28, feito 2026-08-28):
      implementado no `CMakeLists.txt` raiz — `target_link_options` com
      `-static-libgcc -static-libstdc++`, libs de terceiros
      (`mosquittopp`/`mosquitto`) linkadas dinâmicas, e um
      `add_custom_command(POST_BUILD)` que copia as 7 DLLs necessárias (ver
      lista no `CMakeLists.txt`) + `iotrail.conf` pro lado do `.exe`
      automaticamente a cada build.
- [x] Definir estrutura de build (CMake) — `CMakeLists.txt` na raiz do repo,
      build gerado em `build/iotrail/` (Ninja generator).
- [x] **Separar `IotrailClient` e config em arquivos próprios** (decidido e
      feito em conversa, 2026-08-29): `src/iotrail_client.h`/`.cpp` (a
      classe) e `src/config.h`/`.cpp` (`struct Config` +
      `loadConfigs()`, renomeado de `BrokerConfig`/`loadBrokerConfigs()` em
      2026-08-29), `src/main.cpp` agora só orquestra (carrega
      config, cria o client, roda o loop). `.h`/`.cpp` juntos direto em
      `src/`, **sem** `include/` separado ainda — decisão explícita de
      adiar essa separação mais fina de pastas pra quando o código crescer
      mais (prematuro com só 3 arquivos hoje). `CMakeLists.txt` atualizado
      com os novos fontes. Validado rodando contra o broker real.
- [x] **Escolher lib de logging: `spdlog`** (decidido em conversa,
      2026-08-29). Motivos: multiplataforma (Linux e Windows, disponível via
      `pacman` no MSYS2 — `mingw-w64-ucrt-x86_64-spdlog`, mesmo caminho de
      instalação já usado pro mosquitto — e também via `apt`/Arch/Homebrew
      em Linux/macOS, não é lib "de Windows"), API `{}` (usa `fmt` por
      baixo, mais segura que `printf`), e **modo assíncrono** com thread
      dedicada + fila lock-free — evita que uma chamada de log bloqueie
      quem chamou, mesma lógica já registrada em `docs/decisao_sync_write.txt`
      (isolar quem recebe a mensagem MQTT de variância de latência; logar
      de forma síncrona no caminho de recebimento/escrita reintroduziria
      esse problema). Destino planejado: **console + arquivo rotativo
      simultâneos** (logger multi-sink), caminho do arquivo configurável
      (ex.: `logs/iotrail.log`, ao lado dos segmentos) — implementação do
      sink de arquivo/rotação fica pra quando a Fase 9 (logging
      operacional) for implementada de fato; a decisão de lib é a que sai
      fechada agora, junto com o raciocínio de destino, pra não trocar de
      lib no meio do caminho. **Integrada em `main.cpp` no mesmo dia
      (2026-08-29)**: todos os `printf` trocados por
      `spdlog::info`/`warn`/`error`, só sink console por enquanto (sink de
      arquivo rotativo fica pra Fase 9, como já planejado). Detalhes em
      `CLAUDE.md`.
- [x] Fixar padrão C++17 no build — `CMAKE_CXX_STANDARD 17` no `CMakeLists.txt`.
- [x] **Primeiro programa principal** (`src/main.cpp`, 2026-08-28): conecta no
      broker MQTT (host/porta lidos de `iotrail.conf`, movido da raiz pra
      `config/` em 2026-08-29),
      subscreve um tópico hardcoded (`kTopic = "#"` — decisão explícita, só
      broker é configurável por enquanto) e imprime as mensagens recebidas
      (`on_message`). Estrutura similar ao `test_3`, mas com config em
      arquivo em vez de argumentos de linha de comando. Ainda não grava em
      disco nem usa a fila/writer — só monitora. Validado rodando de
      `build/iotrail/` contra o broker real e publicando via `mosquitto_pub`.
      Detalhes de build/link em `CLAUDE.md`.
- [x] **`iotrail.conf` em formato INI com seções, planejado pra multi-broker**
      (decidido e implementado em conversa, 2026-08-28): cada seção `[nome]`
      é um broker (`type`/`host`/`port`). `type` valida contra `"mqtt"` (só
      suportado hoje); qualquer outro valor (ex.: `"mqtt5"`, ainda não
      implementado) é rejeitado com aviso `Protocolo nao suportado` e a
      seção é descartada. `loadConfigs()` em `src/config.cpp` já lê
      **todas** as seções válidas num `vector<Config>`, mas o `main()`
      só conecta na primeira — criar um `IotrailClient` por broker fica pro
      item abaixo (Fase 2), decisão explícita de não fazer agora. Validado
      com seção `mqtt5` sendo corretamente rejeitada e seção `mqtt` válida
      conectando normalmente. Detalhes em `CLAUDE.md`.
- [x] **Publicador de teste em Python** (`src/publish_test.py`, 2026-08-29):
      conecta no broker (host/porta **hardcoded** no topo do arquivo,
      decisão explícita de não ler `iotrail.conf` — script independente do
      programa principal) e publica valores sequenciais 0-99 em
      `iotrail/test`, em loop (`itertools.cycle`), intervalo configurável via
      `--interval-ms` (default 1000). Usa `paho-mqtt` (instalado via `pip`,
      não vem com o Python). Validado ponta a ponta: rodando junto com o
      `iotrail.exe`, as mensagens publicadas aparecem corretamente no
      monitor C++. Detalhes em `CLAUDE.md`.

## Fase 2 — Cliente MQTT + formato provisório do registro + multi-stream
Ordem de trabalho decidida em conversa (2026-08-29): o layout do registro e o
header de segmento vêm **antes** de tudo o mais desta fase — rollover, limite
de fila e índice (Fases 4/5) todos assentam em cima deles, e mexer no layout
depois é a coisa mais cara de reverter do projeto. Definição aqui é
**provisória mas madura** (campos fechados, spec escrita); refinamento fica
pra Fase 3.

**Multi-stream virou requisito na mesma conversa (2026-08-29).** O `CLAUDE.md`
mantinha "uma fila só até aparecer razão concreta de particionamento" — a razão
apareceu: numa rede com muitos sensores, o usuário quer agrupar tópicos MQTT em
quantas streams quiser, tipicamente isolando **um sensor de alta frequência na
sua própria stream** pra ele não atrasar a gravação dos outros. É exatamente o
head-of-line blocking que o `CLAUDE.md` já listava como a justificativa válida
pra múltiplas filas. **Não afeta o formato binário** (verificado campo a campo):
`offset` continua `uint64` — só passa a ser **por stream**, cada uma com seu
contador começando em 0; `topic` continua no registro e fica mais necessário,
já que uma stream agrupa vários tópicos; o nome da stream não entra em lugar
nenhum do arquivo (o caminho identifica).

- [ ] **Layout provisório do registro** (1ª tarefa da fase). Hoje são 14 bytes
      (`RecordHeader` em `src/segment_writer.cpp:27-31`): `timestamp_ms`,
      `topic_len`, `payload_len` — sem offset e sem checksum, os dois campos
      que as Fases 4/5/6 exigem. Layout (parte fixa **26 bytes**,
      little-endian explícito, `#pragma pack(1)` como hoje):
      ```
      [4]  crc32         uint32  — CRC de tudo depois DESTE campo
      [8]  offset        uint64  — absoluto, monotônico, nunca reusado
      [8]  timestamp_ms  uint64  — atribuído na chegada da mensagem
      [2]  topic_len     uint16
      [4]  payload_len   uint32
      [N]  topic
      [M]  payload
      ```
      **Cortados por não ter uso no curto prazo** (decidido 2026-08-29):
      `length` (redundante — o tamanho sai de `topic_len`+`payload_len`; a
      proteção contra *torn write* que ele daria vem de checar os dois campos
      contra `kMaxTopicLen`/`kMaxPayloadLen` antes de ler, e depois do CRC) e
      `flags` (só serviria pra compressão/tombstone, que é assunto de Fase 3).
      `offset` é absoluto **dentro da sua stream** (não relativo ao segmento
      como no Kafka): 8 bytes não pesam nesta escala e elimina bug de conversão
      nas Fases 5–7. Cada stream tem seu próprio contador começando em 0 — não
      existe offset global entre streams.
- [ ] **Header de segmento provisório** (**14 bytes**, uma vez no início do
      arquivo). Não existe hoje — o segmento começa direto no 1º registro.
      ```
      [0..3]   magic          "IOTR"   — valida que o arquivo é nosso no boot
      [4..5]   format_version uint16 = 1
      [6..13]  base_offset    uint64   — offset do 1º registro do segmento
      ```
      **Cortados** (mesma decisão): `header_len` e `reserved` (puro
      provisionamento pra v2), `header_crc32` (o header é escrito uma vez na
      criação do arquivo e sincronizado; não tem a exposição a torn write que
      o rabo do segmento tem) e `created_ms` — este último porque **o rollover
      vai ser só por tamanho** (decidido 2026-08-29), e ele só existia como
      gatilho de rollover por tempo.
      `base_offset` é **mantido** (decidido 2026-08-29). Como os segmentos são
      numerados sequencialmente (`segment-00000.log`, não pelo base offset —
      ver tarefa de adaptar o `SegmentWriter`), o header é a **única** fonte da
      faixa de offsets de um segmento: sem ele, descobrir onde um segmento
      começa exigiria varrer os registros. Custo irrelevante — o header é por
      arquivo, não por registro.
- [x] **CRC32 sem dependência nova** — `src/crc32.h`, feito 2026-08-29.
      Header-only, sem `.cpp` e sem mudança no `CMakeLists.txt`: a tabela de
      256 entradas é `constexpr` (gerada em tempo de compilação, sem custo de
      inicialização em runtime). Expõe `crc32(data, len)` e
      **`crc32Update(crc, data, len)`** — a forma incremental existe porque o
      CRC de um registro cobre uma faixa que fica em buffers separados na hora
      de gravar (os 22 bytes da parte fixa depois do campo `crc32`, mais o
      tópico, mais o payload), então o writer encadeia três chamadas em vez de
      concatenar tudo antes. Mesma convenção do zlib: `crc32Update(0, ...)`
      equivale a `zlib.crc32(dados)`.
      **Validado contra o `zlib.crc32()` do Python** (2026-08-29), que é o
      requisito real da spec — o leitor de referência tem que bater sem
      reimplementar nada. Todos os casos idênticos: valor canônico
      `"123456789"` = `0xCBF43926`, buffer vazio = `0x00000000`, tópico,
      payload, buffer único, e o **encadeamento em 3 pedaços** batendo com o
      buffer único (`0x31B9EC43`). Bit trocado muda o CRC.
- [x] **Escrever a spec em `docs/formato_segmento.md`** antes do código —
      feito 2026-08-29. Cobre convenções (little-endian, `pack(1)`), layout de
      diretório, header de segmento, registro, CRC32, limites de sanidade,
      rollover por tamanho, pseudocódigo da varredura de recuperação no boot,
      tabela dos campos deixados de fora com o motivo, e um leitor de
      referência em Python. É a fonte de verdade — writer C++ e leitor Python
      têm que bater byte a byte com ela.
- [x] **Adaptar `SegmentWriter` + leitor Python ao formato novo** — feito e
      validado 2026-08-29. `src/segment_writer.h`/`.cpp` reescritos:
      construtor virou `(dataDir, streamName)` e grava em
      `data/<stream>/<stream>-00000.log` (`std::filesystem::create_directories`
      cria a pasta); header de segmento gravado na criação; registro montado
      **num buffer único** com um `fwrite` só (o CRC fica no início do
      registro, então precisa ser calculado antes de gravar — e escrita única
      cria menos fronteiras de escrita parcial, que é o que o CRC detecta);
      `push()` agora **descarta com aviso** tópico/payload acima dos limites
      do §5 (gravar fora do limite produziria arquivo que a própria varredura
      rejeitaria no boot seguinte). `src/read_segment.py` é o leitor novo —
      os de `src/test_0`../`test_2` liam o layout antigo e ficam como estão.
      **Numeração sequencial** confirmada; o catálogo em memória
      (`base_offset → arquivo`) só faz sentido com rollover, então fica pra
      Fase 4 junto com ele.
- [x] **Varredura de recuperação no boot** — feito e validado 2026-08-29.
      `"wb"` (que truncava tudo a cada boot) virou `"r+b"` se o arquivo existe
      / `"w+b"` se não. Existindo, `recoverExisting()` valida magic +
      `format_version`, percorre os registros checando limites de sanidade →
      sequência de offset → CRC, trunca no primeiro inválido
      (`_chsize_s`/`ftruncate`) e retoma `nextOffset_` dali.
      **Validado ponta a ponta contra o broker real** (`192.168.0.115`), ciclo
      completo: (1) segmento novo, 5 mensagens, offsets 0–4, arquivo com 274 B
      = 14 + 5×(26+19+7); (2) reinício — não truncou, retomou em offset 5;
      (3) injetados 30 bytes de lixo no fim — o leitor Python parou em 8
      registros reportando `topic_len 6195 acima do limite` (o limite de
      sanidade pegando antes do CRC, exatamente o mecanismo do §5/§8 da spec);
      (4) reinício — `rabo corrompido: topic_len acima do limite (30 bytes
      descartados)`, truncou e retomou em offset 8; (5) mensagem nova gravada
      como offset 8, sequência contínua.
- [ ] **Config de streams no `iotrail.conf`** — seções passam a ser
      prefixadas por tipo, `[broker:nome]` e `[stream:nome]` (decidido e
      confirmado em conversa, 2026-08-29), em vez do `[nome]` solto de hoje.
      `loadConfigs()` (`src/config.cpp`) precisa distinguir os dois tipos.
      ```ini
      [broker:local]
      type=mqtt
      host=192.168.0.115
      port=1883

      [stream:vibracao]
      topics=sensores/vibracao/#

      [stream:telemetria]
      topics=sensores/temp/#, sensores/umidade/#

      [stream:resto]
      topics=#
      ```
      **Validar o nome da stream no `loadConfigs()`** (decidido 2026-08-29):
      ele vira nome de pasta e de arquivo, então nome inválido tem que falhar
      no boot com erro nomeando a seção culpada, não num `fopen` obscuro
      depois. Duas checagens: (a) só `[A-Za-z0-9_-]+` — no Windows
      `/ \ : * ? " < > |` são ilegais, e espaço/ponto no fim somem
      silenciosamente; (b) rejeitar os nomes reservados do DOS (`CON`, `PRN`,
      `AUX`, `NUL`, `COM1`–`COM9`, `LPT1`–`LPT9`, case-insensitive) — eles
      **passam** na regra (a) mas falham no `fopen` mesmo com extensão
      (`NUL.log` não é criável). Detalhes em `docs/formato_segmento.md` §2.
- [ ] **Roteamento tópico → stream, por ordem de declaração** (confirmado em
      conversa, 2026-08-29). A **primeira** stream cujo padrão casar leva a
      mensagem — estilo nginx/iptables. Declara-se o específico antes e o
      catch-all por último, que é exatamente o caso de uso (sensor rápido numa
      stream só dele, resto no catch-all). **Descartado "mais específica
      vence"**: "mais específica" é mal definida quando dois padrões com `+`/`#`
      se cruzam sem um conter o outro, e produz surpresa. **Descartado fan-out**
      (gravar nas duas). Tópico que não casa com nada: `spdlog::warn` e
      descarta — é lacuna de config, tem que aparecer no log, não sumir num
      catch-all implícito.
      **Usar `mosquitto_topic_matches_sub(sub, topic, &result)`**
      (`C:\msys64\ucrt64\include\mosquitto.h:2451`) — é função C livre, não
      método da classe, então a limitação do `m_mosq` privado documentada no
      `CLAUDE.md` (seção test_3) **não** se aplica: dá pra chamar direto do C++
      incluindo `<mosquitto.h>`. Não reimplementar matcher de wildcard MQTT à
      mão (casos de borda com `+` no meio, `#` só no fim, `$SYS`).
- [ ] **Uma `SegmentWriter` por stream.** A classe não precisa mudar — só
      instanciar N, cada uma com sua thread e sua pasta
      (`data/vibracao/vibracao-00000.log` — pasta por stream **e** nome da
      stream repetido no arquivo, decidido 2026-08-29; ver §2 de
      `docs/formato_segmento.md`). `main()` passa a ser dono de um
      `map<string, SegmentWriter>`; `IotrailClient::on_message`
      (`src/iotrail_client.cpp`) deixa de chamar `writer_.push(...)` direto e
      passa a rotear (casa o tópico, acha a stream, empurra naquele writer).
      É o isolamento que motiva a feature: o writer da stream do sensor rápido
      pode engasgar no disco sem atrasar as outras.
      **Pasta por stream deixa de ser adiada** — era pra ficar pra Fase 3
      enquanto existia uma stream só; com multi-stream virando requisito, entra
      agora. (O argumento continua valendo: layout de diretório não está no
      formato binário, muda sem bump de `format_version`.)
- [x] Conectar a um broker configurável — `iotrail.conf`, seção `[nome]` com
      `host`/`port` (formato INI com seções desde 2026-08-28, ver Fase 1),
      lido em `src/main.cpp`. Validado 2026-08-28.
- [x] ~~Subscrever tópicos configuráveis~~ — **absorvido pela config de
      streams** (2026-08-29). Deixa de ser item próprio: o `subscribe()` passa
      a ser a **união dos padrões `topics=` de todas as streams**, então não se
      configura tópico em dois lugares. Some o `kTopic = "#"` hardcoded de
      `src/iotrail_client.cpp`.
- [ ] Tratar reconexão e QoS — parcialmente coberto: `src/main.cpp` já chama
      `reconnect()` quando `loop()` retorna erro, mas sem backoff configurável
      (`reconnect_delay_set` não usado ainda) e QoS fixo em 0 no `subscribe()`.
- [x] Sink inicial: apenas print (validar fluxo de entrada) — `on_message` em
      `src/iotrail_client.cpp` loga tópico/payload. Validado 2026-08-28.
- [ ] **Investigar suporte a MQTT5** (adiado da Fase 1, decidido em conversa
      2026-08-28): a versão de `libmosquittopp` usada no `test_3` (2.0.18-4,
      via MSYS2) não suporta v5 — sem callbacks `_v5`, sem acesso a
      `mosquitto_int_option`/`MOSQ_OPT_PROTOCOL_VERSION=MQTT_PROTOCOL_V5` (só
      o `opts_set` deprecated, que só aceita V31/V311), e `m_mosq` é privado
      sem accessor. Verificar se o repositório upstream do mosquitto (GitHub)
      já atualizou o wrapper C++ com suporte a v5 em versão mais nova que a
      empacotada pelo MSYS2; se não, decidir entre usar a API C
      (`libmosquitto`) direto pras partes que precisam de v5, ou patchear o
      wrapper. Isso alimenta o arquivo de config da Fase 9 (escolher versão
      MQTT 3.1.1 vs 5.0 — ver decisão em `CLAUDE.md`).

### Adiado desta fase
- [x] ~~**Multi-broker de verdade — adiado pra pós-MVP**~~ — **feito em
      2026-09-04, na base reescrita** (`iotrail_refactory/`). O adiamento
      (decidido 2026-08-29, depois de já ter sido adiado uma vez em favor do
      `SegmentWriter`) foi revertido em conversa 2026-09-03: seu próprio motivo
      era que trocar `loop()` manual por `loop_start()` mexe no modelo de
      threading do `main()` — e numa base que estava sendo reescrita, **sem
      writer e sem fila ainda**, esse modelo não existia para ser mexido. O
      custo nunca estaria mais baixo; fazer depois seria mexer nele já com
      writer e fila montados em cima.
      **Como ficou:** um `mqtt::Client` por broker, todos ativos ao mesmo tempo,
      cada um com `loop_start()` (thread criada pela lib), `connect_async()`,
      `client_id` próprio e a lista de streams do seu broker. O `main()` deixou
      de rodar rede — só espera o sinal de parada e chama `Client::tick()`, que
      supervisiona a **primeira** conexão (o auto-reconnect da lib só cobre
      queda depois de conexão estabelecida, `mosquitto.h:1657`).
      **Usa a API C (`libmosquitto`), não o wrapper C++** — decidido 2026-09-03
      e verificado no header: `m_mosq` é `private` em `mosquittopp.h:84`, então
      nem herança alcança o handle, e o wrapper não tem `int_option`, callbacks
      v5 nem properties. Isso mantém o MQTT5 alcançável em vez de bloqueado.
      **Validado 2026-09-04** contra o broker real com dois clientes no mesmo
      IP (`casa1`/`temp` e `casa2`/`umidade`, `client_id` distintos), cada
      mensagem chegando a um cliente só e roteada para a stream certa.
      Detalhes e decisões em `iotrail_refactory/TODO.md`, itens 2.1 e 2.2.
      **Contexto original preservado abaixo, do adiamento:** não destravava
      nenhuma capacidade das Fases 3–6. `SegmentWriter::push`
      já é thread-safe (mutex em `src/segment_writer.cpp:77`), então N
      clientes concorrentes na mesma fila não exigem mudança no writer.
      Contexto original: `loadConfigs()` (`src/config.cpp`) já
      lê todas as seções do `iotrail.conf` num `vector<Config>`, mas
      `main()` só usa a primeira. Implementar um `IotrailClient` por broker
      (estratégia decidida em conversa, 2026-08-28: cada instância chama
      `loop_start()` — thread própria via pthread, dado pela lib — em vez do
      `loop()` manual atual; todas jogam mensagens na mesma fila
      fila/writer quando essa integração existir). Ver raciocínio completo
      em `CLAUDE.md`.

## Fase 3 — Formato de armazenamento (crítico)
Base provisória (layout do registro + header de segmento) sai já na Fase 2 —
ver acima. Sobra pra cá o que exige mais maturidade do resto do sistema:
- [ ] Revisar o layout provisório depois de rodá-lo de verdade (rollover +
      leitor + volume real) — promover `format_version` a 2 se algo mudar.
- [ ] **`topic` interned: registro guarda `topic_id`, não a string** (levantado
      e direcionado em conversa, 2026-09-03). É o principal candidato ao bump
      de `format_version` 2.
      **O problema, medido:** hoje o tópico é gravado inteiro em todo registro.
      Na validação de 2026-08-29 (`274 B = 14 + 5×(26+19+7)`) isso deu
      `topic_len` 19 contra `payload_len` 7 — o tópico é **2,7× o payload e 37%
      do registro**. A 1000 msg/s são ~4,5 GB/dia, dos quais ~1,6 GB são a mesma
      string repetida. Em cartão SD de borda isso é espaço, volume de escrita e
      desgaste de flash.
      **A troca é drop-in no layout:** `topic_id` (uint16) ocupa exatamente o
      lugar de `topic_len`, a parte fixa continua **26 bytes** e a string some.
      Registro vai de `26 + N + M` para `26 + M` — 52 → 33 bytes no caso medido.
      **Cuidado que quase passou batido:** a tabela **não** sai da config. A
      config declara *padrões*, não tópicos — as duas streams do `iotrail.conf`
      usam `#`, e com wildcard o conjunto real (`sensores/vibracao/eixo_x`,
      `eixo_y`, ...) é descoberto em runtime, ilimitado no caso do `topics=#`.
      Numerar os padrões no lugar dos tópicos não serve: um registro dizendo
      "casou com o padrão 1" não distingue `eixo_x` de `eixo_y`, que é
      exatamente a informação que o campo existe pra guardar. Consequência
      prática: a tabela não pode ser escrita na criação do segmento, porque
      naquele momento ainda não se sabe o que vai aparecer.
      **Direção escolhida: sidecar por stream** (inclinação do André,
      2026-09-03) — `data/<stream>/topics.idx`, append-only, id = posição na
      tabela. Os registros do segmento ficam **todos uniformes** (`26 + M`, sem
      exceção), que é a vantagem principal sobre a alternativa.
      **Custos a encarar quando implementar, não esquecer:** (a) o segmento
      **deixa de se explicar sozinho** — sem o sidecar é ilegível, o que
      contraria o princípio que justificou manter o `base_offset` redundante no
      header ("arquivo que sai do contexto se identifica sozinho"); (b) exige
      **fsync do sidecar antes** do primeiro registro que usa um id novo, senão
      um crash na ordem errada deixa registro apontando pra id que não existe.
      **Alternativa mantida no registro — dicionário inline:** a entrada é
      gravada como um registro especial imediatamente antes do primeiro uso do
      tópico; o leitor monta o mapa varrendo pra frente. Segmento continua
      auto-contido e append-only, e crash não perde nada por construção — o
      mapeamento está sempre definido antes do uso, na ordem do arquivo. Custo:
      dois tipos de registro no arquivo, o que exige o campo de tipo/`flags`
      cortado da v1 (ver bullet abaixo).
      **Sub-decisões em aberto:**
      (1) **Largura do id.** `uint16` mantém os 26 bytes redondos mas cria um
      teto de 65.536 tópicos por stream — e teto de formato é exatamente o que
      o §5 da spec evitou ao fazer `payload_len` ser `uint32` em vez de
      `uint16`. `uint32` (ou varint) tira o teto e custa 2 bytes. Decidir com o
      número real de cardinalidade em mãos.
      (2) **Escopo do id: por stream ou por segmento.** O sidecar por stream já
      implica por stream, que é o melhor pro índice da Fase 5 e pros cursores da
      Fase 7 (id estável entre segmentos); por segmento daria tabela menor ao
      custo de o mesmo tópico mudar de id a cada rollover.
      **Depende de medição, e é por isso que é Fase 3 e não Fase 2:** o ganho e
      a largura do id dependem da cardinalidade de tópicos por stream, que só a
      rede real revela. Stream com 3 tópicos: ganho enorme. Stream com tópico
      por dispositivo carregando UUID: o dicionário vira custo. **Instrumentar
      antes** — ao selar um segmento no rollover (Fase 4), logar quantos tópicos
      distintos apareceram e que fração dos bytes foi gasta em `topic`. Não
      toca no formato e faz a Fase 3 chegar com dado em vez de palpite.
- [ ] Reavaliar os campos cortados da versão provisória, se aparecer uso
      concreto: `flags` no registro (compressão de payload, tombstone),
      `length` no registro, `header_len`/`reserved`/`header_crc32` no header.
- [x] ~~Particionamento por tópico/stream, se aparecer razão concreta~~ —
      **a razão apareceu e o item subiu pra Fase 2** (2026-08-29): sensor de
      alta frequência isolado na própria stream. Ver as tarefas de config de
      streams, roteamento e `SegmentWriter` por stream lá em cima. Confirmado
      que **não** tem implicação no layout do registro, ao contrário do que
      esta linha supunha.
      **Terminologia fixada (2026-08-29): `stream` no código/config/caminhos**,
      "fita" só como palavra informal em PT-BR na conversa e nos docs — é o que
      o `CLAUDE.md` já fazia ("stream/fita"). `topic` está ocupado pelo MQTT
      (campo do registro), não serve pra nomear a fita como o Kafka faz.
      **Layout de diretório: uma pasta por stream**
      (`data/telemetria/segment-00000.log`), estilo Kafka — a pasta também
      abriga o índice da Fase 5. Nome da stream **não** entra no header do
      segmento (string variável em header fixo; o caminho do arquivo já
      identifica).

## Fase 4 — Escrita em segmentos sequenciais
- [x] Prototipar writer thread com fila em RAM e `write_interval` por polling
      (feito em `src/test_1/`, ainda com produtor fake — falta plugar cliente
      MQTT real na Fase 2).
- [x] Implementar `sync_interval` (fsync) independente do `write_interval`.
      `src/test_2/test_write_segment.cpp` — mesma fila + threads do test_1,
      mas troca `std::ofstream` por `FILE*` (necessário pra expor o descritor
      nativo pro sync) e adiciona `kSyncInterval`: a própria thread escritora
      checa os dois intervalos a cada acordar e chama `fflush` + `_commit`
      (Windows) / `fsync` (POSIX, via `#ifdef _WIN32`) quando o sync vence,
      mais um sync final garantido ao encerrar. Mesmo layout binário. Leitor
      em `src/test_2/read_test_segment.py`.
- [x] **Integrar fila+writer no código base**, plugado no cliente MQTT real
      em vez do produtor simulado (decidido em conversa, 2026-08-29 — feito
      no lugar de multi-broker, adiado pra depois). `src/segment_writer.h`/
      `.cpp` — classe `SegmentWriter`, mesma lógica de thread única
      (checa `write_interval`/`sync_interval` no mesmo loop) e mesmo layout
      binário de `test_1`/`test_2`, defaults iguais (`write_interval` =
      300ms, `sync_interval` = 1000ms — mesmo raciocínio de dimensionamento
      já registrado, não é escala nova). `IotrailClient::on_message`
      (`iotrail_client.cpp`) chama `writer.push(topico, payload)` além de
      logar. Grava em `segment-00000.log` (nome já no formato pensado pro
      rollover abaixo, mas hoje é sempre o único segmento — sem rollover
      ainda). `main.cpp` cria o `SegmentWriter` antes do `IotrailClient`
      (referência passada no construtor) e chama `stop()` no shutdown, após
      `client.disconnect()`. Validado 2026-08-29: rodando `iotrail.exe`
      contra o broker real, publicando 3 mensagens via `mosquitto_pub` e
      conferindo o `segment-00000.log` gravado com o leitor Python do
      `test_2` — timestamps batendo com o momento de chegada de cada
      mensagem. Detalhes em `CLAUDE.md`.
- [ ] Rollover de segmento **por tamanho** (decidido 2026-08-29 — por tempo
      foi descartado; só importaria em stream de baixo tráfego onde o segmento
      nunca enche, e é o que permitiu cortar `created_ms` do header de
      segmento). Fechar o arquivo/segmento
      atual (com sync final) e abrir o próximo (`segment-00001.log`,
      `segment-00002.log`, ...) ao atingir o limite. Pré-requisito pra Fase 5
      (índice por segmento só faz sentido com mais de um segmento). Decidido
      em conversa (2026-08-28): implementar no código base (pós-Fase 0), não
      como mais um protótipo `test_N`.
- [ ] Limite máximo de batch/fila (proteção contra picos de tráfego). Fila
      hoje (`test_1`/`test_2`) cresce sem limite — proteção é um teto de
      itens/bytes na fila; ao atingir, decidir política (descartar mensagem
      nova / bloquear produtor = backpressure / logar aviso). Mesma decisão
      acima: fica pro código base, não pra outro protótipo.

## Fase 5 — Índice
- [ ] Índice por segmento (offset/timestamp → posição no arquivo), começar esparso.

## Fase 6 — Leitura e replay
- [ ] Reader a partir de offset ou timestamp, entrega em ordem.
- [ ] **Acesso de consumidores: só via API HTTP + token, caminho único**
      (decidido em conversa, 2026-08-29, refinado no mesmo dia — ainda não
      implementado, só a direção). Descartado protocolo binário próprio
      estilo Kafka (`Fetch`/`OffsetCommit`/coordenação de grupo sobre TCP)
      — motivo de existir no Kafka é multi-broker + paralelismo entre
      múltiplas instâncias de um mesmo consumidor, cenário que não se
      aplica ao IoTrail (ver "Decisão de arquitetura: fila é por
      stream/fita" no `CLAUDE.md`). **Decisão refinada: um único caminho de
      acesso**, sempre via API HTTP (`GET /read?topic=x&offset=y&limit=n`)
      por cima da lib de reader (Fase 6) — mesmo pra consumidor rodando na
      mesma máquina (descartada a ideia inicial de dar acesso direto ao
      arquivo pro caso local; um caminho só é mais simples de auditar e de
      controlar offset, ao custo de um hop HTTP mesmo em local).
      **Token = identidade do consumidor** (não login/senha — peso
      desnecessário pro cenário de poucas aplicações internas confiáveis):
      cada request carrega o token, o servidor usa ele pra saber de quem é
      o pedido e pra qual offset consultar/gravar. **1 token = 1 consumidor
      = 1 stream de offset**, sem semântica de "grupo" dividindo trabalho
      entre instâncias (isso é o próximo nível de complexidade do Kafka,
      não necessário agora). Tokens configurados em arquivo próprio, mesmo
      padrão de seção `[nome]` do `iotrail.conf`.
      **Autenticação exige HTTPS/TLS** — token em texto plano por HTTP puro
      é interceptável na rede; sem TLS o esquema de token não protege nada
      de verdade.
      **Processo único, thread própria pro servidor HTTP** (refinado em
      conversa, 2026-08-29, depois de comparar com o Kafka de verdade):
      o broker Kafka roda tudo — produtores e consumidores — num processo
      só, isolando carga entre eles via *thread pools* separados
      (`acceptor`/`processor`/`KafkaRequestHandlerPool`), não via processos
      separados; escalar é adicionar mais nós ao cluster, não separar
      ingestão de leitura. Mesmo princípio já usado no `SegmentWriter`
      (isolar quem recebe a mensagem MQTT de variância de latência do
      disco, via fila+thread — `docs/decisao_sync_write.txt`) se aplica aqui:
      o servidor HTTP roda em thread(s) própria(s) dentro do **mesmo**
      `iotrail.exe`, não em processo separado — uma leitura pesada de um
      consumidor não deve travar o recebimento de mensagens novas do MQTT,
      mas não precisa de dois binários pra isso.
      **Candidato a lib**: `cpp-httplib` (single-header, sem dependências
      além de TLS, que já é transitiva via `mosquitto`) — evita puxar um
      framework HTTP pesado (Boost.Beast, Drogon) que o volume de
      consumidores do IoTrail não justifica.
- [ ] **Token com escopo (consumer vs admin)** — um "consumidor" pode ser
      uma interface de gerência do próprio IoTrail (decidido em conversa,
      2026-08-29), não só aplicação lendo dados. Gerência precisa de outro
      nível de acesso (ver status/métricas, não só ler segmentos), então
      "token = identidade" não basta — token precisa também dizer **o que**
      pode fazer. Mesmo padrão do `AdminClient` do Kafka: fala com o mesmo
      broker/porta/protocolo que produtores e consumidores, só que com
      outros tipos de request (`CreateTopics`/`DescribeConfigs`/
      `AlterConfigs`), autorizados via ACL por principal — não é outro
      servidor, é escopo checado por request. Campo `role=consumer` vs
      `role=admin` na seção do token no arquivo de config; endpoints de
      gerência (`/status`, `/metrics`, `/consumers` — offset de cada um)
      separados dos de dados (`/read`/`/commit`), mesmo servidor/processo,
      exigindo escopo `admin`. **Decisão explícita: gerência via API
      começa só-leitura** (status/métricas/offsets) — mutação de config em
      tempo real (hot-reload do `iotrail.conf` via API) fica de fora dessa
      primeira versão; hoje config continua sendo editar arquivo + reiniciar.

## Fase 7 — Consumo independente
- [ ] Persistir offset de cada consumidor. Ver decisão da Fase 6 acima —
      cursor por token, persistido do lado do servidor (`POST
      /commit?token=x&offset=y`). Como HTTP é stateless, não existe
      "sessão" de consumidor pra recuperar em caso de desconexão: o
      consumidor só chama `GET /read` de novo com o mesmo token e o
      servidor retoma do último offset commitado daquele token — sem
      cerimônia de reconexão.
      **Cursor é por (token, stream), não por token** (consequência do
      multi-stream, 2026-08-29): cada stream tem seu próprio contador de
      offset começando em 0, então "offset 500" só faz sentido qualificado por
      stream. Os endpoints da Fase 6 precisam do parâmetro
      (`GET /read?stream=vibracao&offset=500`), e um mesmo token que lê duas
      streams mantém dois cursores independentes.

## Fase 8 — Retenção
- [ ] Política de descarte de segmentos antigos (idade e/ou tamanho total em disco).

## Fase 9 — Configuração e operação
- [ ] Arquivo de config (broker, tópicos, caminhos, limites de retenção/segmento).
- [ ] Logging operacional.
- [ ] Fechar v0.1 usável.

## Pesquisa / decisões em aberto
- [ ] Confirmar que não há sobreposição forte demais com NATS JetStream, Redpanda,
      EMQX+persistência, VerneMQ (mencionado em `docs/motivacao.txt`, não foi
      aprofundado ainda).

## Marco MVP
Itens das Fases 1–6 completos = MVP (ingestão MQTT + persistência + replay).

# IoTrail — Memória Principal

## O que é
IoTrail é um sistema leve de persistência e streaming de eventos para ambientes IoT,
escrito em C++17. Ele se conecta como **cliente** a broker(s) MQTT já existentes,
subscreve tópicos de interesse e grava as mensagens em um **log append-only
sequencial** (segmentos + índices próprios), sem depender de banco de dados externo.

Inspirado em conceitos do Apache Kafka (offset por consumidor, replay, retenção),
mas propositalmente **muito mais simples e leve** — pensado para redes IoT de
pequeno/médio porte (dezenas de sensores, poucas mensagens/s) e edge computing,
não para o throughput que o Kafka resolve.

Resumo do pitch (ver `docs/motivacao.txt` para a conversa completa que originou o projeto):
> Uma camada leve e persistente de event streaming para IoT, que fica "depois" da
> entrega em tempo real do MQTT: persistência, histórico, replay e consumo
> independente por múltiplas aplicações (dashboard, analytics, IA, banco SQL).

## Por que existe (contexto de decisão)
Discussão registrada em `docs/motivacao.txt`:
- Para ~50 sensores publicando 1x/s (~4,3M msgs/dia), Kafka é "marreta grande demais".
- Existem soluções próximas (NATS JetStream, Redpanda, EMQX+persistência, VerneMQ),
  mas a proposta aqui é um projeto próprio, mais simples, em C++, desacoplado do broker.
- Nome escolhido: **IoTrail** (IoT + trail — "deixar uma trilha dos acontecimentos"),
  entre outras opções cogitadas (IoTape, EdgeLog, EventTape, TinyStream...).

## Arquitetura combinada (write/sync)
```
Broker MQTT → Cliente MQTT → Fila em RAM → Writer Thread
    a cada write_interval (ex: 10ms) → monta batch → write() → Page Cache
    a cada sync_interval  (ex: 500ms) → fsync()     → Disco (segment-xxxxx.log)
```
- Uma única writer thread por stream/fita.
- `write_interval` controla eficiência (batch de escrita).
- `sync_interval` controla durabilidade (fsync periódico) — define a janela máxima
  de dados não sincronizados em caso de queda de energia.
- Limite máximo de batch/fila como proteção contra picos de tráfego.
- Gravação sempre append-only e sequencial, sem banco de dados intermediário.

### Decisão de arquitetura: fila é por stream/fita, não por broker (2026-08-29)
Número de filas **não** é decidido pelo número de brokers — são eixos
independentes. O que define quantas filas existem é quantas **streams/fitas
lógicas** (unidades de gravação/replay/retenção) o projeto quiser manter,
e isso é uma decisão de **particionamento de armazenamento** (ainda em
aberto, não resolvida — só o modelo mental está registrado aqui):
- Com **um broker só**, ainda pode fazer sentido ter mais de uma fila **se**
  os dados forem particionados por tópico/grupo de tópicos — cada partição
  vira sua própria stream, com fila e writer thread próprios, permitindo
  replay/retenção independentes por partição (ex.: descartar telemetria
  verbosa mais cedo que alarmes críticos) e evitando head-of-line blocking
  (um tópico de alto volume atrasando a gravação de um tópico
  prioritário/baixo volume compartilhando a mesma fila).
- ~~Com **múltiplos brokers**, ainda pode fazer sentido ter **uma fila só**
  se o objetivo for um log unificado de tudo, sem distinção de origem.~~ —
  **superado em 2026-09-03: uma stream recebe de um broker só.** O log
  unificado continua possível, mas se monta na leitura, não na gravação. Ver
  "Vínculo stream ↔ broker" na seção Multi-stream abaixo.
- O que **não** justifica múltiplas filas: throughput. Na escala do
  projeto (poucos milhares de msgs/s no limite, ver
  `docs/decisao_sync_write.txt`), uma fila só dá conta perfeitamente — múltiplas
  filas não ganham nada em performance, só em isolamento/particionamento.
- ~~**Decisão atual: manter uma fila só**, até existir uma razão concreta de
  particionamento~~ — **superado em 2026-08-29: a razão apareceu.** Numa rede
  com muitos sensores, o usuário quer agrupar tópicos MQTT em quantas streams
  quiser, tipicamente isolando **um sensor de alta frequência na sua própria
  stream** pra ele não atrasar a gravação dos outros — exatamente o
  head-of-line blocking listado acima como a justificativa válida. Multi-stream
  virou requisito e subiu pra Fase 2 (ver `TODO.md`). Detalhes na seção
  "Multi-stream" abaixo.
- Uma suposição desta seção **não se confirmou**: particionamento *não* teve
  implicação no layout do segmento. Verificado campo a campo — `offset`
  continua `uint64` (só passa a ser por stream), `topic` continua no registro
  (fica mais necessário, já que uma stream agrupa vários tópicos), e o nome da
  stream não entra em lugar nenhum do arquivo. O que muda é o layout de
  **diretório**, que não está no formato binário.

### Multi-stream: agrupamento de tópicos MQTT em streams (2026-08-29)
Requisito confirmado em conversa. **Uma stream = uma fila = uma writer thread
= uma pasta = seu próprio contador de offset** (começando em 0; não existe
offset global entre streams).

- **Terminologia: `stream` no código, config e caminhos**; "fita" fica como a
  palavra informal em PT-BR na conversa e nos docs — que é o que esta seção já
  fazia ("stream/fita"). `topic` **não** serve pra nomear a fita como o Kafka
  faz: o termo está ocupado pelo MQTT e é campo do registro (`topic_len`/
  `topic`), a colisão seria confusa. `stream` é o termo padrão fora do Kafka
  (NATS JetStream, AWS Kinesis).
- **Config: seções prefixadas por tipo** — `[broker:nome]` e `[stream:nome]`,
  em vez do `[nome]` solto de hoje. Cada stream declara `topics=` com um ou
  mais padrões MQTT separados por vírgula.
- **Vínculo stream ↔ broker: um broker por stream** (decidido 2026-09-03).
  `broker=` é obrigatório na seção da stream e aceita **um nome só** — lista
  separada por vírgula é erro de config, não recurso. Sem default "o único
  broker declarado": escolher origem por omissão é o mesmo problema do `host=`
  sem valor — numa config com dois brokers, esquecer a linha faria a stream
  escutar o outro calada.
  **O motivo é assimetria, não gosto.** O registro guarda `topic`, não a origem
  (`docs/formato_segmento.md` §4), então agregar brokers numa stream descarta a
  proveniência **na gravação**, sem volta. O caminho inverso é barato: o log
  unificado de tudo se reconstrói **na leitura**, juntando N streams por
  timestamp. Um lado é adiável, o outro é irreversível. Se o log unificado
  virar requisito de verdade um dia, a resposta certa é um campo de origem no
  registro (Fase 3, revisar layout), não misturar às cegas.
  Três consequências que o 1:1 evita: **lacuna ambígua** (broker A cai, B
  continua, a stream segue avançando offset e nada no log distingue "sensor
  quieto" de "broker fora do ar"); **retenção e replay acoplados** entre
  origens, sendo a stream a unidade das duas coisas (Fases 6 e 8); e
  **head-of-line blocking voltando pela porta dos fundos**, agora entre redes
  de latências diferentes — exatamente o que multi-stream existe pra evitar.
  Quem quer gravar de dois brokers declara uma stream pra cada, com o mesmo
  `topics=`: custa uma pasta a mais e devolve proveniência de graça.
  Implementado em `iotrail_refactory/src/config/config.h` (`Stream::broker`,
  `std::string`) e `config.cpp` (`loadStream`). A lista é rejeitada com
  mensagem própria de propósito: sem essa checagem `casa,fabrica` viraria um
  nome literal e o erro sairia como "broker não declarado", que não diz ao
  operador o que ele fez de errado.
- ~~**Roteamento por ordem de declaração**: a primeira stream cujo padrão casar
  leva a mensagem, estilo nginx/iptables. Declara-se o específico antes e o
  catch-all (`topics=#`) por último. Descartado fan-out (gravar nas duas).~~ —
  **superado em 2026-09-04, implementado na base reescrita: roteamento é
  fan-out.** A mensagem vai para **todas** as streams cujo padrão casar, não só
  a primeira. Motivo: duas streams do mesmo broker cobrindo o mesmo tópico é
  escolha de quem configurou, e a intenção provável é guardar nas duas — com
  retenção e replay independentes por stream, isso faz sentido.
  **Consequência: a ordem de declaração deixou de decidir qualquer coisa.** Ela
  só existia para resolver a disputa entre duas streams que casavam com o mesmo
  tópico; sem disputa, some a regra — e com ela some a surpresa de um catch-all
  mal posicionado engolir tudo. A lista continua na ordem do arquivo, mas agora
  apenas para o log sair determinístico.
  **O que isso custa:** um `topics=#` convivendo com uma stream específica no
  mesmo broker faz o tráfego do sensor rápido ser gravado nas duas. O
  isolamento que motivou o multi-stream passa a depender de **padrões
  disjuntos**, não de precedência. Mais explícito, e o operador que quiser
  exclusividade escreve padrões que não se cruzam.
  Continua descartado "mais específica vence" (mal definida quando dois padrões
  com `+`/`#` se cruzam sem um conter o outro).
  **O casamento é sempre restrito às streams do broker de onde a mensagem
  veio** (precisado 2026-09-03, consequência do um-broker-por-stream): streams
  de brokers diferentes nunca se cruzam.
  **Por que o casamento é local e não vem do broker:** em MQTT 3.1.1 o
  `struct mosquitto_message` traz só `mid`, `topic`, `payload`, `payloadlen`,
  `qos` e `retain` — nenhuma indicação de qual assinatura provocou a entrega.
  Como há **uma conexão por broker** assinando a união dos padrões de todas as
  suas streams, descobrir o destino exige comparar a string do tópico contra os
  padrões de cada stream. As duas alternativas que dispensariam isso ficam
  registradas: **MQTT 5** (a propriedade *Subscription Identifier* volta em
  cada PUBLISH dizendo qual assinatura casou) ou **uma conexão por stream** (aí
  o próprio broker filtra e a conexão identifica o destino, ao custo de uma
  conexão TCP e um `client_id` por stream).
- ~~Tópico que não casa com nada: `warn` e descarta — lacuna de config tem que
  aparecer no log.~~ — **divergência consciente desde 2026-09-04**: hoje o
  descarte é **silencioso**. O `warn` por mensagem inunda o log quando um sensor
  não mapeado publica rápido, e a alternativa mais útil (logar só a primeira
  ocorrência por tópico) cobra estado mutável no caminho concorrente. Decisão
  adiada, com as quatro opções e o custo de cada uma registrados em
  `iotrail_refactory/TODO.md`, item 2.2. **Ao retomar, reconciliar com esta
  linha** — implementando o `warn` ou fechando a mudança de regra aqui.
- **Usar `mosquitto_topic_matches_sub()`** (`mosquitto.h:2451`) pro matching.
  É função C livre, não método da classe — a limitação do `m_mosq` privado
  documentada na seção "Protótipo test_3" **não** se aplica aqui, dá pra
  chamar direto do C++ incluindo `<mosquitto.h>`. Não reimplementar matcher de
  wildcard MQTT à mão.
- **`subscribe()` = união dos padrões das streams daquele broker** (precisado
  2026-09-03; era "de todas as streams", impreciso desde que cada stream passou
  a ter um broker só). Cada cliente assina a união das streams ligadas a ele —
  assinar padrão de stream de outro broker traria tráfego que seria descartado
  no roteamento. Com um broker só, os dois enunciados coincidem. Isso absorveu o
  item "subscrever tópicos configuráveis" da Fase 2 — não se configura tópico
  em dois lugares, a config de stream já diz o que o cliente precisa assinar.
- **`SegmentWriter` não muda** — instanciam-se N, um por stream, cada um com
  sua thread e sua pasta. `main()` vira dono de um `map<string, SegmentWriter>`;
  `IotrailClient::on_message` deixa de chamar `writer_.push(...)` direto e passa
  a rotear.
- **Nome de arquivo: pasta por stream + nome da stream repetido no arquivo**
  (`data/vibracao/vibracao-00000.log`), decidido 2026-08-29. A **pasta é a
  autoridade** — é ela que a retenção (Fase 8) varre e que o índice (Fase 5)
  acompanha; nenhuma operação por stream depende de glob sobre nomes. O nome no
  arquivo é redundância deliberada, mesmo raciocínio do `base_offset` no header:
  arquivo que sai do contexto se identifica sozinho, a custo zero.
  **Layout plano (`data/vibracao-00000.log`, sem pasta) foi descartado**: a
  retenção apagaria dados da stream errada quando um nome for prefixo de outro
  (`vibracao-*.log` casa com `vibracao-extra-00000.log`) — perda de dados
  silenciosa a partir de nomes que o operador escreve sem pensar.
- **Nome da stream é validado no `loadConfigs()`**, no boot: só
  `[A-Za-z0-9_-]+`, mais rejeição à parte dos nomes reservados do DOS (`CON`,
  `NUL`, `COM1`…), que passam na regra de caracteres mas não são criáveis no
  Windows nem com extensão. Ver `docs/formato_segmento.md` §2.
- **Consequência na Fase 6/7**: cursor de consumidor é por **(token, stream)**,
  não por token — "offset 500" só faz sentido qualificado por stream.

**Relação fila ↔ rollover:** são conceitos em camadas diferentes, não se
confundem. Uma fila (= uma stream lógica) é servida por **um writer thread
único e contínuo**; o **rollover** (Fase 4 do `TODO.md` — fechar o
segmento atual com sync final e abrir o próximo, `segment-00001.log`,
`segment-00002.log`, ...) é um detalhe interno de qual **arquivo físico**
esse writer thread tem aberto num dado momento, trocando ao longo do tempo
por tamanho/tempo. Modelo mental completo:
**1 fila = 1 writer thread = 1 stream lógica = N arquivos físicos ao longo
do tempo** (via rollover). Se o particionamento por tópico for adotado no
futuro, cada partição repete esse modelo inteiro (sua própria fila, writer
thread e sequência de rollover, independente das outras).

### Decisão de arquitetura: acesso de consumidores — só via API HTTP + token, caminho único (2026-08-29)
Discutido em conversa comparando com o modelo do Kafka (cliente conecta via
protocolo binário próprio sobre TCP — `Fetch`/`OffsetCommit`, mais um
sub-protocolo de coordenação de grupo pra dividir partições entre múltiplas
instâncias de um mesmo consumidor). Esse protocolo próprio existe no Kafka
por causa de roteamento entre múltiplos brokers e paralelismo entre
instâncias de consumidor — nenhum dos dois se aplica ao IoTrail (mesmo
raciocínio de "não é distribuído" já registrado na seção acima). **Decisão:
não inventar protocolo próprio, e um único caminho de acesso** (refinado no
mesmo dia — cogitado inicialmente ter dois caminhos, local via arquivo
direto e remoto via HTTP, mas descartado: um caminho só é mais simples de
auditar e de controlar offset, mesmo custando um hop HTTP a mais pro caso
local):
- **Sempre via API HTTP**, mesmo pra consumidor rodando na mesma máquina:
  `GET /read?topic=x&offset=y&limit=n` por cima da lib de reader (Fase 6),
  `POST /commit?token=x&offset=y` pro cursor. Reaproveita toda a infra de
  auth/TLS que já existe pra HTTP, em vez de desenhar protocolo binário e
  client library do zero pra cada linguagem que for consumir.
- **Token = identidade do consumidor, não login/senha.** Um sistema de
  usuário/senha de verdade (hash, salt, rotação) é peso desnecessário pro
  cenário do IoTrail — poucas aplicações internas confiáveis na mesma
  rede/edge, não multi-tenant público. Cada request carrega o token; o
  servidor usa ele pra saber de quem é o pedido e pra qual offset
  consultar/gravar — o token faz o papel de autenticação **e** de chave de
  identidade ao mesmo tempo. Tokens configurados em arquivo próprio, mesmo
  padrão de seção `[nome]` já usado no `iotrail.conf` pros brokers.
- **1 token = 1 consumidor = 1 stream de offset**, sem semântica de "grupo"
  dividindo trabalho entre múltiplas instâncias do mesmo token (isso seria
  o próximo nível de complexidade do Kafka - rebalanceamento entre
  instâncias de um grupo -, não necessário agora; reabrir se algum dia
  precisar paralelizar leitura de um mesmo fluxo).
- **Recuperação em desconexão é praticamente de graça, por ser stateless**:
  como HTTP não mantém uma conexão/sessão viva entre requests, não existe
  "sessão de consumidor" pra recuperar depois de cair. O consumidor só
  chama `GET /read` de novo com o mesmo token, e o servidor retoma a
  partir do último offset commitado daquele token — sem cerimônia de
  reconexão, diferente do modelo de sessão/heartbeat de grupo do Kafka.
- **Autenticação exige HTTPS/TLS.** Token em texto plano sobre HTTP puro é
  interceptável por qualquer um capturando pacotes na mesma rede — sem TLS
  o esquema de token não protege nada de verdade, é pré-requisito, não
  opcional.
- **Processo único, thread própria pro servidor HTTP — não processo
  separado** (refinado em conversa, 2026-08-29, depois de checar como o
  Kafka de verdade resolve isso). O broker Kafka roda ingestão (produtores)
  e leitura (consumidores) no **mesmo processo**, isolando a carga entre
  os dois via *thread pools* separados (threads de rede `acceptor`/
  `processor` + um pool `KafkaRequestHandlerPool` que processa os
  requests de uma fila compartilhada) — não via processos diferentes;
  escalar é adicionar mais nós ao cluster, não separar ingestão de
  leitura dentro de um nó. Mesmo princípio que já rege o `SegmentWriter`
  (isolar quem recebe a mensagem MQTT de variância de latência do disco,
  via fila+thread própria — ver "Decisão de arquitetura: por que manter
  fila + thread" mais abaixo) se aplica aqui: o servidor HTTP roda em
  thread(s) própria(s) dentro do **mesmo** `iotrail.exe`, não num binário
  à parte — uma leitura pesada de um consumidor não deve travar o
  recebimento de mensagens MQTT, mas não precisa de dois processos pra
  conseguir isso.
- **Candidato a lib**: `cpp-httplib` — single-header, sem dependências além
  de TLS (já é dependência transitiva via `mosquitto`/OpenSSL), evita
  puxar um framework HTTP pesado (Boost.Beast, Drogon) que o volume de
  consumidores do IoTrail não justifica.
- **Token com escopo (`consumer` vs `admin`)** — um "consumidor" pode ser a
  própria interface de gerência do IoTrail (insight de conversa,
  2026-08-29), não só aplicação lendo dados. Gerência precisa de outro
  nível de acesso (status/métricas do sistema, não só ler segmentos), então
  "token = identidade" sozinho não basta — o token também precisa dizer
  **o que** aquele acesso autoriza. Mesmo padrão do `AdminClient` do
  Kafka: fala com o mesmo broker/porta/protocolo de produtores e
  consumidores, só com outros tipos de request
  (`CreateTopics`/`DescribeConfigs`/`AlterConfigs`), autorizados por ACL
  por principal — não é outro servidor, é escopo checado por request.
  Campo `role=consumer`/`role=admin` na seção do token no config; rotas de
  gerência (`/status`, `/metrics`, `/consumers`) separadas das de dados
  (`/read`/`/commit`), mesmo servidor/processo, exigindo escopo `admin`.
  **Decisão explícita: gerência via API começa só-leitura** (ver
  status/métricas/offset de cada consumidor) — mutação de config em tempo
  real (hot-reload do `iotrail.conf` via API) fica fora dessa primeira
  versão; config continua sendo editar arquivo + reiniciar, como hoje.
- **Ainda não implementado** — só a direção de design fechada agora, pra
  não ter que reabrir essa discussão do zero quando a Fase 6/7 forem
  atacadas de fato. Ver itens correspondentes no `TODO.md`.

## Formato de armazenamento (a decidir com cuidado — é a parte mais cara de mudar)
Cada evento no segmento deve conter algo como:
`offset | timestamp | tópico | payload | tamanho | checksum`
Índice por segmento: offset/timestamp → posição no arquivo (pode começar esparso).

## Escolhas técnicas iniciais
- **C++17** com STL.
- **Python** para tooling (scripts auxiliares, testes, geração de dados).
- Biblioteca MQTT: **Mosquitto** (`libmosquittopp`), decidido na Fase 0/1 (ver seção "Protótipo test_3").
- Biblioteca de logging: **spdlog**, decidido na Fase 1 (ver seção "Decisão de lib de logging" abaixo).
- Metodologia: começar com programas de teste simples (escrita em segmentos
  sequenciais) antes de ir para o layout de disco definitivo.

## Roadmap (README.md)
0. Programas de teste iniciais (escrita em segmentos sequenciais, formato simples → formato real).
1. Fundação do projeto (build, estrutura de pastas, libs base, padrão C++17).
2. Cliente MQTT (conectar, subscrever, reconexão, QoS; sink inicial = print).
3. **Formato de armazenamento** (layout do segmento em disco) — decisão mais crítica.
4. Escrita em segmentos sequenciais (append-only writer, rollover por tamanho/tempo, flush/durabilidade).
5. Índice por segmento (offset/timestamp → posição no arquivo).
6. Leitura e replay (reader a partir de offset/timestamp, em ordem).
7. Consumo independente (offsets persistidos por consumidor).
8. Retenção (descarte de segmentos antigos por idade/tamanho).
9. Configuração e operação (arquivo de config, logging operacional) → fecha v0.1.

**MVP sugerido:** itens 1–6 (ingestão MQTT + persistência + replay).
Itens 7–9 tornam o sistema utilizável por múltiplas aplicações simultâneas.

## Estado atual do repositório
**Fase 0 concluída** — quatro protótipos lado a lado, cada um em sua pasta
(tanto em `src/` quanto no `build/` espelhado), sem build system (compilados
manualmente com o `g++` WinLibs, ver seções individuais mais abaixo):
- `src/test_0/` — escrita sequencial direta, sem fila/threads.
- `src/test_1/` — evolução com fila em RAM + thread consumidora + threads.
- `src/test_2/` — evolução com `sync_interval`/fsync independente do `write_interval`.
- `src/test_3/` — teste isolado de conexão MQTT real (`libmosquittopp`),
  validado contra broker de verdade. Não é evolução do writer (sem fila, sem
  disco) — encerra a fase de protótipos `test_N`.

**Fase 1 iniciada** — primeiro programa principal do projeto:
- `CMakeLists.txt` (raiz) + `src/main.cpp` — build via CMake, toolchain
  unificado no MSYS2 `ucrt64` (`C:\msys64\ucrt64\bin\g++.exe`; instalado o
  pacote `mingw-w64-ucrt-x86_64-gcc` completo, já que o pacote do mosquitto
  só trazia o runtime). Linkagem: `-static-libgcc -static-libstdc++` +
  `mosquittopp`/`mosquitto` dinâmicos, com as DLLs necessárias e o
  `iotrail.conf` copiados automaticamente pro lado do `.exe` a cada build
  (`add_custom_command(POST_BUILD)`). Build gerado em `build/iotrail/`
  (Ninja generator).
- `src/main.cpp` — conecta no broker (host/porta de `iotrail.conf`, raiz do
  repo), subscreve um tópico hardcoded (`kTopic = "#"`) e imprime as
  mensagens recebidas. Validado 2026-08-28 contra o broker real, recebendo
  mensagem publicada via `mosquitto_pub`. Detalhes completos na seção
  **"Programa principal — `src/main.cpp` + `CMakeLists.txt`"** mais abaixo.

**Fase 4 iniciada, multi-broker adiado (2026-08-29):** no lugar de
implementar multi-broker de verdade (item aberto da Fase 2), entrou
`src/segment_writer.h`/`.cpp` — a classe `SegmentWriter` traz a fila+writer
thread já prototipada em `test_1`/`test_2` pro código base, agora plugada
no `IotrailClient` real (`on_message` enfileira cada mensagem recebida).
Grava em `segment-00000.log`, ao lado do `.exe`. Rollover e limite de fila
continuam pendentes (ver `TODO.md`, Fase 4). Detalhes completos na seção
**"`SegmentWriter` — `src/segment_writer.h`/`.cpp`"** mais abaixo.

Detalhes de cada protótipo `test_N` (lib usada, achados, validação) nas
seções individuais mais abaixo. Achado de MQTT5 (limitação de
`libmosquittopp`) descoberto no `test_3`, adiado pra Fase 2 — ver `TODO.md`.

**Decisão de versão do protocolo MQTT (2026-08-28):** test_3 começa com
**MQTT 3.1.1** (protocol level `4` no CONNECT). Não existe negociação
automática de versão — o cliente pede um nível específico no CONNECT e o
broker rejeita (CONNACK de erro + fecha conexão) se não suportar aquele
nível exato; não há fallback implícito. "MQTT 4" nunca existiu como nome de
spec: a numeração pulou de 3.1.1 (2014, OASIS) pra 5.0 (2019) — o `4` é só o
valor do protocol level do 3.1.1 dentro do pacote CONNECT. Mosquitto (broker
e a maioria dos brokers modernos) aceita 3.1.1 e 5.0 simultaneamente sem
config especial, decidindo por conexão conforme o que cada cliente pede.
Plano futuro: arquivo de config pra escolher a versão MQTT — cogitado listar
3.1/3.1.1/5.0, mas **3.1 (protocol level 3) é considerado obsoleto na
prática** e provavelmente nem vale oferecer como opção; só 3.1.1 e 5.0
cobrem os casos reais.

## Protótipo test_0 — `src/test_0/test_write_segment.cpp`
Primeiro programa de teste: grava uma lista fixa de mensagens MQTT simuladas
(tópico + payload, sem cliente MQTT real) sequencialmente em
`build/test_0/test_segment.bin`. Executável e dados gerados ficam ambos em
`build/test_0/` (pasta gitignorada). Leitor em `src/test_0/read_test_segment.py`.

Layout de registro usado (endianness nativa, header compacto seguido dos blobs):
```
[8 bytes]  timestamp_ms  (uint64_t, epoch em ms, gerado no momento da escrita)
[2 bytes]  topic_len     (uint16_t)
[4 bytes]  payload_len   (uint32_t)
[N bytes]  topic
[M bytes]  payload
```
Campos mínimos por escolha (sem offset/tamanho-total/checksum ainda — isso é
assunto da Fase 3, formato de armazenamento "oficial").

Os 3 campos fixos (`timestamp_ms`, `topic_len`, `payload_len`) ficam agrupados
numa `struct RecordHeader` com `#pragma pack(1)` + `static_assert(sizeof(...) == 14)`
pra garantir zero padding, copiada pra um buffer de `char` via `std::memcpy`
(preferido a `reinterpret_cast` direto por evitar violação estrita de aliasing).
Só topic/payload (tamanho variável) ficam fora do header e são escritos à parte.

Escrita direta e ingênua, de propósito: um `std::ofstream` aberto uma vez,
`write()` por registro em loop, sem fila/batch/flush/fsync explícitos. O
`close()` via RAII no fim do escopo entrega o buffer ao SO, mas não força
gravação física em disco — ponto discutido e aceito conscientemente, já que o
objetivo aqui é validar o formato do registro, não a estratégia de persistência
(isso vem na Fase 4, com `write_interval`/`sync_interval`).

`outDir` é `"."` — o programa grava ao lado de onde for executado. Rodar sempre
de dentro de `build/` (ex.: `cd build; .\test_write_segment.exe` no PowerShell),
já que é lá que o `.exe` também vive.

Validado com `Format-Hex build\test_segment.bin` (PowerShell): campos batem
byte a byte com o esperado, sem padding entre eles.

## Protótipo test_1 — `src/test_1/test_write_segment.cpp`
Evolução do test_0: introduz uma **thread produtora** (simula o cliente MQTT
recebendo mensagens em ritmo fixo, `kProduceInterval`) empurrando `QueuedRecord`
(mensagem já com timestamp atribuído no momento da produção, não da escrita)
numa fila `std::deque` protegida por `std::mutex`, e uma **thread consumidora**
que acorda a cada `kWriteInterval`, drena a fila inteira e grava cada registro
(sem fsync ainda). Mesmo layout binário do test_0. Roda de dentro de
`build/test_1/` (mesma convenção `outDir = "."`). Leitor em
`src/test_1/read_test_segment.py`, aponta pra `build/test_1/test_segment.bin`.

Compilar com `-pthread` além do `-static` de sempre (necessário por causa de
`std::thread`).

## Protótipo test_2 — `src/test_2/test_write_segment.cpp`
Evolução do test_1: adiciona `sync_interval` (fsync periódico) independente
do `write_interval`, fechando o item pendente da Fase 4. Mesma fila + threads
produtora/consumidora do test_1; a diferença é que o arquivo é aberto como
`FILE*` (`std::fopen`) em vez de `std::ofstream`, porque `syncToDisk()`
precisa do descritor nativo pra chamar `_commit` (Windows, via `<io.h>`) ou
`fsync` (POSIX, via `<unistd.h>`), escolhido em tempo de compilação com
`#ifdef _WIN32`. A própria thread escritora — sem thread dedicada de sync,
pra não ter dois threads mexendo no mesmo `FILE*` sem sincronização — checa
os dois timers a cada vez que acorda: grava o que tiver na fila, e se já se
passou `kSyncInterval` desde o último sync, chama `fflush` + `_commit`/`fsync`.
Há também um sync final garantido ao sair do loop, pra não perder o último
lote na page cache se o loop terminar antes do próximo `sync_interval`
vencer. Mesmo layout binário do test_0/test_1. Roda de dentro de
`build/test_2/` (mesma convenção `outDir = "."`). Leitor em
`src/test_2/read_test_segment.py`, aponta pra `build/test_2/test_segment.bin`.

Defaults atuais: `kWriteInterval` = 300ms, `kSyncInterval` = 1000ms — dimensionados
em conversa pra um cenário hipotético de 500 sensores publicando entre 300ms e
5s (`write_interval` acompanha o sensor mais rápido, já que nessa escala
`write()` não ganha nada sendo mais agressivo; `sync_interval` é escolhido
pela janela de perda de dados aceitável, não por throughput — custo do
fsync/`_commit` não escala com volume de mensagens).

Validado rodando o executável (log mostra `sync (fsync) executado` no ritmo
esperado, mais o `sync final executado` ao encerrar) e conferindo os 8
registros com o leitor Python — timestamps batem com o ritmo de produção.

## Protótipo test_3 — `src/test_3/test_mqtt_connect.cpp`
Ruptura de propósito em relação a test_0/1/2: não é evolução do writer, é um
teste **isolado** de conexão MQTT real — só valida `libmosquittopp` (connect,
subscribe, `on_message`) contra um broker de verdade. Não usa fila, não
escreve em disco, não tem thread produtora/consumidora. A integração com o
pipeline fila+writer dos testes anteriores fica pra depois (Fase 2).

Classe `TestClient` herda de `mosqpp::mosquittopp`, sobrescreve `on_connect`,
`on_disconnect`, `on_subscribe`, `on_message` e `on_log` (este último só pra
debug — imprime os logs internos da lib, tipo "sending CONNECT"/"received
CONNACK"). `main` recebe host/porta/tópico por linha de comando (defaults:
`127.0.0.1`, `1883`, `iotrail/test3/#`), chama `mosqpp::lib_init()`/
`lib_cleanup()` (obrigatório pela lib), e roda um loop manual chamando
`client.loop(200)` a cada 200ms até `Ctrl+C` (`SIGINT` seta uma flag
`volatile sig_atomic_t`), com `reconnect()` automático se `loop()` retornar
erro. Sem `write_interval`/`sync_interval` aqui — não tem nada sendo escrito.

**Importante:** `stdout` precisa ser setado sem buffer
(`std::setvbuf(stdout, nullptr, _IONBF, 0)`) logo no início do `main` —
sem isso, a saída fica presa no buffer do C quando redirecionada pra
arquivo/pipe (não é TTY) e não aparece se o processo for encerrado à força
(caso comum aqui, já que o teste roda em loop até `Ctrl+C`/kill externo).

**Lib usada:** `libmosquittopp` do MSYS2, pacote
`mingw-w64-ucrt-x86_64-mosquitto` (repo `ucrt64`, versão 2.0.18-4) — inclui
`libmosquitto`(C)/`libmosquittopp`(C++), headers e DLLs, em
`C:\msys64\ucrt64\`. Achado importante ao ler `mosquittopp.h`: **esta versão
do wrapper C++ não suporta MQTT5** — não tem callbacks `_v5` (só os
clássicos: `on_connect`, `on_message`, etc.) nem expõe `mosquitto_int_option`
(a função C moderna que seta `MOSQ_OPT_PROTOCOL_VERSION` pra
`MQTT_PROTOCOL_V5`); o único setter de opções exposto na classe C++,
`opts_set()`, mapeia pra `mosquitto_opts_set` (função C **deprecated**), que
só aceita `MQTT_PROTOCOL_V31`/`V311`. Também não há getter público pro
`struct mosquitto*` interno (`m_mosq` é `private`, sem accessor), então não
dá pra chamar a API C diretamente através de uma instância da classe C++ sem
modificar a lib. **Consequência pra decisão de versão MQTT registrada
abaixo:** dá pra usar MQTT 3.1.1 (default da lib, não precisa setar nada,
confirmado no `mosquitto.h`: `MOSQ_OPT_PROTOCOL_VERSION` default é
`MQTT_PROTOCOL_V311`) sem problema com esse wrapper, mas suportar MQTT5 no
futuro (arquivo de config) vai exigir usar a API C (`libmosquitto`) direto
em vez de `libmosquittopp`, ou patchear o wrapper.

**Build/link (rota escolhida: linkagem dinâmica, decisão 2026-08-28):**
compilado com o `g++` de sempre (WinLibs, GCC 15.1.0), mas sem `-static`
completo — aponta pros headers/libs do MSYS2 e linka dinâmico contra as DLLs:
```
g++ -std=c++17 -Wall -Wextra -O2 -I C:\msys64\ucrt64\include ^
    src\test_3\test_mqtt_connect.cpp -o build\test_3\test_mqtt_connect.exe ^
    -L C:\msys64\ucrt64\lib -lmosquittopp -lmosquitto
```
Motivo de não usar `-static`: `libmosquittopp`/`libmosquitto` do MSYS2 só têm
`.dll`/`.dll.a` (import lib) disponíveis prontos pro cenário completo (com
OpenSSL etc.) — teria que compilar tudo da fonte pra estático. Como esse teste
é só validação isolada da lib (não é o código definitivo), aceitável ficar
dinâmico por ora; decisão de linkagem estática fica pra quando isso migrar
pro código base (Fase 2).

Consequência prática: o `.exe` depende de DLLs do MSYS2 que **não** estão no
`PATH` do sistema, então precisam ser copiadas pro lado do `.exe` (mesma
pasta) — descobertas com `objdump -p` (ferramenta do MinGW) checando
recursivamente as dependências do `.exe` e das próprias DLLs da lib:
`libmosquittopp.dll`, `libmosquitto.dll`, `libgcc_s_seh-1.dll`,
`libstdc++-6.dll`, `libcrypto-3-x64.dll`, `libssl-3-x64.dll`,
`libwinpthread-1.dll` (as `api-ms-win-crt-*.dll` são parte do Universal CRT
do Windows 10+, já presentes no sistema, não precisam ser copiadas).
`libwebsockets`/`c-ares`/`cjson`/`zlib` são dependências de build do pacote
mosquitto mas não aparecem no link do cliente — não usadas em runtime aqui.

**Validado (2026-08-28)** contra broker mosquitto real na rede local do
usuário (`192.168.0.115:1883`, sem TLS): `CONNECT`→`CONNACK(0)`,
`SUBSCRIBE`→`SUBACK`, e `on_message` disparado corretamente recebendo uma
mensagem publicada via `mosquitto_pub` (tópico `iotrail/test3/hello`, payload
de 63 bytes, conteúdo batendo exatamente com o publicado). MQTT 3.1.1
confirmado como protocolo negociado (default, nada setado explicitamente).

## Programa principal — `src/main.cpp` + `CMakeLists.txt`
Primeiro código da Fase 1 (2026-08-28) — não é mais um `test_N`, é o início do
binário definitivo do projeto (`iotrail`). Objetivo deste marco: validar o
build via CMake com o novo toolchain, com um programa mínimo que conecta,
subscreve e monitora (sem gravar nada ainda).

**Organização em arquivos (2026-08-29):** separado em três pares/arquivo
desde o início mínimo, `main.cpp` virou só orquestração:
- `src/iotrail_client.h`/`.cpp` — a classe `IotrailClient`.
- `src/config.h`/`.cpp` — `struct Config` e `loadConfigs()` (renomeado de
  `BrokerConfig`/`loadBrokerConfigs()` em 2026-08-29).
- `src/main.cpp` — `main()`: carrega config, instancia `IotrailClient`, roda
  o loop até `Ctrl+C`.
`.h`/`.cpp` ficam juntos direto em `src/` — **decisão explícita de adiar**
um diretório `include/` separado (mencionado no roadmap original da Fase 1)
pra quando o código crescer mais; com poucos arquivos, separar por tipo em
vez de por módulo só adicionaria navegação extra sem ganho.

Estrutura similar ao `test_3` (`IotrailClient` herda de
`mosqpp::mosquittopp` — obrigatório pelo design da lib, ela só entrega
callbacks via `virtual` sobrescrito, não tem registro por `std::function`/
lambda; ver `on_connect`/`on_disconnect`/`on_subscribe`/`on_message`, mesmo
loop manual `client.loop(200)` até `Ctrl+C`), com duas diferenças
deliberadas:
- **Broker(s) vêm de `config/iotrail.conf`** em vez de argumentos de
  linha de comando.
- **Tópico continua hardcoded** (`kTopic = "#"`, namespace anônimo em
  `iotrail_client.cpp`) — decisão explícita (2026-08-28) de manter o
  primeiro programa simples; só o broker é configurável por enquanto. Ver
  item correspondente na Fase 2 do `TODO.md`.

**`iotrail.conf` — formato INI com seções, planejado pra multi-broker
(2026-08-28):** cada seção `[nome]` descreve um broker —
`type` (protocolo: só `"mqtt"` suportado hoje; `"mqtt5"` cai no mesmo
problema já documentado na seção "Protótipo test_3" — `libmosquittopp` não
suporta v5), `host`, `port`. `loadConfigs()` em `config.cpp`
faz o parsing de todas as seções pra um `vector<Config>` e **valida** cada
`type`: qualquer valor diferente de `"mqtt"` é descartado com a mensagem
`Protocolo nao suportado` (exigida nesse formato exato, é a que aparece no
console) — não interrompe o programa, só ignora aquele broker. Testado
manualmente trocando o `iotrail.conf` temporariamente por um com uma seção
`type=mqtt5` (rejeitada) e outra `type=mqtt` (aceita e conectou normal).

**Importante — o parsing já suporta múltiplos brokers, o `main()` ainda
não:** `main()` pega só `brokers.front()` (a primeira seção válida) e
conecta nela; se houver mais de uma seção válida, imprime aviso mas ignora
o resto. **Decisão explícita (2026-08-28) de não implementar ainda a parte
de instanciar um `IotrailClient` por broker** — isso é o item "Multi-broker
de verdade" na Fase 2 do `TODO.md`. Estratégia já decidida pra quando isso
for implementado: um `IotrailClient` por broker (não precisa mudar a
classe, já recebe `Config` no construtor), cada um chamando
`loop_start()` da própria lib (spawna thread própria via pthread) em vez do
`loop()` manual de hoje, com todas as instâncias jogando mensagens
recebidas na mesma fila do `test_1`/`test_2` quando essa integração
existir — mesma lógica de "isolar quem recebe de variância de latência" já
registrada em `docs/decisao_sync_write.txt`, aplicada agora a isolar um broker
lento/instável de atrapalhar os outros.

`stdout` sem buffer (`std::setvbuf(..., _IONBF, 0)`) logo no início do
`main()` — mesmo motivo do `test_3` (saída redirecionada pra arquivo/pipe não
é TTY, fica presa no buffer se o processo for encerrado à força).

**`CMakeLists.txt`** (raiz do repo):
- `CMAKE_CXX_STANDARD 17`.
- Aponta pra `C:/msys64/ucrt64` (variável de cache `MSYS2_UCRT64`) pros
  headers/libs de `mosquittopp`/`mosquitto` — mesma raiz MSYS2 usada no
  `test_3`, agora formalizada como padrão do projeto (ver decisão de
  toolchain no `TODO.md` Fase 1).
- `target_link_options` com `-static-libgcc -static-libstdc++`; libs de
  terceiros linkadas dinâmicas (`mosquittopp`, `mosquitto`).
- `add_custom_command(TARGET iotrail POST_BUILD ...)` copia 7 DLLs
  (`libmosquittopp.dll`, `libmosquitto.dll`, `libgcc_s_seh-1.dll`,
  `libstdc++-6.dll`, `libcrypto-3-x64.dll`, `libssl-3-x64.dll`,
  `libwinpthread-1.dll` — mesma lista descoberta via `objdump` no `test_3`)
  e o `iotrail.conf` pro lado do `.exe` a cada build. Roda de dentro de
  `build/iotrail/` (mesma convenção `outDir = "."` dos protótipos).

**Configurar e buildar** (PowerShell) — desde 2026-08-29, `g++`/`cmake`/
`ninja` do MSYS2 `ucrt64` estão todos no PATH persistente do usuário (ver
"Build / ambiente" abaixo), então não precisa mais forçar
`CMAKE_CXX_COMPILER` nem ajustar `$env:Path` na mão:
```
cmake -S . -B build/iotrail -G Ninja
cmake --build build/iotrail
```
(Se algum terminal antigo ainda resolver `g++`/`cmake` pro WinLibs, é PATH
de sessão desatualizado — abra um terminal novo.)

**Validado (2026-08-28)** rodando `build/iotrail/iotrail.exe` contra o broker
real (`192.168.0.115:1883`, mesmo broker do `test_3`): config carregada de
`iotrail.conf`, `CONNECT`/`CONNACK`, `SUBSCRIBE`/`SUBACK`, e `on_message`
recebendo corretamente mensagem publicada via `mosquitto_pub`.

## Decisão de lib de logging: spdlog (2026-08-29)
Fecha o último item em aberto da Fase 1. Escolhida **spdlog** — instalável
via `pacman` (`mingw-w64-ucrt-x86_64-spdlog`, mesmo repo `ucrt64` já usado
pro mosquitto) e também disponível em `apt`/Arch/Homebrew em Linux/macOS,
não é lib específica de Windows. Motivos, em ordem de peso:
- **Modo assíncrono** (thread dedicada + fila lock-free): uma chamada de
  log não bloqueia quem chamou. Mesma lógica já registrada na seção
  "Decisão de arquitetura" abaixo (isolar quem recebe a mensagem MQTT de
  variância de latência) — logar de forma síncrona no caminho de
  recebimento/escrita reintroduziria o mesmo problema que a fila já existe
  pra resolver.
- API de formatação `{}` (usa `fmt` por baixo) — mais segura que `printf`
  (checagem de tipo em tempo de compilação) e mais legível que `<<` de
  `std::ostream`.
- Header-only por padrão (suficiente pro tamanho atual do projeto; dá pra
  compilar como lib separada depois se o tempo de build começar a incomodar
  com mais arquivos usando log).

**Destino planejado dos logs:** console + arquivo rotativo simultâneos
(logger multi-sink do próprio spdlog) — console útil em desenvolvimento,
arquivo é o que sobra quando o processo roda em background sem terminal
grudado (caso de uso real do projeto, ver "O que é" no topo deste arquivo).
Caminho do arquivo pensado como configurável (ex.: `logs/iotrail.log`, ao
lado dos segmentos de dados), com rotação por tamanho — mesma preocupação
de "não crescer sem limite" que a Fase 8 (retenção) já vai resolver pros
segmentos, aplicada agora ao log.

**Integrado em `main.cpp` no mesmo dia (2026-08-29)** — todos os `printf`
trocados por `spdlog::info`/`warn`/`error` (nível escolhido por mensagem:
`info` pro fluxo normal — config carregada, conectado, subscrito,
mensagem recebida —, `warn` pra situações degradadas mas não fatais —
config não encontrada, protocolo não suportado, múltiplos brokers
configurados mas só o primeiro usado, `loop()` com erro tentando
reconectar —, `error` só pro `connect()` falhando de cara). Só **sink
console** por enquanto (o default do `spdlog::info`/etc., sem chamar
`spdlog::stdout_color_mt` explicitamente — a lib já usa um logger default
com esse sink); sink de arquivo rotativo fica pra Fase 9, como planejado
acima. `test_3` continua com `printf` — não é código base, não foi migrado
(mesmo raciocínio de não recompilar/mexer nos protótipos antigos).

Duas configurações globais em `main()`, antes de qualquer log:
```cpp
spdlog::set_pattern("[%H:%M:%S] [%^%l%$] %v");
spdlog::flush_on(spdlog::level::trace);
```
- `set_pattern`: formato compacto (hora + nível colorido + mensagem) em vez
  do pattern default da lib (que inclui data completa e nome do logger).
- `flush_on(trace)`: força flush a cada chamada de log, qualquer nível.
  Necessário porque o `spdlog` **não** flusha automaticamente por padrão
  (só em `err`/`critical`) — sem isso, mensagens podem ficar presas em
  buffer, mesmo problema de visibilidade que já resolvemos com
  `std::setvbuf` no `test_3`/versão anterior do `main.cpp` (que usava
  `printf`). Como esse programa é um monitor de baixa frequência de
  mensagens, o custo de flush por chamada é irrelevante.

**Build:** pacote MSYS2 `mingw-w64-ucrt-x86_64-spdlog` (via `pacman`, puxa
`mingw-w64-ucrt-x86_64-fmt` como dependência — `spdlog` usa `fmt` internamente
pra formatação `{}`). Diferente do mosquitto, o `spdlog` tem um pacote CMake
próprio instalado (`C:\msys64\ucrt64\lib\cmake\spdlog\spdlogConfig.cmake`),
então o `CMakeLists.txt` usa `find_package(spdlog CONFIG REQUIRED)` (com
`CMAKE_PREFIX_PATH` apontando pro `MSYS2_UCRT64`) em vez de
`target_include_directories`/`target_link_directories` manuais — `mosquitto`
continua manual porque não tem esse pacote CMake. Só existe como `.dll`/
`.dll.a` (sem `.a` estática), mesma situação do mosquitto — linkagem
dinâmica, `libspdlog-1.17.dll` e `libfmt-12.dll` adicionadas à lista de DLLs
copiadas no `add_custom_command(POST_BUILD)`.

**Validado (2026-08-29)** rodando `iotrail.exe` contra o broker real —
saída com timestamp/nível (`[00:31:42] [info] [client] conectado...`),
mensagem recebida via `mosquitto_pub` aparecendo corretamente.

## Publicador de teste — `src/publish_test.py`
Ferramenta de teste em Python (2026-08-29), não faz parte do binário
`iotrail` — serve pra gerar tráfego MQTT real sem precisar de sensores de
verdade nem do `mosquitto_pub` na mão repetidamente. Publica valores
sequenciais `0..99` (`itertools.cycle`, reinicia em 0 ao chegar em 99) no
tópico `iotrail/test`, em loop até `Ctrl+C`, com o intervalo entre
publicações configurável via `--interval-ms` (default `1000`).

Host/porta do broker (`BROKER_HOST`/`BROKER_PORT`) são **hardcoded** no
topo do arquivo — decisão explícita (2026-08-29) de não ler do
`iotrail.conf`, pra manter a ferramenta simples e independente do programa
principal (não é uma decisão de arquitetura, só manter esse script pequeno).

Depende de `paho-mqtt` (`pip install paho-mqtt`, não vem com o Python —
instalado nesta máquina em 2026-08-29, versão 2.1.0). Usa a API nova da lib
(`mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, ...)`, `loop_start()` pra
rodar o processamento de rede em thread própria, igual ao padrão já adotado
no `IotrailClient` em C++).

**Importante:** rodar com `python -u` (ou `python3 -u`) quando a saída for
redirecionada pra arquivo/pipe — mesmo problema de buffering já visto no
`test_3`/`main.cpp` (stdout do Python também é bufferizado quando não é um
TTY; sem `-u`, a saída fica presa no buffer se o processo for encerrado à
força antes de terminar sozinho).

**Validado (2026-08-29)** ponta a ponta: rodando `src/publish_test.py` junto
com `build/iotrail/iotrail.exe`, as mensagens (`payload="0"`, `"1"`, `"2"`...)
apareceram corretamente no monitor C++, na ordem certa.

## Decisão de arquitetura: por que manter fila + thread mesmo com write imediato
Discussão completa registrada por escrito em `docs/decisao_sync_write.txt`
(anotações pessoais do usuário, não geradas por script). Resumo da
conclusão, importante pra não refazer essa análise do zero depois:

- **`write()` é barato** (só entrega pra page cache do SO) — bufferizar/atrasar
  escritas manualmente com um timer não ajuda em performance nessa escala (IoT,
  poucos milhares de msgs/s no limite). O `std::ofstream` já bufferiza
  internamente antes de fazer a syscall real; um `write_interval` manual só
  reproduziria isso de forma pior. Batching explícito só se justificaria em
  escala tipo Kafka de verdade (dezenas/centenas de milhares de msgs/s) ou com
  I/O direto contornando o page cache — não é o caso aqui.
- **Mas a fila continua valendo a pena**, e por um motivo diferente do que se
  pensava a princípio: não é sobre agrupar escritas por performance, é sobre
  **isolar quem recebe a mensagem (futuramente a callback do cliente MQTT) de
  qualquer variância de latência do disco**. Um `write()` normalmente rápido
  pode ocasionalmente travar (antivírus, contenção de filesystem, soluço de
  disco); se quem processa a rede for a mesma thread que escreve, esse
  travamento atrasa o recebimento de novas mensagens MQTT. A fila + thread
  consumidora dedicada evita esse acoplamento, independente do volume de
  sensores.
- Volume de dados em si (mesmo 1000 sensores em rajada) não é problema pro page
  cache do SO — os registros são pequenos (~20-40 bytes), muito abaixo dos
  limiares de "dirty page throttling" do kernel.
- Conclusão prática: manter a arquitetura atual do `test_1` (fila + thread
  consumidora com seu timer). O `write_interval` nesse desenho serve como
  controle de responsividade/tamanho de lote (salvaguarda), não como
  otimização de throughput. `sync_interval`/`fsync()` foi implementado no
  `test_2` (ver seção acima).
- Detalhe técnico do `fsync`, confirmado na implementação: `std::ofstream` não
  expõe fsync portável, só `flush()` (que não força gravação física). O
  `test_2` resolveu isso usando `FILE*` (`std::fopen`/`fwrite`) em vez de
  `std::ofstream`, pra poder chamar `_fileno`+`_commit` (Windows) via
  `#ifdef _WIN32`, com `fileno`+`fsync` como alternativa POSIX no `#else`.

## `SegmentWriter` — formato v1 em disco (2026-08-29, segunda versão)
Reescrito no formato de `docs/formato_segmento.md` (**a spec é a fonte de
verdade**; esta seção só registra o que mudou em relação à primeira versão,
descrita logo abaixo).

- **Construtor virou `(dataDir, streamName)`**, grava em
  `data/<stream>/<stream>-00000.log`, criando a pasta com
  `std::filesystem::create_directories`. `main()` passa `("data", "default")`
  fixo por enquanto — a config de streams é o bloco B da Fase 2.
- **Registro montado num buffer único, um `fwrite` só** (antes eram três:
  header, tópico, payload). Duas razões: o `crc32` fica no *início* do
  registro, então precisa ser calculado com o registro inteiro já montado; e
  uma escrita única cria menos fronteiras de escrita parcial — que é
  exatamente o que o CRC existe pra detectar.
- **`push()` valida os limites do §5 da spec** e descarta com `warn` o que
  passar. Necessário, não cosmético: gravar um registro fora do limite
  produziria um arquivo que a própria varredura de recuperação rejeitaria no
  boot seguinte.
- **`"wb"` → `"r+b"` / `"w+b"`.** O modo antigo truncava o segmento inteiro a
  cada boot. Agora: existe → abre sem truncar e roda `recoverExisting()`; não
  existe → cria e grava o header de segmento.
- **`recoverExisting()`** valida magic + `format_version`, percorre os
  registros checando limites de sanidade → sequência de offset → CRC, trunca
  no primeiro inválido (`_chsize_s` no Windows, `ftruncate` em POSIX — mesmo
  `#ifdef` já usado no `syncToDisk`) e retoma `nextOffset_` dali. É o que
  torna o offset confiável entre reinícios.
- **`src/crc32.h`** — header-only, tabela `constexpr`, valida contra
  `zlib.crc32()`. O writer usa a forma incremental só na varredura; na
  escrita o buffer já está contíguo.
- **`src/read_segment.py`** é o leitor do formato novo. Os leitores dos
  protótipos (`src/test_0`../`test_2/read_test_segment.py`) liam o layout
  antigo de 14 bytes por registro e ficaram obsoletos — não foram migrados,
  mesma política de não mexer nos protótipos.

**Validado ponta a ponta (2026-08-29)** contra o broker real: segmento novo
(274 B = 14 + 5×52 para 5 mensagens), reinício retomando em offset 5 sem
truncar, 30 bytes de lixo injetados no fim sendo detectados pelo leitor Python
(`topic_len acima do limite` — o limite de sanidade pegando antes do CRC) e
truncados pelo writer no boot seguinte, com a gravação continuando em offset 8
sem furo na sequência.

## `SegmentWriter` — primeira versão, layout de 14 bytes (2026-08-29)
Histórico: como o writer nasceu, antes da reescrita acima. O layout descrito
aqui **não é mais o formato em disco**.
Primeira integração real da fila+writer thread (Fase 4) no código base,
plugada num cliente MQTT de verdade em vez do produtor simulado do
`test_1`/`test_2`. **Decidido em conversa fazer isso agora no lugar do
multi-broker** (adiado — ver Fase 2 do `TODO.md`), já que o roadmap original
apontava multi-broker como próximo item da Fase 2, mas fila+writer é
avanço maior de valor imediato e não depende de multi-broker pra existir.

Mesma arquitetura e mesmo layout binário já validados no `test_2` (ver
seção acima) — não é reinvenção, é a versão "de verdade" do protótipo:
- Classe `SegmentWriter` encapsula fila (`std::deque<QueuedRecord>` +
  `std::mutex`) e a thread escritora própria (criada no construtor, parada
  em `stop()`). `push(topico, payload)` é o método thread-safe que
  `IotrailClient::on_message` chama pra enfileirar — timestamp atribuído
  ali, no momento em que a mensagem chega, mesma decisão do `test_1`/`test_2`.
- A thread interna (`writerLoop`) acorda a cada `write_interval`, drena a
  fila e grava; a cada `sync_interval` (checado no mesmo loop, sem thread
  dedicada — mesma razão já registrada na seção "Decisão de arquitetura"
  acima) força `fflush`+`_commit`/`fsync`. `stop()` sinaliza parar, dá
  `join()` na thread e faz um sync final antes de fechar o arquivo —
  cobre tanto o shutdown via `Ctrl+C` (`main()` chama `stop()` depois do
  `client.disconnect()`) quanto a destruição normal do objeto (destrutor
  chama `stop()` também, idempotente).
- **Defaults dos timers mantidos:** `write_interval` = 300ms, `sync_interval`
  = 1000ms — mesmo raciocínio já validado no `test_2` (dimensionar
  `write_interval` pelo sensor mais rápido esperado, `sync_interval` pela
  janela de perda de dados aceitável, não por throughput). Nenhuma premissa
  de escala mudou, então não havia motivo pra reabrir essa conta.
- **Arquivo de saída:** `segment-00000.log`, gravado ao lado do `.exe`
  (mesma convenção `outDir = "."` dos protótipos). Nome já no formato
  pensado pro rollover (`segment-00001.log`, ...), mas rollover **ainda
  não está implementado** — hoje é sempre um único segmento, sem limite de
  tamanho/tempo. Decisão explícita de não criar uma pasta `data/` separada
  ainda (prematuro, mesmo raciocínio de adiar separação de pastas já usado
  na organização de `src/`).
- `IotrailClient` passou a receber uma `SegmentWriter&` no construtor
  (`iotrail_client.h`) — referência, não dono: o ciclo de vida do
  `SegmentWriter` é controlado por `main()`, que cria o `SegmentWriter`
  antes do `IotrailClient` (pra existir antes de qualquer mensagem poder
  chegar) e chama `stop()` nele só depois de `client.disconnect()` no
  shutdown, pra não parar de escrever enquanto ainda pode chegar mensagem.
  `on_message` (`iotrail_client.cpp`) continua logando via spdlog **e**
  chama `writer_.push(...)` — as duas coisas acontecem, não é uma
  substituindo a outra.

**Validado (2026-08-29)**: rodando `iotrail.exe` contra o broker real
(`192.168.0.115:1883`), publicando 3 mensagens via `mosquitto_pub` no
tópico `iotrail/segtest` e conferindo `segment-00000.log` com o leitor
Python do `test_2` (`src/test_2/read_test_segment.py`, mesmo layout —
reaproveitado direto, sem precisar de leitor novo) — os 3 registros vieram
com tópico/payload corretos e timestamps espaçados no ritmo real de
chegada de cada mensagem (não no momento da escrita em lote).

## Build / ambiente
- **IntelliSense do VS Code (2026-08-29).** Resolvido o
  `cannot open source file "spdlog/spdlog.h"`: `.vscode/c_cpp_properties.json`
  (rastreado no git) + `set(CMAKE_EXPORT_COMPILE_COMMANDS ON)` no
  `CMakeLists.txt`. As três chaves fazem coisas diferentes e nenhuma é
  redundante:
  - `compilerPath` (`C:/msys64/ucrt64/bin/g++.exe`) — **é o que resolve os
    headers.** A extensão C/C++ pergunta ao próprio compilador quais são os
    diretórios de include do sistema, e o `g++` do MSYS2 já busca em
    `C:/msys64/ucrt64/include` por padrão.
  - `compileCommands` (`build/iotrail/compile_commands.json`) — traz os
    `-D` do build real (`SPDLOG_COMPILED_LIB`, `SPDLOG_FMT_EXTERNAL`,
    `SPDLOG_SHARED_LIB`, vindos do target CMake do spdlog) e o `-std`.
    **Não traz `-I` nenhum**: o CMake omite `${MSYS2_UCRT64}/include` da linha
    de compilação porque já é diretório implícito daquele compilador. Por isso
    `compilerPath` é obrigatório, não opcional.
  - `includePath` — fallback só pros fontes que não estão no
    `add_executable()` (protótipos `src/test_0`..`test_3` e arquivos novos
    ainda não adicionados), que não aparecem no `compile_commands.json`.
- **Toolchain único desde 2026-08-29: MSYS2 `ucrt64`.** `g++`/`cmake`/`ninja`
  estão todos em `C:\msys64\ucrt64\bin`, que está no **PATH persistente do
  usuário** (adicionado nessa data). O **WinLibs foi removido do PATH**
  (`C:\Program Files\mingw64\bin` — os binários continuam instalados em
  disco, só não estão mais no PATH; não foi desinstalado, só desativado do
  PATH). Motivo: evitar os dois toolchains MinGW-w64 coexistindo causando
  confusão sobre qual `g++` está sendo usado. Se algum terminal ainda
  resolver `g++`/`cmake` pro WinLibs, é sessão aberta antes da mudança —
  abrir um terminal novo resolve (mudança de PATH via registro não afeta
  processos já rodando).
- Confirmado (2026-08-29) que o comando de build manual abaixo (usado nos
  protótipos `test_0`/`_1`/`_2`) continua funcionando idêntico com o `g++`
  do MSYS2 no lugar do WinLibs — recompilei `test_2` como sanity check.
- Executáveis e dados gerados pelos protótipos vão para `build/` (gitignorado).
- **Importante nesta máquina (MinGW-w64/UCRT no Windows)**: o binário linkado
  dinamicamente falha ao rodar com `STATUS_DLL_NOT_FOUND` (DLLs do runtime do
  MinGW não estão no PATH). É preciso compilar com `-static`:
  ```
  g++ -std=c++17 -Wall -Wextra -O2 -static src/arquivo.cpp -o build/programa.exe
  ```
  Exceção: `test_3` linka dinâmico contra DLLs do MSYS2 (`libmosquittopp`
  não tem `.a` estática pronta pro cenário completo) — ver seção "Protótipo
  test_3" pro comando de build e a lista de DLLs que precisam ser copiadas
  pro lado do `.exe`.
- **MSYS2 instalado nesta máquina** em `C:\msys64`
  (`C:\msys64\usr\bin\bash.exe` pra rodar `pacman` etc.). É o único toolchain
  MinGW-w64 no PATH desde 2026-08-29 (ver nota de PATH acima) — o WinLibs
  ainda está instalado em disco (usado pra compilar `test_0..test_3`
  originalmente, não migrados), mas não está mais no PATH, então qualquer
  `g++`/`cmake`/`ninja` chamado sem caminho completo resolve pro MSYS2.
  Repositório relevante do MSYS2: `ucrt64` (runtime UCRT) — **não** usar o
  repo `mingw64` puro (runtime msvcrt, incompatível). `pacman` é usado tanto
  pro compilador/build tools (`gcc`, `cmake`, `ninja`) quanto pra libs de
  terceiros (mosquitto hoje, outras no futuro). Ao instalar/atualizar pacotes
  MSYS2, usar `pacman -Su`/`-Syu` (upgrade completo) em vez de instalar um
  pacote isolado quando houver conflito de versão de dependência (aconteceu
  2026-08-29: `gcc-libs` puxado como dependência de outro pacote ficou numa
  versão diferente do `gcc` já instalado, bloqueando a instalação de
  `cmake`/`ninja` até rodar o upgrade completo).
- **Usar PowerShell, não o Git Bash da ferramenta Bash, para compilar/rodar
  binários.** Foi observado que arquivos criados via ferramenta Bash (ex.: o
  `.exe` compilado, a pasta `data/` de uma iteração anterior) não persistiam no
  disco real visto pelo PowerShell — pareciam existir só numa sandbox/overlay
  temporária do Bash que depois foi resetada. `git status` rodado nos dois
  shells já bateu depois disso, mas por segurança: builds e execuções de
  programas C++ deste projeto devem ser feitos com a ferramenta PowerShell.

## Estrutura de arquivos relevante
- `README.md` — descrição do projeto e roadmap.
- `docs/motivacao.txt` — transcrição da conversa que originou o projeto, nome e
  arquitetura de write/sync. Fonte de verdade para decisões de design ainda não
  formalizadas em código.
- `docs/help/git.md` — cheatsheet pessoal de comandos git.
- `docs/help/markdown.md` — cheatsheet pessoal de sintaxe markdown/GitHub.
- `src/test_0/` — protótipo Fase 0 v1 (escrita sequencial direta).
- `src/test_1/` — protótipo Fase 0 v2 (fila em RAM + threads produtora/consumidora).
- `src/test_2/` — protótipo Fase 0 v3 (`sync_interval`/fsync independente do `write_interval`).
- `src/test_3/` — protótipo Fase 0 v4 (teste isolado de conexão MQTT real com `libmosquittopp`).
- `build/test_0/`, `build/test_1/`, `build/test_2/`, `build/test_3/` — executáveis e dados de cada protótipo (gitignorado).
- `CMakeLists.txt` (raiz) — build do programa principal (Fase 1 em diante).
- `src/main.cpp` — programa principal do projeto (`iotrail`): só orquestra (config, client, loop).
- `src/iotrail_client.h`/`.cpp` — classe `IotrailClient` (conexão/callbacks MQTT).
- `src/config.h`/`.cpp` — `struct Config` e `loadConfigs()` (parsing do `iotrail.conf`; renomeado de `BrokerConfig`/`loadBrokerConfigs()` em 2026-08-29).
- `src/segment_writer.h`/`.cpp` — classe `SegmentWriter` (fila + thread escritora, grava `segment-00000.log`; ver seção própria acima).
- `src/publish_test.py` — ferramenta de teste (Python) que publica valores sequenciais 0-99 em `iotrail/test`, host/porta hardcoded no arquivo.
- `config/iotrail.conf` — config de brokers MQTT, formato INI com seções `[nome]` (`type`/`host`/`port`). **Movido da raiz pra `config/` em 2026-08-29**: config é o que o operador edita em runtime, não entrada de compilação (por isso não foi pra `src/`), e não vai ser arquivo único — o `tokens.conf` da Fase 6 mora na mesma pasta. Convenção do Kafka (`config/server.properties`). O código **não** mudou: `src/main.cpp:27` lê `"iotrail.conf"` relativo ao cwd, e a cópia continua caindo ao lado do `.exe` via `POST_BUILD` — só o caminho de origem no `CMakeLists.txt` mudou.
- `build/iotrail/` — saída do build CMake (executável, DLLs copiadas, `iotrail.conf` copiado) (gitignorado).
- `.gitignore` — ignora `build/`.
- `docs/decisao_sync_write.txt` — anotações do usuário sobre a decisão write imediato
  vs `write_interval`/fila (ver seção "Decisão de arquitetura" acima).

## Convenções de trabalho
- Repositório em português (README, docs, mensagens de commit podem ser em PT-BR).
- Ver `claude_memory/TODO.md` para o backlog ativo.

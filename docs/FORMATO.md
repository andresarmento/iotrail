# Formato de segmento — IoTrail

`format_version = 1` · migrado da base de conhecimento em 2026-09-06 (tarefa 3.1)

Especificação do formato em disco dos segmentos. É a fonte de verdade para o
writer em C++ e para qualquer leitor — as implementações têm que bater byte a
byte com este documento.

**Origem:** `knowledge_base/docs/formato_segmento.md`, escrito em 2026-08-29 e
maduro, mas com âncoras para o código da rodada anterior e alguns pontos que a
revisão da 3.1 mudou. As mudanças estão marcadas com **[3.1]** ao longo do
texto, e indexadas no §11.

**Nada nunca foi gravado por este binário.** Não existe arquivo v1 no mundo que
precise continuar legível, então mudar campo agora custa zero. Depois da Fase 4
rodando com dados no disco, custa migração — é por isso que a revisão acontece
aqui e não depois.

---

## Visão geral

Quatro estruturas, dois arquivos por stream. O segmento guarda os registros; o
arquivo de metadados guarda os tópicos, uma vez cada, e os registros apontam
para ele por `topic_id`.

```
data/temperatura/
  temperatura.meta           metadados + topicos da stream, valem para todos os segmentos
  temperatura-00000.log      segmentos, numerados sequencialmente
  temperatura-00001.log
```

**SEGMENTO** — `<stream>-NNNNN.log`

header, 14 bytes, gravado uma vez na criação:

```
+--------------+-------+------------------------+
|    magic     |  ver  |      base_offset       |
|    "IOTR"    |  u16  |          u64           |
+--------------+-------+------------------------+
0              4       6                       14
```

registro, 28 bytes fixos + payload, colados um após o outro:

```
+---------+--------------+--------------+----------+-------------+-----------+
|  crc32  |    offset    | timestamp_ms | topic_id | payload_len |  payload  |
|   u32   |     u64      |     u64      |   u32    |     u32     |    M B    |
+---------+--------------+--------------+----------+-------------+-----------+
0         4              12             20         24            28       28+M
          |------------------------ faixa do CRC-32 -------------------------|
```

**METADADOS DA STREAM** — `<stream>.meta`

header, 12 bytes:

```
+--------------+-------+------------+---------------+
|    magic     |  ver  | header_len | next_topic_id |
|    "IOTM"    |  u16  |    u16     |      u32      |
+--------------+-------+------------+---------------+
0              4       6            8              12
```

tabela de tópicos, do byte `header_len` até o fim: entrada de 10 bytes fixos +
tópico, append-only, uma por tópico distinto:

```
+---------+----------+---------+------------------+
|  crc32  | topic_id |  t_len  |      topic       |
|   u32   |   u32    |   u16   |       N B        |
+---------+----------+---------+------------------+
0         4          8         10              10+N
          |----------- faixa do CRC-32 -----------|
```

Little-endian, sem padding, sem alinhamento. Nas duas estruturas que têm CRC-32
ele cobre a mesma faixa: do byte 4 até o fim — tudo menos ele próprio. Os dois
headers não têm CRC: no do segmento porque é imutável (§9), no de metadados
porque a única exposição é um campo cuja corrupção a leitura corrige (§5).

---

## 1. Convenções

- **Endianness: little-endian**, explicitamente, para todos os campos inteiros.
  Em x86 e ARM isso é no-op — a declaração existe para que o arquivo seja
  portável, não porque alguma plataforma alvo precise de conversão hoje.
- **Sem padding.** Os tamanhos deste documento são os tamanhos em disco.
- **Sem alinhamento.** Registros são gravados colados, um após o outro. Nem o
  header (14 bytes) nem a parte fixa do registro (26) são múltiplos de 8, então
  os campos `uint64` ficam desalinhados no arquivo — isso é intencional e não
  custa nada enquanto ninguém fizer `mmap` + cast. Como o código monta esses
  bytes é decisão da 3.2.
- Todos os offsets de byte neste documento são relativos ao início da estrutura
  descrita.

## 2. Layout de diretório

Uma pasta por stream, e o nome da stream **repetido** no nome do arquivo. O
nome vem da config (`[stream:nome]` no `iotrail.conf`) e não é gravado dentro
de nenhum arquivo — o caminho é quem identifica.

```
data/
  temperatura/
    temperatura.meta
    temperatura-00000.log
    temperatura-00001.log
  umidade_sala/
    umidade_sala.meta
    umidade_sala-00000.log
```

Três extensões, três ciclos de vida: `.log` são os segmentos, `.meta` é o
arquivo de metadados e tópicos da stream (§5, um por stream, sobrevive a
rollover e a retenção) e `.idx` será o índice `offset → posição` da Fase 5 (um
por segmento, derivável e descartável). O `.meta` é dado — apagou, os tópicos
dos registros viram números órfãos; o `.idx` é cache — apagou, o programa varre
e reconstrói.

A raiz é o `data_dir` da seção `[general]`, já resolvido para absoluto contra o
diretório do executável (`src/config/config.cpp:330-336`).

Segmentos são numerados **sequencialmente** a partir de `00000`, cinco dígitos
com zeros à esquerda. O número do arquivo não tem relação com os offsets que
ele contém — quem carrega essa informação é o `base_offset` no header (§3).

**A pasta é a autoridade.** É ela que agrupa, que a retenção da Fase 8 varre, e
que o índice da Fase 5 acompanha. Nenhuma operação por stream deve depender de
glob sobre nomes de arquivo.

**O nome no arquivo é redundância deliberada**, pelo mesmo motivo que o
`base_offset` continua no header apesar da numeração sequencial: um arquivo que
sai do seu contexto — copiado para investigação, citado num log de erro, aberto
solto num editor — se identifica sozinho. Custo zero, já que o nome da stream é
necessário para criar a pasta de qualquer forma.

**Layout plano foi descartado** (`data/temperatura-00000.log`, sem pasta).
Motivo principal: a retenção da Fase 8 apagaria segmentos da stream errada
quando um nome for prefixo de outro — `temperatura-*.log` casa tanto com
`temperatura-00000.log` quanto com `temperatura-externa-00000.log`. Perda de
dados silenciosa causada por dois nomes que o operador escreve sem pensar. Com
pasta, é estruturalmente impossível. Secundários: os `.idx` da Fase 5 dobrariam
a contagem de arquivos num diretório único, e extrair o nome da stream de volta
a partir do arquivo exigiria uma regra de parsing que a pasta dispensa.

### Nome de stream: caracteres aceitos

O nome vira nome de pasta e de arquivo, então é validado no boot — não deixar o
`fopen` falhar depois com mensagem obscura. **Já implementado na 1.6:**
`config::valid_stream_name()`, `src/config/config.cpp:63`.

1. **Caracteres: só `[A-Za-z0-9_-]+`.** No Windows `/ \ : * ? " < > |` são
   ilegais em nome de arquivo; espaço e ponto final também causam problema (são
   removidos silenciosamente do fim de um nome). A lista branca evita enumerar
   casos.
2. **Nomes reservados do DOS, rejeitados à parte** (`src/config/config.cpp:72`):
   `CON`, `PRN`, `AUX`, `NUL`, `COM1`–`COM9`, `LPT1`–`LPT9`. Eles **passam** na
   regra de caracteres acima, mas falham no `fopen` mesmo com extensão —
   `NUL.log` não é criável no Windows. Comparação **case-insensitive**.

### Fan-out: a mesma mensagem em N streams

Quando filtros de streams diferentes casam com o mesmo tópico, a mensagem vira
**N registros completos**, um por stream, cada um com seu offset. Tópico e
payload são duplicados integralmente, e cada writer faz seu próprio `fsync`.

É o desenho, não efeito colateral: stream é a unidade de offset, retenção e
replay, e deduplicar exigiria um log comum com indireção — que tiraria
exatamente a independência que justifica a stream existir. O custo maior no
edge é a escrita repetida no cartão, não o espaço.

O `iotrail.conf` de exemplo já tem o caso: `umidade/sala` e `umidade/#` no
mesmo broker. **[3.1] Fechado:** o aviso está no `iotrail.conf`, no comentário
de `topics` da seção `[stream:nome]` — o operador precisa saber disso ao
escrever a config, não ao olhar o consumo do cartão. Detectar a sobreposição
sozinho no boot é computável, mas daria falso positivo e não vale nesta fase.

## 3. Header de segmento (14 bytes)

Gravado uma única vez, no início de cada arquivo, no momento da criação, e
sincronizado ali mesmo — antes de qualquer registro entrar.

| offset | tam | campo | tipo | valor |
|---|---|---|---|---|
| 0 | 4 | `magic` | `char[4]` | `"IOTR"` (`0x49 0x4F 0x54 0x52`) |
| 4 | 2 | `format_version` | `uint16` | `1` |
| 6 | 8 | `base_offset` | `uint64` | offset do primeiro registro deste segmento |

O primeiro registro começa no byte 14.

**`base_offset`** é o que permite localizar um offset sem varrer arquivos. Como
o rollover é por tamanho e os registros têm comprimento variável, a quantidade
de registros por segmento varia — não existe divisão que diga em qual arquivo
mora um offset. No boot o programa monta um catálogo em memória lendo estes 14
bytes de cada segmento, e a busca da Fase 6 roda sobre ele.

**[3.1] O header é acelerador, não autoridade.** O `offset` do primeiro
registro está dentro da faixa coberta pelo CRC; o `base_offset` não está
coberto por nada. Onde os dois discordam, quem vale é o registro (§8).

## 4. Registro (28 bytes fixos + variável)

| offset | tam | campo | tipo | descrição |
|---|---|---|---|---|
| 0 | 4 | `crc32` | `uint32` | CRC de tudo a partir do byte 4 |
| 4 | 8 | `offset` | `uint64` | posição deste registro na stream |
| 12 | 8 | `timestamp_ms` | `uint64` | epoch em ms, atribuído na chegada |
| 20 | 4 | `topic_id` | `uint32` | entrada no dicionário da stream (§5) |
| 24 | 4 | `payload_len` | `uint32` | bytes do payload |
| 28 | M | `payload` | `byte[M]` | payload MQTT, opaco |

Tamanho total do registro: `28 + payload_len`.

### `crc32`

Cobre uma faixa **contígua**: do byte 4 do registro até o último byte do
payload. Ou seja, tudo menos o próprio campo `crc32`.

A posição no início é deliberada. Com o CRC no meio da parte fixa, a faixa
coberta ou vira descontígua (dois pedaços com um buraco), ou teria que excluir
`offset` e `timestamp_ms` da proteção — e `offset` corrompido é a pior
corrupção possível aqui, porque o índice (Fase 5) e os cursores de consumidor
(Fase 7) são ambos chaveados por ele.

Algoritmo: **CRC-32 IEEE 802.3** (o mesmo do zlib e do gzip), polinômio
refletido `0xEDB88320`, valor inicial `0xFFFFFFFF`, resultado invertido no
final. Tabela de 256 entradas, sem dependência externa. No lado Python,
`zlib.crc32()` produz exatamente este valor.

### `offset`

Monotônico, incrementa de 1 em 1, nunca é reusado. **É por stream** — cada
stream tem seu próprio contador começando em 0. Não existe offset global entre
streams; "offset 500" só faz sentido qualificado por stream.

### `timestamp_ms`

Capturado no momento em que a mensagem chega do broker, **não** no momento em
que ela é gravada. Registros drenados num mesmo lote preservam o espaçamento
temporal real de chegada.

**[3.1] Uma captura por mensagem, não uma por stream.** Já é assim: `arrived_ms`
é lido uma vez em `src/mqtt/client.cpp:170`, antes do laço que casa as streams
(`:188`). No fan-out, os N registros saem com timestamp idêntico — que é o
certo, e é o tipo de coisa que uma refatoração desfaz sem querer quando não
está escrito.

### `topic_id`

**[3.1] O tópico não é gravado no registro.** Ele vive uma vez no dicionário da
stream (§5) e o registro guarda só o id. A string se repetia em todo registro:
na medição de 2026-08-29 o tópico tinha 19 bytes contra 7 de payload — **2,7× o
payload e 37% do registro**. A 1000 msg/s são ~1,6 GB/dia da mesma string.

O id é atribuído na primeira vez que o tópico aparece, vale para a stream
inteira (não muda no rollover) e **nunca é reusado** — as regras estão no §5.

**Registro com `topic_id` desconhecido não é descartado.** Offset, timestamp e
payload continuam íntegros e provados por CRC; só o tópico não resolve. O leitor
devolve um marcador (`<topic_id 42 desconhecido>`). Jogar fora dado bom por
causa de um arquivo lateral seria a troca errada.

### `payload`

Sem encoding declarado, tratado como opaco — o IoTrail não interpreta conteúdo.

## 5. Metadados da stream (`<stream>.meta`)

Um arquivo por stream, ao lado dos segmentos. Duas partes: um header com campos
da stream e, logo depois, a tabela de tópicos append-only — é ela que dá sentido
ao `topic_id` do registro (§4).

### Header (12 bytes)

| offset | tam | campo | tipo | valor |
|---|---|---|---|---|
| 0 | 4 | `magic` | `char[4]` | `"IOTM"` (`0x49 0x4F 0x54 0x4D`) |
| 4 | 2 | `format_version` | `uint16` | `1` |
| 6 | 2 | `header_len` | `uint16` | `12` na v1 — onde a tabela começa |
| 8 | 4 | `next_topic_id` | `uint32` | id a atribuir ao próximo tópico novo |

**`header_len` é o que deixa os campos crescerem por versão.** Uma v2 acrescenta
campos no fim do header e aumenta o valor; leitor antigo pula o header pelo
tamanho declarado e continua lendo os tópicos, leitor novo lendo um arquivo v1
assume default nos campos que não existem. É o mesmo campo que o §9 cortou do
header do segmento — lá era provisionamento para uma v2 que não existia, aqui é
o mecanismo de extensão de um arquivo que já nasceu para ganhar campos.

**Sem CRC no header**, ao contrário das outras duas estruturas — ver o fim desta
seção.

### Tabela de tópicos

Entradas append-only, do byte `header_len` até o fim do arquivo. Uma por tópico
distinto, gravada na primeira vez que o tópico aparece.

| offset | tam | campo | tipo | descrição |
|---|---|---|---|---|
| 0 | 4 | `crc32` | `uint32` | CRC de tudo a partir do byte 4 |
| 4 | 4 | `topic_id` | `uint32` | id gravado explicitamente, ver abaixo |
| 8 | 2 | `t_len` | `uint16` | bytes do tópico, `1..max_topic_len` |
| 10 | N | `topic` | `char[N]` | tópico MQTT, sem terminador |

**Os dois arquivos compartilham `format_version`.** São um formato só em dois
arquivos; versioná-los em separado criaria combinações (segmento v1 com
metadados v2) que ninguém quer testar. O `magic` diferente existe só para o
arquivo se identificar sozinho.

### A tabela não sai da config

A config declara **padrões**, não tópicos. Com `topics=umidade/#` o conjunto
real (`umidade/sala`, `umidade/quarto`, ...) só é descoberto em runtime, e é
ilimitado no caso de `topics=#`. Numerar os padrões não serve: um registro
dizendo "casou com o padrão 1" não distingue `sala` de `quarto`, que é
exatamente a informação que o campo existe para guardar.

Consequência prática: a tabela **não pode ser escrita na criação do arquivo** —
naquele momento ainda não se sabe o que vai aparecer. Ela cresce durante a vida
da stream, uma entrada por tópico novo.

Pelo mesmo motivo, **nome do broker e padrões subscritos ficam de fora**: os
dois vivem no `iotrail.conf`, que é fonte viva. Um espelho de config no disco
envelhece na primeira edição do operador e passa a mentir para quem investiga.
Se um dia essa informação fizer falta, o lugar dela é campo novo de header na
v2, com natureza de evento datado ("no boot de tal dia esta stream estava ligada
ao broker X"), não de espelho.

### `topic_id`: explícito, `uint32`, nunca reusado

- **Explícito, não implícito na posição.** Custa 4 bytes por tópico distinto —
  ruído num arquivo de algumas centenas de entradas — e compra duas coisas: uma
  ferramenta de limpeza pode remover entradas sem renumerar as seguintes (com id
  posicional, remover uma entrada invalidaria todos os registros já gravados), e
  uma entrada com rabo corrompido deixa de deslocar a numeração de todas as
  outras — vira um tópico ilegível, não a tabela inteira errada.
- **`uint32`**, não `uint16`: 65 mil tópicos parece muito até aparecer uma
  stream com id de dispositivo no tópico. Teto de formato é exatamente o que o
  §6 evitou ao fazer `payload_len` ser `uint32`, e aqui custa os mesmos 2 bytes.
- **Nunca reusado.** Ids removidos por limpeza deixam buracos permanentes;
  reusar faria um registro antigo passar a apontar para outro tópico, em
  silêncio.
- **A tabela em memória é um map, não um vetor.** Consequência direta do item
  acima: depois de uma limpeza ela é esparsa.

### Por que `next_topic_id` é campo, e não conta

Derivar o próximo id de "maior id da tabela + 1" funciona enquanto nada é
removido, e quebra exatamente no caso que a limpeza cria: ids 0..99, a
ferramenta apaga os tópicos mortos 90..99, o maior id vivo passa a ser 89 — e o
próximo tópico novo recebe 90, que já foi de outro. Reuso silencioso.

Na leitura, o valor efetivo é:

```
next_topic_id = max(header.next_topic_id, maior id da tabela + 1)
```

Nenhum dos dois sozinho basta. O header segura o valor quando a limpeza removeu
os ids do topo; as entradas puxam para cima quando o header ficou atrasado por
um crash entre gravar a entrada e atualizar o header.

**Exposição aceita (decidida na 3.1):** `next_topic_id` é o único campo
reescrito, e sem CRC no header uma corrupção nele passa despercebida.
Corrompido para cima, gera buraco nos ids — inofensivo, ids esparsos já são
esperados. Corrompido para baixo, o `max()` acima conserta em todos os casos
menos um: depois de uma limpeza ter removido os ids do topo, daria reuso. É raro
ao quadrado — limpeza mais torn write de 4 bytes no começo do arquivo — e o
preço de cobrir seria um CRC num header de 12 bytes.

### Ordem de escrita: os metadados vão primeiro

**A entrada nova é gravada e sincronizada antes do primeiro registro que usa o
id.** Na ordem inversa, uma queda de energia entre as duas escritas deixa
registros apontando para um id que não existe.

O custo aparece só quando um tópico aparece pela primeira vez — em regime,
nunca. É a única vez que a ingestão paga um `fsync` fora do `sync_interval`.

### Recuperação

Mesma varredura do §8, e pelo mesmo motivo: append-only sofre torn write igual.
Lê o header, valida `magic`, `format_version` e `header_len`, e percorre as
entradas conferindo o CRC de cada uma. Entrada que reprova encerra a tabela ali,
e o arquivo é truncado no fim da última entrada íntegra.

Um tópico cuja entrada se perdeu volta a ser tratado como tópico novo e recebe
**id novo** — nunca o id perdido, pela regra do `max()` acima. Os registros que
usavam o id antigo caem no caso "`topic_id` desconhecido" do §4: mantidos, com o
tópico irresolvível.

### Limpeza é ferramenta offline

Depois que a retenção da Fase 8 apagar segmentos antigos, sobram entradas de
tópicos que não existem em nenhum segmento vivo. Removê-las exige varrer todos
os segmentos restantes da stream para saber quais ids ainda são referenciados —
trabalho de ferramenta, não do writer.

**Com o IoTrail parado.** Rodando junto, a ferramenta apagaria uma entrada no
mesmo instante em que o writer atribui um id novo. Ela também é a única coisa
que reescreve o arquivo inteiro, e tem que preservar o `next_topic_id` do header
— que a essa altura é o único registro de que os ids removidos existiram.

### O que não entra aqui

Registrado para não ser rediscutido: `created_ms` e contador de mensagens por
tópico são dado sem consumidor hoje — e o `header_len` torna barato adicioná-los
na v2 se aparecer uso. QoS e flag de retained são propriedade da mensagem, não
do tópico. Estado do writer (próximo offset, segmento atual) sai dos segmentos, e
duplicar criaria duas verdades que podem divergir.

### O que isso custa

O segmento **deixa de se explicar sozinho**. Sem o `.meta`, um `.log` copiado
para investigação vira offsets e payloads com números no lugar dos tópicos — e
isso contraria o princípio que justificou manter `base_offset` redundante no
header e repetir o nome da stream no arquivo (§2).

Aceito conscientemente, com a mitigação de que o `.meta` é minúsculo e mora na
mesma pasta: copia-se a pasta, não o arquivo. A troca é 33% do registro no caso
medido em 2026-08-29 (52 → 35 bytes), e no edge isso é espaço, volume de escrita
e desgaste de flash.

## 6. Limites

**[3.1] Duas categorias, que a versão anterior misturava numa tabela só.**

### Do formato — o leitor precisa conhecer

Servem para validar `payload_len` (no registro) e `t_len` (na entrada de
tópico) **antes** de o CRC poder validá-los (§9), na varredura de recuperação.
São constantes, e é isso que as faz servirem: mudam só com bump de
`format_version`.

| constante | valor | motivo |
|---|---|---|
| `max_topic_len` | 1024 | vale para o `t_len` do `.meta` (§5); tópicos MQTT reais ficam muito abaixo, e o teto duro do campo `uint16` é 65535 de qualquer forma |
| `payload_hard_max` | 1 MiB (1048576) | teto absoluto do formato — não é o limite de ingestão, ver abaixo |

**[3.1] Por que o teto de leitura é separado do limite de ingestão.** O limite
de payload tem dois papéis, e juntá-los num valor configurável abre um caminho
de perda de dados: sobe-se a chave para 4 MiB, gravam-se registros de 4 MiB,
baixa-se de volta para 1 MiB — e no boot seguinte a varredura vê um registro
legítimo, chama de corrupção e **trunca o segmento ali**. Config editada,
dados apagados, sem aviso.

Então a validação de leitura usa `payload_hard_max`, que nunca muda, e a
admissão usa a chave de config (abaixo), que vale só para o que entra. Subir o
teto do formato exige bump de `format_version`, e é por isso que ele fica em
1 MiB: já é ~1000× o payload de um sensor típico, e é o tamanho do lixo que a
varredura pode alocar no pior caso — uma vez, não por registro.

**[3.1] Fechado:** duas regras que a versão anterior deixava implícitas, e
implícito aqui vira bug de leitor.

- **`t_len == 0` é inválido.** MQTT 3.1.1 exige tópico com pelo menos um
  caractere, e o broker nunca entrega vazio — zero no disco só pode ser
  corrupção. Vira checagem da varredura do `.meta`, ao lado do `max_topic_len`.
- **`payload_len == 0` é válido e é gravado.** Payload vazio é mensagem MQTT
  legítima: é como se apaga um retained. Descartar na ingestão abriria no
  histórico um buraco que nenhum consumidor consegue explicar depois — o
  IoTrail grava o que chegou. O registro fica com 28 bytes, sem corpo. Está
  escrito aqui para ninguém "consertar" isso depois.

Cuidado de implementação, para a 3.2: com `payloadlen == 0` a mosquitto entrega
`msg->payload` nulo, então o `push` não pode chamar `memcpy` cego.

`payload_len` é `uint32` e não `uint16` de propósito: o teto de 64 KB de um
`uint16` viraria uma limitação **do formato**, impossível de ajustar sem bump de
versão. Como `uint32`, o teto real é a constante acima.

### Do writer — política, configurável

Nenhum leitor precisa destas para interpretar um arquivo; elas governam o que o
writer aceita e produz.

| chave (`[general]`) | default | faixa | quando entra |
|---|---|---|---|
| `max_payload_len` | 64 KiB | 1 KiB .. `payload_hard_max` | Fase 3, com o `push` |
| `segment_max_bytes` | 8 MiB | a definir na Fase 4 | Fase 4, com o rollover |

**[3.1] Fechado — `max_payload_len`: regra de admissão.** Vale para a mensagem
que chega, nunca para o que já está no disco. Acima do limite, a mensagem é
**descartada**: `warn` no log com tópico, tamanho recebido e teto — sem os três,
o operador não sabe qual sensor ajustar — mais um contador por stream, porque um
sensor mandando 256 KiB a cada 5 s transforma o log em ruído e o contador é o que
sobra para explicar sumiço de dados depois (e é um `uint64`, que a Fase 9
aproveita nas métricas).

A checagem é no `on_message`, **antes** do fan-out: a mensagem é a mesma para as
N streams, então rejeita uma vez e loga uma vez.

**O default é 64 KiB, e não 1 KiB, de propósito.** O limite não é de sensor, é
de rede MQTT — e nem toda mensagem na rede vem de um sensor: `bridge/devices` do
zigbee2mqtt carrega o inventário inteiro (dezenas de KB), o discovery do Home
Assistant repete o bloco `device` em cada entidade, e um ESP32-CAM publicando
JPEG manda 20–100 KB. A assimetria decide: limite alto não custa nada em regime,
porque o registro tem o tamanho do payload real e nada é pré-alocado no caminho
quente; limite baixo custa histórico que não volta, e a perda só aparece semanas
depois, num `warn` que já rolou para fora do log.

**Coerência com o segmento, validada no boot:**

```
28 + max_payload_len <= segment_max_bytes
```

Um registro maior que o segmento inteiro é incoerente — o segmento estouraria o
próprio limite para caber num registro só. A regra mora aqui, na config, e não
no formato: amarrar `payload_hard_max` a `segment_max_bytes` traria de volta o
problema que a separação acima resolve (baixar uma chave apagaria dado já
gravado). De brinde, ela é o que sustenta a invariante do rollover na Fase 4 —
se o writer rolar antes de escrever um registro que não caberia, nenhum segmento
ultrapassa `segment_max_bytes`, e isso só vale porque o registro grande demais
foi proibido no boot.

Gravar truncado foi descartado — registro válido com conteúdo mutilado é pior
que registro nenhum, e nada no formato diria que houve truncagem. Derrubar a
stream ou o processo também: uma mensagem gorda de um sensor mal configurado não
pode parar a gravação das outras.

**[3.1] Fechado — `segment_max_bytes`:** é gatilho de rollover, e o arquivo é
autodescritivo sem ele; **default 8 MiB**, não os 64 MiB da versão anterior
(§7). A chave entra na Fase 4, junto do rollover que a usa — declarar antes
seria opção que não faz nada. Valor por stream foi considerado e adiado: só
ganha sentido quando existir stream com perfil muito diferente das outras, e o
`[general]` cobre o caso comum.

Falta padronizar como as duas aceitam tamanho — bytes puros (`8388608`) ou
sufixo (`8MiB`). Decidido junto, na tarefa que implementar a primeira delas.

## 7. Rollover

Assunto da Fase 4; aqui fica só o que o formato exige.

**Por tamanho.** Quando o segmento atual atinge `segment_max_bytes`, o writer:

1. faz um sync final e fecha o arquivo;
2. cria o próximo (`<stream>-NNNNN.log`, número +1) e grava o header de 14
   bytes com `base_offset` = o próximo offset a ser atribuído, sincronizando o
   header antes de gravar registro;
3. continua gravando.

**[3.1] Correção:** a versão anterior dizia `segment-NNNNN.log` aqui e
`<stream>-NNNNN.log` no §2. Vale o §2.

**[3.1] Por que 64 MiB é grande demais.** O problema não é o espaço, é que o
**segmento ativo nunca é apagável** — a retenção da Fase 8 só libera segmento
fechado. Um sensor a cada 30 s com registro de ~60 B produz ~2 KB/dia; a 64 MiB
esse segmento leva décadas para fechar, e a stream fica com um arquivo aberto
para sempre que a retenção não pode tocar. Com 8 MiB o problema é menor, não
resolvido: a solução de verdade é rollover por idade, que fica para a Fase 4.

**[3.1] O que cabe em 8 MiB.** Registro é `28 + payload_len`, então a conta é
quase toda payload:

| payload típico | registro | registros | 1 sensor a cada 30 s | 50 sensores a 1 Hz |
|---|---|---|---|---|
| `23.5` | 32 B | ~262.000 | ~91 dias | ~87 min |
| `{"t":23.5,"h":61.2}` | 47 B | ~178.000 | ~62 dias | ~59 min |
| JSON de 120 B | 148 B | ~57.000 | ~20 dias | ~19 min |
| 1 KB | 1052 B | ~8.000 | ~2,8 dias | ~3 min |

O tópico não entra na conta desde a 3.1 — ele mora no `.meta` (§5), gravado uma
vez por tópico distinto.

Os dois extremos são o motivo de o valor sozinho não resolver: na stream lenta o
segmento demora dois meses para fechar (com 64 MiB seriam mais de um ano, e até
lá a retenção não tem o que apagar), e na stream cheia saem ~24 arquivos por dia,
~200 MB/dia. É o argumento do rollover por idade, na Fase 4.

**Rollover por tempo continua fora da v1** e foi o que permitiu cortar
`created_ms` do header. Mesmo que ele volte na Fase 4, o campo continua
desnecessário: o primeiro registro do segmento carrega `timestamp_ms`, então
ler 40 bytes dá a idade do segmento.

## 8. Recuperação no boot

O writer abre o último segmento em modo append, não truncando. Antes de voltar
a gravar, precisa descobrir onde termina a parte íntegra do arquivo — uma queda
de energia pode ter deixado um registro parcial no fim (*torn write*).

Só o **último** segmento precisa desta varredura. Os anteriores foram fechados
com sync final durante o rollover; deles basta ler os 14 bytes de header para
montar o catálogo.

**[3.1] Reescrito.** A versão anterior inicializava `next_offset = base_offset`
e reprovava o primeiro registro em `offset != next_offset`. Um `base_offset`
corrompido — 8 bytes sem CRC nenhum — fazia a varredura parar em `pos = 14`,
`pos < file_size` dava verdadeiro, e o arquivo **inteiro** era truncado para o
header. Perda total e silenciosa de um segmento por causa de oito bytes. As duas
mudanças abaixo fecham isso sem gastar byte em disco.

```
abrir o segmento de maior numero
ler os 14 bytes de header
  magic != "IOTR"          -> erro fatal, nao e arquivo nosso
  format_version != 1      -> erro fatal, formato desconhecido

file_size    = tamanho do arquivo
pos          = 14
next_offset  = indefinido        (o primeiro registro valido define)

repetir:
    se pos + 28 > file_size                          -> falha de TAMANHO, parar
    ler 28 bytes em pos
    parse crc32, offset, timestamp_ms, topic_id, payload_len
      pos + 28 + payload_len > file_size             -> falha de TAMANHO, parar
      payload_len > payload_hard_max                 -> falha de CONTEUDO, parar
      next_offset definido e offset != next_offset   -> falha de CONTEUDO, parar
    ler payload_len bytes
    crc = crc32(bytes de pos+4 ate o fim do registro)
      crc != crc32 do registro                       -> falha de CONTEUDO, parar
    se next_offset indefinido:                       # primeiro registro valido
        se offset != base_offset:
            warn; reescrever o header com base_offset = offset e sincronizar
    pos         += 28 + payload_len
    next_offset  = offset + 1

# pos == 14 significa que nenhum registro validou
se pos == 14 e falha de CONTEUDO:
    nao truncar: renomear para <arquivo>.corrupt, error no log,
    PARAR ESTA STREAM (as outras seguem)
senao se pos < file_size:
    truncar o arquivo em pos

retomar gravacao em pos, proximo offset = next_offset
    (ou base_offset, se o segmento estava vazio)
```

**A checagem de tamanho antes de ler** (`pos + 28 + ... > file_size`) troca uma
alocação de até `payload_hard_max` por uma conta. A versão anterior chegava no
mesmo lugar por "leitura curta", depois de já ter alocado — e ela vem antes das
checagens de conteúdo, porque registro cortado no fim é falha de tamanho.

**A checagem `offset != next_offset`** é redundante com o CRC (um registro que
passa no CRC tem o offset certo), mas é barata e pega uma classe de erro que o
CRC não pega: um segmento de outra stream ou de outra época parar no diretório
errado. **[3.1]** Ela só entra a partir do segundo registro — o primeiro é quem
define a verdade, não quem é julgado por ela.

**[3.1] Fechado — o header é reescrito, não só logado.** Quando o primeiro
registro válido discorda do `base_offset`, o writer grava o valor do registro no
header e sincroniza. Só logar deixaria o catálogo em memória — o que a busca da
Fase 6 usa — com o valor errado, repetiria o `warn` em todo boot e mandaria a
busca por offset para o arquivo errado, que é exatamente o que o `base_offset`
existe para evitar. O valor novo tem prova de CRC atrás dele; o que estava lá
não tinha nenhuma.

**O que esse cruzamento não pega:** um segmento inteiro de outra stream ou de
outra época largado na pasta. Ali o header e os registros concordam entre si e
nada localmente parece errado. Quem pega é o catálogo do boot, comparando
segmentos consecutivos — se o `-00003` termina no offset 4200 e o `-00004`
começa em 900, há buraco ou sobreposição. **`warn` e não agir:** o programa não
tem como saber qual dos dois está certo, e agir seria adivinhar em cima de dado
alheio.

**[3.1] Fechado — nunca truncar para 14, e o critério é a natureza da falha.**
A varredura para por dois motivos diferentes, e eles merecem tratamento
diferente:

- **Falha de tamanho** — a parte fixa não cabe, ou `pos + 26 + lens` passa do
  fim do arquivo. É registro cortado no fim, assinatura de *torn write*.
  Truncar é seguro: depois de um registro incompleto não existe dado válido.
- **Falha de conteúdo** — o corpo está completo, mas o CRC não bate ou os
  campos são absurdos. O arquivo tem bytes que dizem ser um registro inteiro e
  não são; truncar aqui é aposta, preservar é a resposta.

Um limiar por bytes ("sobrou mais que um registro mínimo") foi considerado e
descartado: um registro de 1 KB escrito pela metade deixa ~500 bytes de sobra —
caso benigno que o limiar mandaria para `.corrupt`. A natureza da falha a
varredura já conhece; custa um enum em vez de uma subtração.

**Depois de preservar, a stream para — não abre segmento novo.** Esta é a
consequência que o desenho inicial não tinha visto: os registros dentro do
`.corrupt` têm offsets (500..900, digamos), e retomar pelo penúltimo segmento
faria os registros novos receberem 500..900 outra vez, com conteúdo diferente. O
§4 promete que offset nunca é reusado e a Fase 7 chaveia cursor de consumidor
por ele — um consumidor parado em 500 voltaria e leria outro registro, sem sinal
nenhum de que algo mudou.

Então: `error` no boot nomeando o arquivo `.corrupt` e o que fazer, e **aquela
stream não grava nesta execução** — as mensagens dela entram no contador de
descarte do §6. As outras streams seguem normalmente. Derrubar o processo
inteiro foi descartado: num edge desatendido, perder as outras quatro streams
por causa de uma é o pior resultado possível.

**Truncar** é operação de plataforma (`_chsize_s` no Windows, `ftruncate` em
POSIX) e mora no módulo da 3.3, junto do `fsync` — não espalhado em `#ifdef`.

## 9. Deixado de fora

Registrado para não ser rediscutido do zero. Nenhum destes campos existe na v1,
e todos foram cortados conscientemente:

| campo | onde estaria | por que não |
|---|---|---|
| `length` | registro | redundante — é `28 + payload_len`; a proteção contra torn write vem dos limites do §6 mais o CRC |
| `flags` | registro | só serviria para compressão/tombstone, que não é assunto de nenhuma fase planejada |
| `header_len` | header do segmento | provisionamento para um header v2 que não existe — o header do `.meta` **tem** o campo, porque lá ele é o mecanismo de extensão (§5) |
| `reserved` | header | idem |
| `header_crc32` | header | ver abaixo |
| `created_ms` | header | era o gatilho de rollover por tempo, descartado (§7), e o primeiro registro já dá a idade |
| nome da stream | header | string de tamanho variável em header fixo; o caminho do arquivo já identifica |

**[3.1] `header_crc32` reconferido e mantido fora.** O header é gravado e
sincronizado uma vez, na criação, então não tem a exposição a torn write que o
rabo do segmento tem — mas essa não é a razão forte, porque o `base_offset`
podia corromper por outros caminhos e o estrago era total (§8). A razão é que o
CRC do header **detectaria** sem **resolver**: descoberta a corrupção, ainda
seria preciso saber qual é o `base_offset` certo, e a resposta é a mesma —
perguntar ao primeiro registro. A regra do §8 faz as duas coisas e custa zero
byte; o CRC faria metade e custaria quatro.

Uma propriedade conhecida e aceita, consequência de ter cortado o `length`:
`payload_len` **está** dentro da faixa coberta pelo CRC, mas precisa ser lido e
usado **antes** que o CRC possa ser verificado — é ele que diz quantos bytes
ler — ou seja, existe uma janela em que o programa confia num tamanho ainda não
validado. **[3.1] Ela encolheu quando o tópico saiu do registro:** antes eram
dois campos nessa condição (`topic_len` e `payload_len`), agora é um; na entrada
do `.meta` o campo equivalente é o `t_len`. É para fechar essa janela que os
limites do §6 existem, e eles bastam: o pior caso é ler ~1 MiB de lixo e
descartar, sem risco de estouro de memória e sem laço infinito, porque a
varredura para no CRC. O
`crc32` em si, naturalmente, não se protege — se ele corromper, o registro é
descartado como se o conteúdo estivesse corrompido, que é a falha segura
desejada.

## 10. Leitor de referência (Python)

Dois arquivos: o `.meta` dá a tabela de tópicos, o `.log` dá os registros.

```python
import struct, zlib

META_HDR = struct.Struct("<4sHHI")   # magic, format_version, header_len, next_topic_id
TOPICO   = struct.Struct("<IIH")     # crc32, topic_id, t_len
SEG_HDR  = struct.Struct("<4sHQ")    # magic, format_version, base_offset
RECORD   = struct.Struct("<IQQII")   # crc32, offset, timestamp_ms, topic_id, payload_len

max_topic_len    = 1024
payload_hard_max = 1048576    # teto do formato, nao o limite de ingestao

def read_meta(path):
    """Devolve {topic_id: topico}. Tabela esparsa: ids removidos deixam buracos."""
    topicos = {}
    with open(path, "rb") as f:
        magic, version, header_len, _next_id = META_HDR.unpack(f.read(META_HDR.size))
        assert magic == b"IOTM", f"nao e um .meta do IoTrail: {magic!r}"
        assert version == 1, f"format_version desconhecida: {version}"
        f.seek(header_len)                             # pula campos de versao futura

        while True:
            fixo = f.read(TOPICO.size)
            if len(fixo) < TOPICO.size:
                break                                  # fim integro
            crc, topic_id, t_len = TOPICO.unpack(fixo)

            if not 0 < t_len <= max_topic_len:
                break                                  # rabo corrompido
            topico = f.read(t_len)
            if len(topico) < t_len:
                break
            if zlib.crc32(fixo[4:] + topico) != crc:
                break

            topicos[topic_id] = topico.decode("utf-8", "replace")
    return topicos

def read_segment(path, topicos):
    with open(path, "rb") as f:
        magic, version, base_offset = SEG_HDR.unpack(f.read(SEG_HDR.size))
        assert magic == b"IOTR", f"nao e um segmento IoTrail: {magic!r}"
        assert version == 1, f"format_version desconhecida: {version}"

        next_offset = None
        while True:
            fixo = f.read(RECORD.size)
            if len(fixo) < RECORD.size:
                break                                  # fim integro
            crc, offset, ts_ms, topic_id, payload_len = RECORD.unpack(fixo)

            if payload_len > payload_hard_max:
                break                                  # rabo corrompido
            if next_offset is not None and offset != next_offset:
                break

            payload = f.read(payload_len)
            if len(payload) < payload_len:
                break                                  # rabo corrompido
            if zlib.crc32(fixo[4:] + payload) != crc:
                break

            next_offset = offset + 1
            yield {
                "offset": offset,
                "timestamp_ms": ts_ms,
                # id desconhecido nao invalida o registro (§4)
                "topic": topicos.get(topic_id, f"<topic_id {topic_id} desconhecido>"),
                "payload": payload,
            }
```

Nos dois arquivos o CRC é calculado sobre `fixo[4:] + resto` — a parte fixa
depois do campo `crc32`, seguida do que vem variável. É a faixa contígua descrita
no §4 e no §5.

O leitor **não** valida `base_offset` contra o primeiro registro, não corrige
`next_topic_id` e não trunca nada: ler é sempre não destrutivo. Quem conserta é o
writer no boot (§8), porque só ele tem os arquivos abertos para escrita.

## 11. Decisões da 3.1

Os seis pontos que a revisão abriu, todos fechados em 2026-09-06. O raciocínio
de cada um está na seção que ele mudou; aqui fica o índice.

1. ~~`segment_max_bytes` fora da spec~~ — **fechado (2026-09-06):** vai para o
   `[general]` do `iotrail.conf`, default 8 MiB, chave implementada na Fase 4
   junto do rollover (§6, §7). Rollover por idade fica marcado para a Fase 4.
2. ~~§8 com o primeiro registro como autoridade sobre `base_offset`~~ —
   **fechado (2026-09-06):** o primeiro registro válido conduz a varredura, o
   header é reescrito e sincronizado quando diverge, sem `header_crc32`; buraco
   ou sobreposição entre segmentos consecutivos é `warn` no catálogo, sem ação
   (§3, §8, §9).
3. ~~Proibição de truncar para 14~~ — **fechado (2026-09-06):** trunca só em
   falha de tamanho; falha de conteúdo preserva o arquivo como `.corrupt` e
   **para aquela stream**, para não reusar offset (§8).
4. ~~`topic_len == 0` inválido, `payload_len == 0` válido~~ — **fechado
   (2026-09-06):** zero em `t_len` (hoje no `.meta`, ver ponto 7) é corrupção;
   payload vazio é gravado como registro sem corpo (§6, §8).
5. ~~Mensagem acima do limite~~ — **fechado (2026-09-06):** `max_payload_len`
   vai para o `[general]` (default 64 KiB) como regra de admissão, separado do
   `payload_hard_max` de 1 MiB que a varredura usa; acima do limite a mensagem
   é descartada com `warn` e contador por stream (§6).
6. ~~Fan-out documentado no `iotrail.conf`~~ — **fechado (2026-09-06):** aviso
   no comentário de `topics`, seção `[stream:nome]` (§2).
7. **Tópico sai do registro** — aberto e **fechado (2026-09-06)**, fora dos seis
   pontos originais. O tópico era 37% do registro na medição de 2026-08-29 e se
   repetia em toda mensagem; agora mora no `.meta` da stream e o registro guarda
   `topic_id`. Registro passa de `26 + topic + payload` para `28 + payload`, e o
   `.meta` ganha header com `header_len` e `next_topic_id` (§5). A ideia vinha de
   `knowledge_base/claude_memory/TODO.md:383-437`, onde estava prevista como
   candidata ao `format_version` 2 — entrou na v1 porque nada foi gravado ainda.

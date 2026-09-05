# Formato de segmento — IoTrail

`format_version = 1` · definido em 2026-08-29 · **provisório**

Especificação do formato em disco dos segmentos do IoTrail. É a fonte de
verdade para o writer em C++ (`src/segment_writer.cpp`) e para o leitor em
Python — as duas implementações têm que bater byte a byte com este documento.

**Status provisório.** Esta é a versão madura o suficiente para sustentar
rollover (Fase 4) e índice (Fase 5) sem retrabalho, mas deliberadamente enxuta:
campos sem uso no curto prazo foram cortados (ver
[Deixado de fora](#deixado-de-fora)). A Fase 3 revisita o formato depois que
ele rodar com volume real, promovendo `format_version` a 2 se algo mudar.

---

## 1. Convenções

- **Endianness: little-endian**, explicitamente, para todos os campos
  inteiros. Em x86 e ARM isso é no-op — a declaração existe para que o arquivo
  seja portável, não porque alguma plataforma alvo precise de conversão hoje.
- **Sem padding.** Todas as structs usam `#pragma pack(push, 1)` e são
  validadas com `static_assert` no tamanho esperado, como já é feito hoje em
  `src/segment_writer.cpp:26-35`.
- **Sem alinhamento.** Registros são gravados colados, um após o outro.
- Todos os offsets de byte neste documento são relativos ao início da
  estrutura descrita.

## 2. Layout de diretório

Uma pasta por stream, e o nome da stream **repetido** no nome do arquivo. O
nome vem da config (`[stream:nome]` no `iotrail.conf`) e não é gravado dentro
de nenhum arquivo — o caminho é quem identifica.

```
data/
  vibracao/
    vibracao-00000.log
    vibracao-00001.log
  telemetria/
    telemetria-00000.log
```

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

**Layout plano foi descartado** (`data/vibracao-00000.log`, sem pasta). Motivo
principal: a retenção da Fase 8 apagaria segmentos da stream errada quando um
nome for prefixo de outro — `vibracao-*.log` casa tanto com `vibracao-00000.log`
quanto com `vibracao-extra-00000.log`. Perda de dados silenciosa causada por
dois nomes que o operador escreve sem pensar (`temperatura` e
`temperatura-externa`). Com pasta, é estruturalmente impossível. Secundários:
os `.idx` da Fase 5 dobrariam a contagem de arquivos num diretório único, e
extrair o nome da stream de volta a partir do arquivo exigiria uma regra de
parsing (`rsplit` no último `-`) que a pasta dispensa.

### Nome de stream: caracteres aceitos

O nome vira nome de pasta e de arquivo, então precisa ser validado no
`loadConfigs()` (`src/config.cpp`), no boot — não deixar o `fopen` falhar
depois com mensagem obscura.

Duas checagens, ambas necessárias:

1. **Caracteres: só `[A-Za-z0-9_-]+`.** No Windows `/ \ : * ? " < > |` são
   ilegais em nome de arquivo; espaço e ponto final também causam problema
   (são removidos silenciosamente do fim de um nome). A lista branca evita
   enumerar casos.
2. **Nomes reservados do DOS, rejeitados à parte:** `CON`, `PRN`, `AUX`,
   `NUL`, `COM1`–`COM9`, `LPT1`–`LPT9`. Eles **passam** na regra de
   caracteres acima, mas falham no `fopen` mesmo com extensão — `NUL.log` não
   é criável no Windows. Comparação **case-insensitive** (`nul` falha igual).

Nome inválido em qualquer uma das duas: erro claro no boot, nomeando a seção
`[stream:...]` culpada.

## 3. Header de segmento (14 bytes)

Gravado uma única vez, no início de cada arquivo, no momento da criação.

| offset | tam | campo | tipo | valor |
|---|---|---|---|---|
| 0 | 4 | `magic` | `char[4]` | `"IOTR"` (`0x49 0x4F 0x54 0x52`) |
| 4 | 2 | `format_version` | `uint16` | `1` |
| 6 | 8 | `base_offset` | `uint64` | offset do primeiro registro deste segmento |

O primeiro registro começa no byte 14.

**`base_offset`** é o que permite localizar um offset sem varrer arquivos.
Como o rollover é por tamanho e os registros têm comprimento variável, a
quantidade de registros por segmento varia — não existe divisão que diga em
qual arquivo mora um offset. No boot o programa monta um catálogo em memória
lendo estes 14 bytes de cada segmento, e a busca da Fase 6 roda sobre ele.

## 4. Registro (26 bytes fixos + variável)

| offset | tam | campo | tipo | descrição |
|---|---|---|---|---|
| 0 | 4 | `crc32` | `uint32` | CRC de tudo a partir do byte 4 |
| 4 | 8 | `offset` | `uint64` | posição deste registro na stream |
| 12 | 8 | `timestamp_ms` | `uint64` | epoch em ms, atribuído na chegada |
| 20 | 2 | `topic_len` | `uint16` | bytes do tópico |
| 22 | 4 | `payload_len` | `uint32` | bytes do payload |
| 26 | N | `topic` | `char[N]` | tópico MQTT, sem terminador |
| 26+N | M | `payload` | `byte[M]` | payload MQTT, opaco |

Tamanho total do registro: `26 + topic_len + payload_len`.

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
final. Implementado com tabela de 256 entradas em `src/crc32.h` — sem
dependência externa. No lado Python, `zlib.crc32()` produz exatamente este
valor.

### `offset`

Monotônico, incrementa de 1 em 1, nunca é reusado. **É por stream** — cada
stream tem seu próprio contador começando em 0. Não existe offset global entre
streams; "offset 500" só faz sentido qualificado por stream.

### `timestamp_ms`

Capturado no momento em que a mensagem chega do broker (em
`SegmentWriter::push`, `src/segment_writer.cpp:76-79`), **não** no momento em
que ela é gravada. Registros drenados num mesmo lote preservam o espaçamento
temporal real de chegada.

### `topic` e `payload`

Sem terminador nulo, sem encoding declarado. O `payload` é tratado como opaco
— o IoTrail não interpreta conteúdo.

## 5. Limites

Constantes de sanidade, usadas para validar campos antes de confiar neles na
varredura de recuperação:

| constante | valor | motivo |
|---|---|---|
| `kMaxTopicLen` | 1024 | tópicos MQTT reais ficam muito abaixo disso |
| `kMaxPayloadLen` | 1 MiB (1048576) | payload de sensor IoT é ordens de grandeza menor |
| `kSegmentMaxBytes` | 64 MiB | gatilho de rollover (ver §6) |

`payload_len` é `uint32` e não `uint16` de propósito: o teto de 64 KB de um
`uint16` viraria uma limitação **do formato**, impossível de ajustar sem bump
de versão. Como `uint32`, o teto real é a constante acima — mudável a qualquer
momento.

## 6. Rollover

**Por tamanho apenas.** Quando o segmento atual atinge `kSegmentMaxBytes`, o
writer:

1. faz um sync final (`fflush` + `_commit`/`fsync`) e fecha o arquivo;
2. cria o próximo (`segment-NNNNN.log`, número +1) e grava o header de 14
   bytes com `base_offset` = o próximo offset a ser atribuído;
3. continua gravando.

Rollover **por tempo foi descartado** — só importaria em stream de baixo
tráfego onde o segmento nunca enche, e é o que permitiu cortar o campo
`created_ms` do header.

## 7. Recuperação no boot

O writer abre o último segmento em modo append, não truncando. Antes de voltar
a gravar, ele precisa descobrir onde termina a parte íntegra do arquivo — uma
queda de energia pode ter deixado um registro parcial no fim (*torn write*).

Só o **último** segmento precisa desta varredura. Os anteriores foram fechados
com sync final durante o rollover; deles basta ler os 14 bytes de header para
montar o catálogo.

```
abrir o segmento de maior número
ler os 14 bytes de header
  magic != "IOTR"           → erro fatal, não é arquivo nosso
  format_version != 1       → erro fatal, formato desconhecido

pos          = 14
next_offset  = base_offset

repetir:
    ler 26 bytes em pos
      leitura curta / EOF                       → fim íntegro, parar
    parse crc32, offset, timestamp_ms, topic_len, payload_len
      topic_len   > kMaxTopicLen                → rabo corrompido, parar
      payload_len > kMaxPayloadLen              → rabo corrompido, parar
      offset      != next_offset                → rabo corrompido, parar
    ler topic_len + payload_len bytes
      leitura curta                             → rabo corrompido, parar
    crc = crc32(bytes de pos+4 até o fim do registro)
      crc != crc32 do registro                  → rabo corrompido, parar
    pos         += 26 + topic_len + payload_len
    next_offset += 1

se pos < tamanho do arquivo:
    truncar o arquivo em pos
retomar gravação a partir de pos, próximo offset = next_offset
```

A checagem `offset != next_offset` é redundante com o CRC (um registro que
passa no CRC tem o offset certo), mas é barata e pega uma classe de erro que o
CRC não pega: um segmento de outra stream ou de outra época parar no diretório
errado.

**Truncar** usa o mesmo padrão de `#ifdef` já presente em
`src/segment_writer.cpp:49-53`: `_chsize_s(_fileno(f), pos)` no Windows,
`ftruncate(fileno(f), pos)` em POSIX.

## 8. Deixado de fora

Registrado para não ser rediscutido do zero. Nenhum destes campos existe na
v1, e todos foram cortados conscientemente por não terem uso no curto prazo:

| campo | onde estaria | por que não |
|---|---|---|
| `length` | registro | redundante — sai de `topic_len` + `payload_len`; a proteção contra torn write vem dos limites do §5 mais o CRC |
| `flags` | registro | só serviria para compressão/tombstone, que é assunto da Fase 3 |
| `header_len` | header | provisionamento para um header v2 que não existe |
| `reserved` | header | idem |
| `header_crc32` | header | o header é escrito uma vez na criação e sincronizado; não tem a exposição a torn write que o rabo do segmento tem |
| `created_ms` | header | era o gatilho de rollover por tempo, que foi descartado (§6) |
| nome da stream | header | string de tamanho variável em header fixo; o caminho do arquivo já identifica |

Uma propriedade conhecida e aceita, consequência de ter cortado o `length`:
`topic_len` e `payload_len` **estão** dentro da faixa coberta pelo CRC, mas
precisam ser lidos e usados **antes** que o CRC possa ser verificado — são eles
que dizem quantos bytes ler. Ou seja, existe uma janela em que o programa
confia num tamanho ainda não validado. É exatamente para fechar essa janela que
os limites do §5 existem: eles bastam para impedir uma leitura absurda, e o CRC
descarta o registro logo em seguida. O `crc32` em si, naturalmente, não se
protege — se ele corromper, o registro é descartado como se o conteúdo
estivesse corrompido, que é a falha segura desejada.

## 9. Leitor de referência (Python)

```python
import struct, zlib

HEADER = struct.Struct("<4sHQ")      # magic, format_version, base_offset
RECORD = struct.Struct("<IQQHI")     # crc32, offset, timestamp_ms, topic_len, payload_len

def read_segment(path):
    with open(path, "rb") as f:
        magic, version, base_offset = HEADER.unpack(f.read(HEADER.size))
        assert magic == b"IOTR", f"nao e um segmento IoTrail: {magic!r}"
        assert version == 1, f"format_version desconhecida: {version}"

        while True:
            fixed = f.read(RECORD.size)
            if len(fixed) < RECORD.size:
                break                                  # fim integro
            crc, offset, ts_ms, topic_len, payload_len = RECORD.unpack(fixed)

            body = f.read(topic_len + payload_len)
            if len(body) < topic_len + payload_len:
                break                                  # rabo corrompido

            if zlib.crc32(fixed[4:] + body) != crc:
                break                                  # rabo corrompido

            yield {
                "offset": offset,
                "timestamp_ms": ts_ms,
                "topic": body[:topic_len].decode("utf-8", "replace"),
                "payload": body[topic_len:],
            }
```

Repara que o CRC é calculado sobre `fixed[4:] + body` — os 22 bytes da parte
fixa depois do campo `crc32`, seguidos do tópico e do payload. É a faixa
contígua descrita no §4.

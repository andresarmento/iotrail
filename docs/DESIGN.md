# IoTrail — Design

Decisões consolidadas. Enxuto de propósito: cresce conforme as fases avançam.
O detalhamento de cada decisão vive no `docs/TODO.md` da tarefa que a fechou, e
o raciocínio longo em comentário junto da linha que a implementa.

---

## 1. O que é

Uma camada leve e persistente de event streaming para IoT, que fica **depois**
da entrega em tempo real do MQTT. O IoTrail é **cliente** de brokers já
existentes — não é broker, não substitui o mosquitto. Ele subscreve tópicos e
grava as mensagens num log append-only próprio, dando histórico, replay,
retenção e consumo independente por várias aplicações.

**Alvo:** redes IoT pequenas e médias (dezenas de sensores, poucas msgs/s) e
edge computing. Baixo consumo de RAM, CPU e disco.

**O que não é:** não é Kafka. As ideias vêm de lá (offset por consumidor,
replay, retenção), o throughput não. Para ~50 sensores publicando 1x/s, Kafka é
marreta grande demais — essa é a razão de o projeto existir
(`knowledge_base/docs/motivacao.txt`).

**Sem dependência de banco de dados.** Segmentos e índices são formato próprio,
em arquivo.

---

## 2. Arquitetura de ingestão

```
Broker MQTT ──> Cliente MQTT ──> Fila em RAM ──> Writer Thread ──> disco
                                                      │
                        a cada write_interval ────────┤  monta batch, write()  ──> page cache
                        a cada sync_interval  ────────┘  fsync()               ──> disco
```

**Dois intervalos separados, e essa separação é o ponto:**

- `write_interval` controla **eficiência** — de quanto em quanto tempo a thread
  acorda, drena a fila e escreve um lote.
- `sync_interval` controla **durabilidade** — de quanto em quanto tempo o fsync
  força a ida ao disco. Define a janela máxima de perda numa queda de energia.

Amarrá-los seria trocar throughput por durabilidade num parâmetro só.
Raciocínio completo em `knowledge_base/docs/decisao_sync_write.txt`.

**Quem recebe do broker não bloqueia.** A callback MQTT só enfileira. É por isso
que o logging é assíncrono também — logar de forma síncrona no caminho de
recebimento reintroduziria exatamente a variância de latência que essa
arquitetura existe pra evitar.

---

## 3. Modelo de dados

**Uma stream = uma fila = uma writer thread = uma pasta em `data/` = seu próprio
contador de offset, começando em 0.**

- **Não existe offset global.** "Offset 500" só faz sentido qualificado por
  stream.
- **Offset é monotônico, incrementa de 1, nunca é reusado.**
- **Uma stream recebe de um único broker** (N:1, não N:M). Fan-in de vários
  brokers numa stream exigiria decidir dedup e ordenação entre origens, e o
  registro guarda o tópico, não a origem — juntar brokers perderia a
  proveniência sem como recuperar depois. Para gravar de dois brokers, declare
  uma stream para cada.
- **O timestamp é capturado na chegada, não na escrita.** Registros drenados no
  mesmo lote preservam o espaçamento temporal real.
- **O payload é opaco.** O IoTrail não interpreta conteúdo.

---

## 4. Persistência

Especificação byte a byte: `knowledge_base/docs/formato_segmento.md`
(`format_version = 1`, provisória). Ela é a fonte de verdade para o writer em
C++ e para o leitor em Python — as duas implementações têm que bater com o
documento. **Migra para `docs/` na Fase 3**, quando o writer for escrito.

Resumo do que está decidido:

- **Uma pasta por stream**, nome repetido no arquivo:
  `data/vibracao/vibracao-00000.log`. Layout plano foi descartado — a retenção
  apagaria segmentos da stream errada quando um nome fosse prefixo de outro
  (`temperatura` e `temperatura-externa`).
- **Header de 14 bytes** por segmento: `magic "IOTR"`, `format_version`,
  `base_offset`.
- **Registro:** 26 bytes fixos (`crc32`, `offset`, `timestamp_ms`, `topic_len`,
  `payload_len`) + tópico + payload.
- **CRC-32 IEEE** cobrindo do byte 4 ao fim do payload — tudo menos o próprio
  CRC. Faixa contígua, e protege o `offset`, que é a pior corrupção possível
  (índice e cursores de consumidor são chaveados por ele).
- **Little-endian explícito, sem padding, sem alinhamento.**
- **Rollover por tamanho apenas.** Por tempo foi descartado.
- **Recuperação no boot:** só o último segmento precisa de varredura — os
  anteriores foram fechados com sync no rollover. A varredura acha o fim da
  parte íntegra (torn write) e trunca ali.

---

## 5. Configuração

`iotrail.conf`, INI com **seções tipadas** — `[broker:nome]` e `[stream:nome]`.
O tipo no cabeçalho é o que permite broker e stream conviverem no mesmo arquivo
sem ambiguidade.

- **O arquivo-fonte é o da raiz do projeto**; o build copia uma versão dele pro
  lado do executável, e é de lá que o programa lê. Editar a cópia em `build/`
  não adianta.
- **Config inválida derruba o boot.** Nada de fallback para um destino que
  ninguém escreveu.
- **O arquivo é lido até o fim**, e todos os problemas vão pro log de uma vez —
  em vez de um erro por boot.
- **Nome de stream é validado no boot**, na config, não no writer: nome vira
  pasta e arquivo, e tem que falhar nomeando a seção culpada, não num `fopen`
  obscuro depois. Só `[A-Za-z0-9_-]+`, mais rejeição dos nomes reservados do DOS
  (`CON`, `NUL`, `COM1`–`COM9`, `LPT1`–`LPT9`), que passam na regra de
  caracteres mas não são criáveis no Windows.

---

## 6. Ciclo de vida do processo

Sobe na ordem `logging::init()` → `signals::init()` → o resto (config na Fase 1,
clientes na 2, writers na 3). O logging vem primeiro porque erro de qualquer um
dos outros precisa de onde sair.

**Todo pedido de parada passa por um ponto só** — `signals::request_stop()`, lido
por `signals::stop_requested()`. Entram por ali o `SIGINT`/`SIGTERM`, os eventos
de console do Windows e, quando existir, a API de gerência. O `main` espera em
polling de 200 ms.

**O prazo do encerramento não é nosso, é do SO.** No Windows, `CTRL_CLOSE`
(fechar a janela), `LOGOFF` e `SHUTDOWN` matam o processo assim que o handler
**retorna** — os "poucos segundos" são o prazo pra trabalhar lá dentro, não
depois. Por isso o handler desses três bloqueia até o `main` avisar que terminou,
em vez de voltar na hora. Prazo real medido nesta máquina: **5013 ms** (vem do
registro do Windows, muda de máquina).

Consequência para a Fase 3, quando parar deixar de ser barato: **drenar as filas
e fazer o `fsync` final roda dentro do handler**, com orçamento de ~5 s menos os
200 ms do polling, e são M filas e M `fsync` para M streams. Se não couber, a
escolha é entre limitar a drenagem (perda conhecida) e tentar até o fim
(arriscar ser morto no meio).

**Handler não loga.** Logar chamaria `malloc` e travaria o mutex do spdlog em
contexto de sinal. Quem anuncia a parada é o `main`, depois do laço.

---

## 7. Ambiente e build

- **C++17 + STL.** C++20 avaliado e descartado nesta rodada; o toolchain
  suporta, então subir depois continua possível.
- **MQTT 3.1.1 via mosquitto.** API C (`libmosquitto`) ou wrapper C++
  (`libmosquittopp`) fica para a Fase 2. Precedente forte pela API C: o wrapper
  não expõe o `mosquitto*` interno, o que fecha a porta para MQTT 5.
- **Logging: spdlog**, assíncrono (ver §2).
- **MSYS2 ucrt64**, CMake + Ninja. Multiplataforma é objetivo: o código
  específico de SO fica isolado atrás de `#ifdef` em pontos nomeados (parada
  ordenada, localização do executável, `fsync`, `truncate`), não espalhado.

---

## 8. Ainda em aberto

- Acesso dos consumidores: API HTTP + token é a direção, sem decisão fechada.
- Índice por segmento (esparso?), política de retenção, limite de fila.
- Revisão do `format_version` depois de rodar com volume real.

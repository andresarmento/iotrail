#!/usr/bin/env python3
"""Leitor de segmentos do IoTrail (format_version 1).

Implementa docs/formato_segmento.md - header de segmento de 14 bytes,
registros de 26 bytes fixos + topico + payload, com offset e crc32.

Substitui os leitores dos prototipos (src/test_0../test_2/read_test_segment.py),
que liam o layout antigo de 14 bytes por registro, sem offset nem checksum.

Uso:
    python -u src/read_segment.py build/iotrail/data/default/default-00000.log
    python -u src/read_segment.py <arquivo> --limit 20
    python -u src/read_segment.py <arquivo> --quiet     # so o resumo
"""

import argparse
import struct
import sys
import zlib
from datetime import datetime

MAGIC = b"IOTR"
FORMAT_VERSION = 1

# Little-endian explicito ("<"), sem padding - casa com o #pragma pack(1) do
# lado C++. Ver docs/formato_segmento.md secao 1.
SEGMENT_HEADER = struct.Struct("<4sHQ")   # magic, format_version, base_offset
RECORD_HEADER = struct.Struct("<IQQHI")   # crc32, offset, timestamp_ms,
                                          # topic_len, payload_len

# Limites de sanidade - secao 5 da spec. Sao usados antes do CRC poder
# validar, porque sao eles que dizem quantos bytes ler.
MAX_TOPIC_LEN = 1024
MAX_PAYLOAD_LEN = 1024 * 1024


class SegmentError(Exception):
    pass


def read_segment(path):
    """Gera (registro, None) por registro valido e termina.

    Levanta SegmentError se o header do segmento for invalido. Um rabo
    corrompido nao e' erro - e' o caso esperado depois de queda de energia:
    a geracao simplesmente para, e o motivo fica em .stop_reason.
    """
    with open(path, "rb") as f:
        raw = f.read(SEGMENT_HEADER.size)
        if len(raw) < SEGMENT_HEADER.size:
            raise SegmentError("arquivo menor que o header de 14 bytes")

        magic, version, base_offset = SEGMENT_HEADER.unpack(raw)
        if magic != MAGIC:
            raise SegmentError(f"magic inesperado: {magic!r} (esperado {MAGIC!r})")
        if version != FORMAT_VERSION:
            raise SegmentError(
                f"format_version {version}, este leitor entende {FORMAT_VERSION}"
            )

        yield {"_header": True, "base_offset": base_offset, "version": version}

        expected_offset = base_offset
        while True:
            fixed = f.read(RECORD_HEADER.size)
            if len(fixed) < RECORD_HEADER.size:
                if len(fixed) > 0:
                    yield {"_stop": "header de registro truncado no fim"}
                return

            crc, offset, ts_ms, topic_len, payload_len = RECORD_HEADER.unpack(fixed)

            if topic_len > MAX_TOPIC_LEN:
                yield {"_stop": f"topic_len {topic_len} acima do limite"}
                return
            if payload_len > MAX_PAYLOAD_LEN:
                yield {"_stop": f"payload_len {payload_len} acima do limite"}
                return
            if offset != expected_offset:
                yield {"_stop": f"offset {offset} fora de sequencia (esperado {expected_offset})"}
                return

            body = f.read(topic_len + payload_len)
            if len(body) < topic_len + payload_len:
                yield {"_stop": "registro truncado"}
                return

            # CRC cobre do byte 4 do registro ate o fim do payload - faixa
            # contigua. fixed[4:] sao os 22 bytes da parte fixa depois do
            # proprio campo crc32.
            if zlib.crc32(fixed[4:] + body) != crc:
                yield {"_stop": f"crc32 nao confere no offset {offset}"}
                return

            yield {
                "offset": offset,
                "timestamp_ms": ts_ms,
                "topic": body[:topic_len].decode("utf-8", "replace"),
                "payload": body[topic_len:],
            }
            expected_offset += 1


def main():
    ap = argparse.ArgumentParser(description="Leitor de segmentos IoTrail")
    ap.add_argument("arquivo", help="caminho do segment .log")
    ap.add_argument("--limit", type=int, default=0,
                    help="mostra no maximo N registros (0 = todos)")
    ap.add_argument("--quiet", action="store_true",
                    help="nao lista registros, so o resumo")
    args = ap.parse_args()

    total = 0
    base_offset = None
    stop_reason = None
    primeiro_ts = None
    ultimo_ts = None

    try:
        for item in read_segment(args.arquivo):
            if item.get("_header"):
                base_offset = item["base_offset"]
                print(f"segmento: format_version={item['version']} "
                      f"base_offset={base_offset}")
                continue
            if "_stop" in item:
                stop_reason = item["_stop"]
                break

            total += 1
            if primeiro_ts is None:
                primeiro_ts = item["timestamp_ms"]
            ultimo_ts = item["timestamp_ms"]

            if not args.quiet and (args.limit == 0 or total <= args.limit):
                quando = datetime.fromtimestamp(item["timestamp_ms"] / 1000)
                try:
                    payload = item["payload"].decode("utf-8")
                except UnicodeDecodeError:
                    payload = repr(item["payload"])
                print(f"  offset={item['offset']:<6} "
                      f"{quando:%H:%M:%S.%f}"[:-3] +
                      f"  topico=\"{item['topic']}\"  payload=\"{payload}\"")
    except SegmentError as e:
        print(f"ERRO: {e}", file=sys.stderr)
        return 1
    except FileNotFoundError:
        print(f"ERRO: arquivo nao encontrado: {args.arquivo}", file=sys.stderr)
        return 1

    if not args.quiet and args.limit and total > args.limit:
        print(f"  ... (+{total - args.limit} registros nao mostrados)")

    print(f"\n{total} registros validos", end="")
    if total:
        print(f", offsets {base_offset}..{base_offset + total - 1}", end="")
        span = (ultimo_ts - primeiro_ts) / 1000
        print(f", {span:.3f}s entre o primeiro e o ultimo", end="")
    print()

    if stop_reason:
        print(f"parou: {stop_reason}")
        print("(esperado se o processo foi morto no meio de uma escrita - o "
              "writer trunca esse rabo no proximo boot)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Lê e imprime os registros gravados por test_write_segment.cpp.

Layout do registro (little-endian, sem padding — espelha o RecordHeader
`#pragma pack(1)` do lado C++):
    timestamp_ms : uint64
    topic_len    : uint16
    payload_len  : uint32
    topic        : bytes[topic_len]
    payload      : bytes[payload_len]
"""

import struct
import sys
from datetime import datetime, timezone
from pathlib import Path

HEADER_FORMAT = "<QHI"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)

DEFAULT_PATH = Path(__file__).resolve().parent.parent.parent / "build" / "test_1" / "test_segment.bin"


def read_records(path: Path):
    data = path.read_bytes()
    offset = 0
    total = len(data)

    while offset < total:
        if offset + HEADER_SIZE > total:
            raise ValueError(
                f"arquivo truncado: sobraram {total - offset} bytes, "
                f"menos que o header ({HEADER_SIZE})"
            )

        timestamp_ms, topic_len, payload_len = struct.unpack_from(HEADER_FORMAT, data, offset)
        offset += HEADER_SIZE

        topic = data[offset:offset + topic_len]
        offset += topic_len

        payload = data[offset:offset + payload_len]
        offset += payload_len

        yield timestamp_ms, topic.decode("utf-8"), payload.decode("utf-8")


def format_timestamp(timestamp_ms: int) -> str:
    dt = datetime.fromtimestamp(timestamp_ms / 1000, tz=timezone.utc).astimezone()
    return dt.strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]


def main():
    path = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_PATH

    if not path.exists():
        print(f"Arquivo não encontrado: {path}", file=sys.stderr)
        sys.exit(1)

    count = 0
    for timestamp_ms, topic, payload in read_records(path):
        print(f"[{count}] {format_timestamp(timestamp_ms)}  {topic:<20} payload={payload!r}")
        count += 1

    print(f"\n{count} registros lidos de {path}")


if __name__ == "__main__":
    main()

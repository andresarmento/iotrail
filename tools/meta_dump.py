#!/usr/bin/env python3
"""
IoTrail - dump do <stream>.meta: header + tabela de topicos.

Formato em docs/FORMATO.md secao 5; a varredura aqui espelha a do writer
(src/storage/meta.cpp:149-271), inclusive nos motivos de parada.

LEITURA PURA: nao trunca, nao conserta, nao reescreve nada. Quem conserta e' o
writer no boot, que e' quem tem o arquivo aberto pra escrita - rodar isto com o
IoTrail no ar e' seguro, so pode pegar um instante entre a escrita da entrada e
o sync.

Uso:  python tools/meta_dump.py <arquivo.meta | pasta> [...]
      pasta e' varrida recursivamente atras de *.meta, entao apontar pro
      data_dir mostra todas as streams de uma vez.
"""
import struct
import sys
import zlib
from pathlib import Path

# Espelham src/storage/format.h e o header de src/storage/meta.cpp:22-33.
meta_magic = b"IOTM"
format_version = 1
meta_header_len = 12
max_topic_len = 1024

meta_header = struct.Struct("<4sHHI")  # magic, format_version, header_len, next_topic_id
entry_head = struct.Struct("<IIH")     # crc32, topic_id, t_len


def scan(data):
    """Le a tabela do byte meta_header_len ate onde ela continuar integra.

    Devolve (entradas, fim, parada, avisos): 'fim' e' onde a ultima entrada
    integra terminou - o ponto em que o writer truncaria - e 'parada' e' o
    motivo, ou None se o arquivo acabou redondo.
    """
    entries = []
    warnings = []
    seen_ids = set()
    seen_topics = set()
    pos = meta_header_len
    stop = None

    while True:
        if pos + entry_head.size > len(data):
            # Sobrou menos que a parte fixa: rabo de uma escrita interrompida.
            if pos < len(data):
                stop = "sobraram %d bytes, menos que a parte fixa da entrada" % (len(data) - pos)
            break

        crc, topic_id, t_len = entry_head.unpack_from(data, pos)

        # t_len == 0 e' corrupcao, nao entrada vazia: MQTT 3.1.1 exige topico
        # com pelo menos um caractere, entao zero no disco nao veio de broker.
        if not 0 < t_len <= max_topic_len:
            stop = "t_len %d fora da faixa 1..%d" % (t_len, max_topic_len)
            break

        end = pos + entry_head.size + t_len
        if end > len(data):
            stop = "topico de %d bytes nao cabe no que resta" % t_len
            break

        # O CRC cobre do byte 4 da entrada ate o fim do topico - a mesma faixa
        # contigua do secao 5.
        if zlib.crc32(data[pos + 4:end]) != crc:
            stop = "CRC nao confere"
            break

        topic = data[pos + entry_head.size:end].decode("utf-8", "replace")

        # Duplicata nao deveria existir num arquivo append-only, mas a
        # ferramenta de limpeza reescreve o arquivo inteiro. Vence a primeira, e
        # a varredura segue - nao afeta a leitura dos dados.
        flag = ""
        if topic_id in seen_ids:
            flag = "id repetido"
            warnings.append("topic_id %d repetido em %d - vale a primeira entrada"
                            % (topic_id, pos))
        elif topic in seen_topics:
            flag = "topico repetido"
            warnings.append('topico "%s" repetido em %d - vale a primeira entrada'
                            % (topic, pos))
        seen_ids.add(topic_id)
        seen_topics.add(topic)

        entries.append((pos, topic_id, t_len, topic, flag))
        pos = end

    return entries, pos, stop, warnings


def dump(path):
    """Imprime um .meta. False = arquivo com problema."""
    data = path.read_bytes()
    print("%s  (%d bytes)" % (path, len(data)))

    if len(data) < meta_header_len:
        print("  ERRO: header incompleto - %d bytes, esperados %d" % (len(data), meta_header_len))
        return False

    magic, version, header_len, next_id = meta_header.unpack_from(data)
    if magic != meta_magic:
        print("  ERRO: magic %r - o arquivo nao e' um .meta do IoTrail" % magic)
        return False
    if version != format_version:
        print("  ERRO: format_version %d desconhecida (este leitor le %d)"
              % (version, format_version))
        return False
    # Na v1 o header tem tamanho fixo. Numa v2 e' aqui que a tabela passaria a
    # comecar mais adiante, e o leitor antigo pularia o excedente.
    if header_len != meta_header_len:
        print("  ERRO: header_len %d invalido na v1 (esperado %d)" % (header_len, meta_header_len))
        return False

    entries, end, stop, warnings = scan(data)

    # max(header, maior id + 1): o header segura o valor quando uma limpeza
    # removeu os ids do topo; as entradas puxam pra cima quando o header ficou
    # atrasado por uma queda entre gravar a entrada e atualizar o header.
    effective = max([next_id] + [e[1] + 1 for e in entries])

    print("  magic           %s" % magic.decode())
    print("  format_version  %d" % version)
    print("  header_len      %d" % header_len)
    print("  next_topic_id   %d   (header)" % next_id)
    print("  efetivo         %d   max(header, maior id + 1)%s"
          % (effective, "" if effective == next_id else "   <- header atrasado"))
    print("  topicos         %d   em %d bytes de tabela" % (len(entries), end - meta_header_len))

    if entries:
        print()
        print("    offset      id  t_len  topico")
        for pos, topic_id, t_len, topic, flag in entries:
            line = "    %6d  %6d  %5d  %s" % (pos, topic_id, t_len, topic)
            print(line + ("   <- " + flag if flag else ""))

    for w in warnings:
        print("  AVISO: %s" % w)

    if stop:
        print("  AVISO: %d bytes ilegiveis no fim (%s)" % (len(data) - end, stop))
        print("         o writer trunca em %d no proximo boot" % end)
        return False

    return True


def targets(args):
    for arg in args:
        p = Path(arg)
        if p.is_dir():
            yield from sorted(p.rglob("*.meta"))
        else:
            yield p


def main(argv):
    if len(argv) < 2:
        print(__doc__.strip())
        return 2

    # Topico MQTT e' UTF-8; o console do Windows costuma nao ser.
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except AttributeError:
        pass

    paths = list(targets(argv[1:]))
    if not paths:
        print("nenhum .meta encontrado em: %s" % " ".join(argv[1:]))
        return 2

    ok = True
    for i, path in enumerate(paths):
        if i:
            print()
        try:
            ok = dump(path) and ok
        except OSError as e:
            print("%s\n  ERRO: %s" % (path, e))
            ok = False

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))

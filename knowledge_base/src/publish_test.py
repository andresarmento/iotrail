#!/usr/bin/env python3
"""iotrail - publicador de teste: conecta no broker MQTT (host/porta
hardcoded abaixo) e publica valores sequenciais 0-99 em "iotrail/test", em
loop, no intervalo configurado (--interval-ms). Ctrl+C pra parar.
"""

import argparse
import itertools
import sys
import time

import paho.mqtt.client as mqtt

BROKER_HOST = "192.168.0.115"
BROKER_PORT = 1883
TOPIC = "iotrail/test"


def main() -> int:
    parser = argparse.ArgumentParser(
        description='Publica valores sequenciais 0-99 em "iotrail/test", em loop.')
    parser.add_argument(
        "--interval-ms", type=int, default=1000,
        help="intervalo entre publicacoes, em ms (default: 1000)")
    args = parser.parse_args()

    if args.interval_ms <= 0:
        print("--interval-ms precisa ser positivo")
        return 1

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                          client_id="iotrail-publish-test")
    client.connect(BROKER_HOST, BROKER_PORT, keepalive=60)
    client.loop_start()

    print(f'[publish] conectado em {BROKER_HOST}:{BROKER_PORT}, publicando '
          f'em "{TOPIC}" a cada {args.interval_ms}ms (Ctrl+C pra parar)')
    try:
        for value in itertools.cycle(range(100)):
            client.publish(TOPIC, payload=str(value), qos=0)
            print(f"[publish] {TOPIC} = {value}")
            time.sleep(args.interval_ms / 1000)
    except KeyboardInterrupt:
        print("\n[publish] encerrando.")
    finally:
        client.loop_stop()
        client.disconnect()

    return 0


if __name__ == "__main__":
    sys.exit(main())

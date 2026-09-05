## Roadmap:

1. Fundação do projeto
Estrutura de build, organização de pastas, escolha de bibliotecas base (uma lib MQTT como Eclipse Paho C++ ou Mosquitto, e algo para logging). Definir o padrão C++ (C++17). logging, signal handling (ctrl+c), leitura de arquivo de configuracao (iotrail.conf), config: validação e regras do domínio.

2. Cliente MQTT
Conectar a um broker, subscrever tópicos configuráveis e receber mensagens de forma estável. Trate reconexão e QoS. Nessa etapa o "sink" pode ser só um print — o objetivo é ter o fluxo de entrada funcionando.

3. Formato de armazenamento (o coração do sistema)
Defina o layout do segmento em disco: como cada evento é serializado (offset, timestamp, tópico, payload, tamanho, checksum). Isso é a decisão mais importante e cara de mudar depois — vale prototipar antes de escrever muito código em volta.


## Fases seguintes pra decidir, só ideias:

4. Escrita em segmentos sequenciais
Append-only writer que grava eventos no segmento ativo e faz rollover para um novo segmento por tamanho ou tempo. Garanta flush/durabilidade mínima.

5. Índice
Um índice por segmento mapeando offset (e/ou timestamp) → posição no arquivo, para permitir busca sem varrer o segmento inteiro. Pode começar simples (índice esparso).

6. Leitura e replay
Um reader que lê a partir de um offset ou timestamp e entrega eventos em ordem. É o que habilita histórico e replay.

7. Consumo independente (offsets de consumidor)
Persistir a posição de cada consumidor para que dashboards, análise e IA leiam no próprio ritmo sem interferir uns nos outros.

8. Retenção
Política de descarte de segmentos antigos por idade ou tamanho total em disco. Importante para o alvo de edge/baixo armazenamento.

9. Configuração e operação
Arquivo de config (broker, tópicos, caminhos, limites de retenção/segmento) e logging operacional. Fecha o pacote de uma v0.1 usável.

Sugestão de corte para o MVP mais rápido possível: itens 1–6 já entregam a proposta central (ingestão MQTT + persistência + replay). Os itens 7–9 transformam num sistema realmente utilizável por múltiplas aplicações.
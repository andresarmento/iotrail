# IoTrail 

IoTrail é um sistema leve de persistência e streaming de eventos voltado a ambientes IoT, desenvolvido em C++. O sistema conecta-se como cliente a um ou mais brokers MQTT existentes, subscrevendo tópicos de interesse e registrando as mensagens recebidas em logs sequenciais persistentes, organizados em segmentos e índices próprios, sem depender de bancos de dados externos. Inspirado em conceitos utilizados por plataformas como Apache Kafka, o IoTrail busca oferecer recursos como histórico de eventos, replay, retenção e consumo independente por múltiplas aplicações — como dashboards, sistemas de análise e inteligência artificial — mantendo baixo consumo de memória, processamento e armazenamento, de forma adequada a redes IoT de pequeno e médio porte e dispositivos de edge computing.

## Algumas escolhas iniciais
* C++17 com STL
* Python para tooling



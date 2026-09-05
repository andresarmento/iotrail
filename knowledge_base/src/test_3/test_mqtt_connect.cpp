// test_3: teste isolado de conexao MQTT real (connect + subscribe), so pra
// validar a lib libmosquittopp (MSYS2 ucrt64) e o link com o g++ do projeto.
// Nao integra com fila/writer thread dos testes 1/2 - isso fica pra depois.
//
// Protocolo: MQTT 3.1.1 (default da lib, nao setamos nada explicito).

#include <mosquittopp.h>
#include <cstdio>
#include <cstring>
#include <csignal>

namespace {
volatile std::sig_atomic_t g_stop = 0;
void onSigint(int) { g_stop = 1; }
}  // namespace

class TestClient : public mosqpp::mosquittopp {
 public:
  TestClient(const char* id, const char* host, int port, const char* topic)
      : mosquittopp(id), topic_(topic) {
    printf("[client] conectando em %s:%d ...\n", host, port);
    int rc = connect(host, port, 60);
    if (rc != MOSQ_ERR_SUCCESS) {
      printf("[client] connect() falhou: %s\n", mosqpp::strerror(rc));
    }
  }

  void on_connect(int rc) override {
    if (rc == 0) {
      printf("[client] conectado. subscrevendo em \"%s\"...\n", topic_);
      subscribe(nullptr, topic_, 0);
    } else {
      printf("[client] conexao recusada, rc=%d (%s)\n", rc,
             mosqpp::connack_string(rc));
    }
  }

  void on_disconnect(int rc) override {
    printf("[client] desconectado, rc=%d\n", rc);
  }

  void on_subscribe(int mid, int qosCount, const int* grantedQos) override {
    printf("[client] subscribe confirmado (mid=%d, qos[0]=%d)\n", mid,
           qosCount > 0 ? grantedQos[0] : -1);
  }

  void on_message(const mosquitto_message* msg) override {
    printf("[msg] topico=\"%s\" payload=\"%.*s\" (%d bytes)\n", msg->topic,
           msg->payloadlen, static_cast<const char*>(msg->payload),
           msg->payloadlen);
  }

  void on_log(int, const char* str) override { printf("[log] %s\n", str); }

 private:
  const char* topic_;
};

int main(int argc, char** argv) {
  const char* host = argc > 1 ? argv[1] : "127.0.0.1";
  int port = argc > 2 ? std::atoi(argv[2]) : 1883;
  const char* topic = argc > 3 ? argv[3] : "iotrail/test3/#";

  std::signal(SIGINT, onSigint);
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  mosqpp::lib_init();

  TestClient client("iotrail-test3", host, port, topic);

  printf("[client] loop_forever ate Ctrl+C...\n");
  while (!g_stop) {
    int rc = client.loop(200);
    if (rc != MOSQ_ERR_SUCCESS) {
      printf("[client] loop() erro: %s, tentando reconectar...\n",
             mosqpp::strerror(rc));
      client.reconnect();
    }
  }

  printf("[client] encerrando.\n");
  client.disconnect();
  mosqpp::lib_cleanup();
  return 0;
}

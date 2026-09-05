#include "signals.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <mutex>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

namespace signals {
    static std::atomic<bool> stop_flag{false};

    // Teto da espera do handler de fechamento. Nao e' escolha nossa: o Windows
    // mata o processo quando o handler retorna OU quando o prazo dele estoura, o
    // que vier antes. Medido nesta maquina: ~5 s ate o corte (o valor sai do
    // registro do Windows e muda de maquina pra maquina). 3 s deixa margem -
    // estourar o nosso teto ainda sai pelo caminho normal; estourar o do SO e'
    // morte no meio do trabalho.
    static constexpr auto shutdown_grace = std::chrono::seconds(3);

    static std::mutex shutdown_mutex;
    static std::condition_variable shutdown_cv;
    static bool shutdown_finished = false;

    // Handler nao loga: logar chamaria malloc e travaria o mutex do spdlog em
    // contexto de sinal. Quem anuncia a parada e' o main, depois do laco - o
    // preco e' nao dar pra saber pelo log qual evento pediu pra parar.
    static void on_signal(int) {
        request_stop();
    }

#ifdef _WIN32
    // SIGINT no Windows cobre so Ctrl+C. Fechar a janela, Ctrl+Break, logoff e
    // shutdown do sistema chegam por aqui, e sem isto matariam o processo sem
    // parada ordenada.
    static BOOL WINAPI on_console_event(DWORD event) {
        switch (event) {
            case CTRL_C_EVENT:
            case CTRL_BREAK_EVENT:
                // Aqui TRUE basta: significa "tratado", nao cai no handler
                // default (que encerraria na hora) e o processo segue vivo ate o
                // main sair sozinho.
                request_stop();
                return TRUE;

            case CTRL_CLOSE_EVENT:
            case CTRL_LOGOFF_EVENT:
            case CTRL_SHUTDOWN_EVENT: {
                // Nestes tres o retorno nao segura nada: o Windows mata o
                // processo assim que este handler RETORNA. Medido com o handler
                // so setando a flag e voltando: fechar a janela matava em ~2 ms,
                // exit code 0xC000013A, antes de o main sequer acordar dos
                // 200 ms do polling. Ou seja, o trabalho de encerrar tem que
                // acontecer ENQUANTO estamos aqui dentro - por isso a espera.
                //
                // Hoje o main so loga e sai; na Fase 3 e' aqui que cabem a
                // drenagem das filas e o fsync final de cada stream.
                request_stop();
                std::unique_lock<std::mutex> lock(shutdown_mutex);
                shutdown_cv.wait_for(lock, shutdown_grace,
                                     [] { return shutdown_finished; });
                return TRUE;
            }

            default:
                return FALSE;
        }
    }
#endif

    void init() {
        // SIGTERM no Windows nao e' entregue por ninguem; esta aqui pelo porte
        // pra Linux, onde e' o sinal que um supervisor manda.
        std::signal(SIGINT, on_signal);
        std::signal(SIGTERM, on_signal);

#ifdef _WIN32
        SetConsoleCtrlHandler(on_console_event, TRUE);
#endif
    }

    // Publico: parar sem sinal do SO (API de gerencia, Fase 6/7) ja tem por onde
    // entrar, e os handlers escrevem a flag por aqui em vez de mexer nela direto.
    void request_stop() {
        stop_flag.store(true);
    }

    bool stop_requested() {
        return stop_flag.load();
    }

    void shutdown_done() {
        {
            std::lock_guard<std::mutex> lock(shutdown_mutex);
            shutdown_finished = true;
        }
        shutdown_cv.notify_all();
    }
}

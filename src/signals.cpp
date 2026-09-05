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

    // Este bloco é um tratamento para fechar corretamente o programa
    // por conta de casos de parada específicos do Windows
    static constexpr auto shutdown_grace = std::chrono::seconds(3);
    static std::mutex shutdown_mutex;
    static std::condition_variable shutdown_cv;
    static bool shutdown_finished = false;

    static void on_signal(int) {
        request_stop();
    }

#ifdef _WIN32
    static BOOL WINAPI on_console_event(DWORD event) {
        switch (event) {
            case CTRL_C_EVENT:
            case CTRL_BREAK_EVENT:
                request_stop();
                return TRUE;

            case CTRL_CLOSE_EVENT:
            case CTRL_LOGOFF_EVENT:
            case CTRL_SHUTDOWN_EVENT: {
                // Nestes três o retorno nao segura nada: o Windows mata o
                // processo assim que este handler retorna. 
                request_stop();
                // Espera o main avisar que terminou — mas não mais que 3 segundos.
                std::unique_lock<std::mutex> lock(shutdown_mutex);
                shutdown_cv.wait_for(lock, shutdown_grace, [] { 
                    return shutdown_finished; 
                });
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

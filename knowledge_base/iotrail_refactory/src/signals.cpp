#include "signals.h"

#include <atomic>
#include <csignal>

#ifdef _WIN32
    // Guardados porque o libstdc++ do MinGW ja define os dois - redefinir gera
    // warning. Com MSVC eles nao viriam de lugar nenhum.
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

namespace signals {

    // atomic e' o tipo certo aqui, e nao volatile sig_atomic_t: no Windows tanto
    // o handler de SIGINT quanto o do console rodam numa thread que o SO cria,
    // entao isto e' comunicacao entre threads, nao interrupcao de sinal.
    static std::atomic<bool> stop_flag{false};

    static void onSignal(int) {
        stop_flag.store(true);
    }

#ifdef _WIN32
    // SIGINT no Windows so cobre Ctrl+C. Fechar a janela do console, Ctrl+Break,
    // logoff e shutdown do sistema chegam por aqui.
    static BOOL WINAPI onConsoleEvent(DWORD event) {
        switch (event) {
            case CTRL_C_EVENT:
            case CTRL_BREAK_EVENT:
            case CTRL_CLOSE_EVENT:
            case CTRL_LOGOFF_EVENT:
            case CTRL_SHUTDOWN_EVENT:
                stop_flag.store(true);
                // TRUE = tratado, nao passa pro handler default (que encerraria
                // o processo na hora). Em CTRL_CLOSE_EVENT o Windows ainda mata
                // depois de poucos segundos, entao o encerramento tem que ser
                // rapido.
                return TRUE;
            default:
                return FALSE;
        }
    }
#endif

    void init() {
        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);

#ifdef _WIN32
        SetConsoleCtrlHandler(onConsoleEvent, TRUE);
#endif
    }

    bool stopRequested() {
        return stop_flag.load();
    }
}

#include "paths.h"

#include "logging.h"

#include <string>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <cerrno>
    #include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace paths {
    static constexpr char config_name[] = "iotrail.conf";
    static constexpr char usage[] = "uso: iotrail [-c <arquivo.conf>]";
    static constexpr size_t path_limit = 32768;

    fs::path exe_dir() {
#ifdef _WIN32
        std::wstring buf(MAX_PATH, L'\0');
        for (;;) {
            DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
            if (n == 0) {
                logging::error("nao consegui descobrir o caminho do executavel (erro {})",
                               GetLastError());
                return {};
            }
            if (n < buf.size()) {
                buf.resize(n);
                break;
            }
            if (buf.size() >= path_limit) {
                logging::error("caminho do executavel longo demais");
                return {};
            }
            buf.resize(buf.size() * 2);
        }
#else
        // readlink não termina em '\0' e trunca calado, igual ao Windows:
        // devolver o tamanho do buffer é o sinal de "não coube".
        // Não testado - está aqui pelo porte pra Linux.
        std::string buf(1024, '\0');
        for (;;) {
            ssize_t n = readlink("/proc/self/exe", buf.data(), buf.size());
            if (n < 0) {
                logging::error("nao consegui ler /proc/self/exe (errno {})", errno);
                return {};
            }
            if (static_cast<size_t>(n) < buf.size()) {
                buf.resize(static_cast<size_t>(n));
                break;
            }
            if (buf.size() >= path_limit) {
                logging::error("caminho do executavel longo demais");
                return {};
            }
            buf.resize(buf.size() * 2);
        }
#endif
        return fs::path(buf).parent_path();
    }

    fs::path config_from_args(int argc, char* argv[]) {
        fs::path from_flag;

        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "-c") {
                if (i + 1 >= argc) {
                    logging::error("-c exige o caminho do arquivo. {}", usage);
                    return {};
                }
                from_flag = argv[++i];
            } else {
                logging::error("argumento desconhecido: {}. {}", arg, usage);
                return {};
            }
        }

        if (!from_flag.empty()) {
            return from_flag;
        }

        fs::path dir = exe_dir();
        if (dir.empty()) {
            return {};
        }
        return dir / config_name;
    }
}

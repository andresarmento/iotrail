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

}

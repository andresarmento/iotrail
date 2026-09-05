#include "paths.h"

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <unistd.h>
#endif

namespace paths {

    std::filesystem::path executableDir() {
#ifdef _WIN32
        std::wstring buffer(4096, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                                static_cast<DWORD>(buffer.size()));
        if (length == 0 || length == buffer.size()) return {};
        buffer.resize(length);
        return std::filesystem::path(buffer).parent_path();
#else
        std::string buffer(4096, '\0');
        const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length <= 0 || static_cast<size_t>(length) == buffer.size()) return {};
        buffer.resize(static_cast<size_t>(length));
        return std::filesystem::path(buffer).parent_path();
#endif
    }
}

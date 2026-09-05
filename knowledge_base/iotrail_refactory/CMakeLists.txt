# IoTrail - base reescrita.
#
# Itens 1.1 (esqueleto de build) e 1.2 (logging) do TODO.md.
#
# Configurar e buildar (PowerShell), da raiz de iotrail_refactory/:
#     cmake -S . -B build -G Ninja
#     cmake --build build
#
# Minimo 3.21 por causa de TARGET_RUNTIME_DLLS, usado na copia de DLLs no fim
# deste arquivo. O CMake desta maquina e' 4.4.2.
cmake_minimum_required(VERSION 3.21)
project(iotrail VERSION 0.1.0 LANGUAGES CXX)

# C++17 (decidido 2026-09-02, item 1.1) - mantem o padrao ja registrado em
# claude_memory/CLAUDE.md, secao "Escolhas tecnicas iniciais".
#
# C++20 foi cogitado e descartado nesta rodada. O que teria trazido, pra nao
# refazer a analise do zero se o assunto voltar:
#   - std::jthread + std::stop_token, que substituiriam o par
#     std::atomic<bool> + std::thread + join manual usado hoje em
#     src/segment_writer.h:51-52 - padrao que vai se repetir uma vez por stream.
#   - std::span no lugar do par (const void*, size_t) de src/crc32.h:50,60.
# Confirmado por teste que o GCC 16.2.0 do MSYS2 ucrt64 compila e roda os dois,
# entao a limitacao nao e' do toolchain - subir depois continua sendo possivel.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Sem extensoes GNU: -std=c++17, nao -std=gnu++17. Codigo que so compila com
# extensao de compilador e' o tipo de coisa que so aparece na primeira vez que o
# projeto for compilado em outro lugar.
set(CMAKE_CXX_EXTENSIONS OFF)

# Gera build/compile_commands.json com a linha de compilacao real de cada fonte.
# E' o que o IntelliSense do VS Code le (.vscode/c_cpp_properties.json na raiz do
# repo) - assim ele acompanha o build sozinho, sem lista de includes mantida na
# mao.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Raiz da instalacao MSYS2 ucrt64 - fonte das libs de terceiros (spdlog agora,
# mosquitto na Fase 2). Variavel de cache pra dar pra apontar pra outro lugar
# na linha de comando (-D IOTRAIL_MSYS2_UCRT64=...) sem editar este arquivo.
set(IOTRAIL_MSYS2_UCRT64 "C:/msys64/ucrt64" CACHE PATH "Raiz da instalacao MSYS2 ucrt64")
list(APPEND CMAKE_PREFIX_PATH "${IOTRAIL_MSYS2_UCRT64}")

# spdlog tem pacote CMake proprio no MSYS2 (lib/cmake/spdlog), entao
# find_package resolve include e link sozinho - nao precisa de
# target_include_directories/target_link_directories na mao.
#
# O alvo spdlog::spdlog e' a lib COMPARTILHADA (decidido 2026-09-02, item 1.2).
# O pacote tambem exporta spdlog::spdlog_header_only, que dispensaria a DLL ao
# custo de recompilar os headers do spdlog em cada .cpp e de um .exe bem maior
# (medido: 468 KB compartilhada contra 5,6 MB header-only + link estatico).
find_package(spdlog CONFIG REQUIRED)

# A libmosquitto do MSYS2 nao traz pacote CMake (so os .dll.a e o header), entao
# o alvo importado e' montado na mao. Vale o trabalho: assim ela entra em
# TARGET_RUNTIME_DLLS junto com a spdlog e a copia de DLL continua automatica,
# sem lista de nomes mantida em texto.
#
# Escolhida a API C (libmosquitto), nao o wrapper C++ (libmosquittopp) -
# decidido 2026-09-03, item 2.1: o wrapper nao expoe o struct mosquitto*
# interno, o que bloqueia MQTT5 (ver claude_memory/CLAUDE.md).
find_library(IOTRAIL_MOSQUITTO_LIB NAMES mosquitto REQUIRED)
add_library(mosquitto::mosquitto SHARED IMPORTED)
set_target_properties(mosquitto::mosquitto PROPERTIES
    IMPORTED_LOCATION "${IOTRAIL_MSYS2_UCRT64}/bin/libmosquitto.dll"
    IMPORTED_IMPLIB "${IOTRAIL_MOSQUITTO_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${IOTRAIL_MSYS2_UCRT64}/include")

# Um alvo so, fontes direto em src/ (decidido 2026-09-02, item 1.1).
add_executable(iotrail
    src/main.cpp
    src/logging.cpp
    src/signals.cpp
    src/paths.cpp
    src/mqtt_client.cpp
    src/config/ini.cpp
    src/config/config.cpp
)

# Os dois diretorios de fonte entram no include path, entao os includes sao
# sempre pelo nome do arquivo ("ini.h", "logging.h") - nao ha include relativo
# atravessando pastas ("config/ini.h" ou "../logging.h").
target_include_directories(iotrail PRIVATE src src/config)

# Flags de GCC/Clang. Sem -Werror de proposito: um aviso novo (compilador
# atualizado, header de terceiro) nao deve quebrar o build no meio do trabalho.
# Adicionar depois e' uma linha.
target_compile_options(iotrail PRIVATE -Wall -Wextra -Wpedantic)

target_link_libraries(iotrail PRIVATE spdlog::spdlog mosquitto::mosquitto)

# NAO ha -static-libgcc/-static-libstdc++ aqui, ao contrario do CMakeLists.txt
# antigo na raiz do repo. Motivo, medido em 2026-09-02: a libspdlog-1.17.dll foi
# compilada contra a libstdc++ dinamica (confirmado com objdump -p na propria
# DLL), entao libstdc++-6.dll acompanha o programa de qualquer forma. Linkar o
# nosso codigo estatico nao eliminaria DLL nenhuma - so faria existirem DOIS
# runtimes C++ no mesmo processo, o estatico do nosso lado e o dinamico dentro
# da DLL, com std::string cruzando essa fronteira.
#
# O raciocinio depende de a spdlog ser compartilhada. Na rota header-only nao ha
# DLL de spdlog, e ai -static passa a fazer sentido e produz um .exe sem nenhuma
# DLL de terceiro (medido e confirmado rodando).

# Copia as DLLs pro lado do .exe apos cada build, pra rodar de dentro da pasta
# de build. Duas listas, porque tem origens diferentes:
#
# 1. TARGET_RUNTIME_DLLS resolve sozinho as DLLs dos alvos importados
#    (libspdlog-1.17.dll hoje, mosquitto na Fase 2). Preferido a uma lista na
#    mao porque nao carrega o numero de versao no nome - o CMakeLists.txt antigo
#    fixa "libspdlog-1.17.dll" em texto, que quebra silenciosamente no proximo
#    upgrade do pacote.
add_custom_command(TARGET iotrail POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_RUNTIME_DLLS:iotrail>" "$<TARGET_FILE_DIR:iotrail>"
    COMMAND_EXPAND_LISTS
    VERBATIM)

# 2. O runtime do compilador nao vem de alvo importado nenhum, entao fica na
#    mao. Estas tres sao dependencia da libspdlog-1.17.dll, nao do nosso codigo
#    (verificado com objdump -p na DLL). Nao ha libfmt-12.dll na lista: nesta
#    versao do pacote o fmt esta estatico dentro da libspdlog.dll - o
#    CMakeLists.txt antigo copia essa DLL sem precisar.
set(IOTRAIL_RUNTIME_DLLS
    libstdc++-6.dll
    libgcc_s_seh-1.dll
    libwinpthread-1.dll
)
foreach(dll IN LISTS IOTRAIL_RUNTIME_DLLS)
    add_custom_command(TARGET iotrail POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${IOTRAIL_MSYS2_UCRT64}/bin/${dll}" "$<TARGET_FILE_DIR:iotrail>"
        VERBATIM)
endforeach()

# O programa procura o iotrail.conf no proprio diretorio (paths::executableDir),
# entao a copia nao e' conveniencia - sem ela nao ha config nenhuma ao rodar do
# build. A fonte e' o iotrail.conf da raiz; a copia e' descartavel e sobrescrita
# sempre que a fonte mudar.
add_custom_command(TARGET iotrail POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/iotrail.conf" "$<TARGET_FILE_DIR:iotrail>"
    VERBATIM)

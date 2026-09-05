cmake_minimum_required(VERSION 3.20)
project(iotrail CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Gera build/iotrail/compile_commands.json com a linha de compilacao real de
# cada fonte. E' o que o IntelliSense do VS Code le (.vscode/c_cpp_properties.json)
# pra achar os headers do MSYS2 - assim ele acompanha o build sozinho, sem uma
# lista de includes mantida na mao que desatualiza a cada lib nova.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Raiz da instalacao MSYS2 ucrt64 - fonte das libs de terceiros (mosquitto
# hoje, outras no futuro via pacman). Ver claude_memory/CLAUDE.md, secao
# "Build / ambiente", para a decisao de unificar o toolchain nela.
set(MSYS2_UCRT64 "C:/msys64/ucrt64" CACHE PATH "Raiz da instalacao MSYS2 ucrt64")
list(APPEND CMAKE_PREFIX_PATH ${MSYS2_UCRT64})

# spdlog tem pacote CMake proprio no MSYS2 (lib/cmake/spdlog); mosquitto nao,
# por isso continua incluido/linkado na mao logo abaixo.
find_package(spdlog CONFIG REQUIRED)

# Fontes diretas em src/ por enquanto (sem include/ separado - prematuro com
# poucos arquivos; ver claude_memory/TODO.md Fase 1 pro plano de separar
# quando o codigo crescer mais).
add_executable(iotrail
    src/main.cpp
    src/iotrail_client.cpp
    src/config.cpp
    src/segment_writer.cpp
    src/init.cpp
)

target_include_directories(iotrail PRIVATE ${MSYS2_UCRT64}/include)
target_link_directories(iotrail PRIVATE ${MSYS2_UCRT64}/lib)
target_link_libraries(iotrail PRIVATE mosquittopp mosquitto spdlog::spdlog)

# Runtime do compilador estatico, libs de terceiros dinamicas (decisao
# 2026-08-28, ver TODO.md Fase 1).
target_link_options(iotrail PRIVATE -static-libgcc -static-libstdc++)

# Copia as DLLs de terceiros e o iotrail.conf pro lado do executavel apos
# cada build, pra rodar direto de dentro da pasta de build (mesma convencao
# dos prototipos test_0..test_3).
set(IOTRAIL_RUNTIME_DLLS
    libmosquittopp.dll
    libmosquitto.dll
    libgcc_s_seh-1.dll
    libstdc++-6.dll
    libcrypto-3-x64.dll
    libssl-3-x64.dll
    libwinpthread-1.dll
    libspdlog-1.17.dll
    libfmt-12.dll
)
foreach(dll ${IOTRAIL_RUNTIME_DLLS})
    add_custom_command(TARGET iotrail POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MSYS2_UCRT64}/bin/${dll}"
            "$<TARGET_FILE_DIR:iotrail>"
    )
endforeach()

add_custom_command(TARGET iotrail POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/config/iotrail.conf"
        "$<TARGET_FILE_DIR:iotrail>"
)

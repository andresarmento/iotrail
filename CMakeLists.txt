# IoTrail - build (tarefas 1.1 e 1.2 do docs/TODO.md).
#
# Configurar e buildar, da raiz do projeto:
#     cmake -S . -B build -G Ninja
#     cmake --build build
#
# Minimo 3.21 por causa de TARGET_RUNTIME_DLLS (usado na copia de DLLs no fim
# deste arquivo), que resolve sozinho as DLLs dos alvos importados - sem lista
# com numero de versao fixo em texto, que quebra calado no proximo upgrade.
cmake_minimum_required(VERSION 3.21)
project(iotrail VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# -std=c++17, nao -std=gnu++17. Codigo que so compila com extensao de
# compilador e' o tipo de coisa que so aparece na primeira vez que o projeto
# for compilado em outro lugar.
set(CMAKE_CXX_EXTENSIONS OFF)

# Linha de compilacao real de cada fonte, pro IntelliSense acompanhar o build
# sem lista de includes mantida na mao.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Ninja e' single-config: sem isto o build sai sem -O e sem -g, nem otimizado
# nem depuravel, e nada avisa. RelWithDebInfo como default (decidido
# 2026-09-05) porque o projeto existe pra medir latencia em edge - numero
# medido em build -O0 nao vale nada.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Tipo de build" FORCE)
endif()
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
    Debug Release RelWithDebInfo MinSizeRel)

# Raiz do MSYS2 ucrt64 - fonte das libs de terceiros (spdlog na 1.2, mosquitto
# na Fase 2) e do runtime do compilador. Variavel de cache pra apontar pra
# outra instalacao por linha de comando sem editar este arquivo.
set(IOTRAIL_MSYS2_UCRT64 "C:/msys64/ucrt64" CACHE PATH "Raiz da instalacao MSYS2 ucrt64")
list(APPEND CMAKE_PREFIX_PATH "${IOTRAIL_MSYS2_UCRT64}")

# spdlog tem pacote CMake proprio no MSYS2 (lib/cmake/spdlog), entao
# find_package resolve include, link e dependencias sozinho - nada de
# target_include_directories/target_link_directories na mao.
#
# O alvo spdlog::spdlog e' a lib COMPARTILHADA. O pacote tambem exporta
# spdlog::spdlog_header_only, que dispensaria a DLL ao custo de recompilar os
# headers do spdlog em cada .cpp e de um .exe bem maior (medido na rodada
# anterior: 468 KB compartilhada contra 5,6 MB header-only + link estatico). E'
# esta escolha que sustenta a decisao de runtime C++ dinamico ali em cima.
find_package(spdlog CONFIG REQUIRED)

# Um alvo so, fontes direto no add_executable. Biblioteca + executavel fino so
# se pagaria pra um teste linkar o codigo, e o framework de teste ficou fora da
# Fase 1 - reabrir quando entrar.
add_executable(iotrail
    src/main.cpp
    src/logging.cpp
)

# Todo diretorio de fonte entra no include path, entao include e' sempre pelo
# nome do arquivo ("logging.h"), nunca relativo atravessando pasta.
target_include_directories(iotrail PRIVATE src)

target_link_libraries(iotrail PRIVATE spdlog::spdlog)

# -Werror de proposito: o projeto comeca com zero aviso, e manter em zero e'
# mais barato que limpar depois. Se um upgrade do GCC quebrar o build por um
# aviso novo, o escape e' -DCMAKE_CXX_FLAGS=-Wno-error.
#
# So o NOSSO codigo: a flag e' por alvo, entra apenas na linha dos .cpp do
# IoTrail. spdlog e mosquitto vem compiladas em DLL pelo pacman, nunca passam
# por este build. A unica exposicao sao os HEADERS delas dentro dos nossos
# .cpp - e essa o CMake ja fecha sozinho, porque include de alvo importado
# entra como -isystem, que silencia aviso do header de terceiro. Nao adicionar
# -isystem na mao pra ucrt64/include: reordena a busca e quebra o
# #include_next da libstdc++ (testado - vira "fatal error: stdlib.h").
target_compile_options(iotrail PRIVATE -Wall -Wextra -Wpedantic -Werror)

# Sem -static-libgcc/-static-libstdc++, ao contrario do CMakeLists.txt do
# projeto antigo. Na rota escolhida (spdlog compartilhada) a libspdlog-1.17.dll
# e' compilada contra a libstdc++ dinamica, entao a DLL acompanha o programa de
# qualquer forma - linkar estatico nao eliminaria DLL nenhuma, so faria existir
# dois runtimes C++ no mesmo processo com std::string cruzando a fronteira.

# Copia as DLLs pro lado do .exe apos cada build, pra rodar de dentro da pasta
# de build sem depender do PATH apontar pro MSYS2. Sao duas listas porque tem
# origens diferentes.
#
# 1. DLL de alvo importado: TARGET_RUNTIME_DLLS resolve sozinho (libspdlog-1.17
#    hoje, mosquitto na Fase 2). Preferido a uma lista escrita na mao porque nao
#    carrega o numero de versao no nome - o CMakeLists.txt do projeto antigo
#    fixava "libspdlog-1.17.dll" em texto, e isso quebra calado no proximo
#    upgrade do pacote.
add_custom_command(TARGET iotrail POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_RUNTIME_DLLS:iotrail>" "$<TARGET_FILE_DIR:iotrail>"
    COMMAND_EXPAND_LISTS
    VERBATIM)

# 2. Runtime do compilador: nao vem de alvo importado nenhum, entao fica na mao.
#    Esta lista nao foi copiada de lugar nenhum - e' exatamente o que o
#    `objdump -p build/iotrail.exe` aponta hoje, tirando as api-ms-win-crt-* e a
#    KERNEL32, que sao do proprio Windows. Nao ha libfmt-12.dll: o pacote do
#    MSYS2 compila spdlog com SPDLOG_FMT_EXTERNAL, mas o fmt entra estatico na
#    libspdlog (conferido com objdump -p na propria DLL).
#    Conferir de novo sempre que uma dependencia nova entrar.
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

# O programa procura o iotrail.conf no proprio diretorio (tarefa 1.4), entao
# esta copia nao e' conveniencia: sem ela nao ha config ao rodar do build. A
# fonte e' o iotrail.conf da raiz; a copia e' descartavel.
add_custom_command(TARGET iotrail POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/iotrail.conf" "$<TARGET_FILE_DIR:iotrail>"
    VERBATIM)

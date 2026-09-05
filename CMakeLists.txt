# IoTrail - esqueleto de build (tarefa 1.1 do docs/TODO.md).
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

# Um alvo so, fontes direto no add_executable. Biblioteca + executavel fino so
# se pagaria pra um teste linkar o codigo, e o framework de teste ficou fora da
# Fase 1 - reabrir quando entrar.
add_executable(iotrail
    src/main.cpp
)

# Todo diretorio de fonte entra no include path, entao include e' sempre pelo
# nome do arquivo ("logging.h"), nunca relativo atravessando pasta.
target_include_directories(iotrail PRIVATE src)

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

# Sem copia de DLL ainda: com main.cpp vazio o .exe depende so das
# api-ms-win-crt-*/KERNEL32 do proprio Windows (conferido com objdump -p).
# libstdc++-6.dll e libgcc_s_seh-1.dll passam a ser necessarias na 1.2, junto
# com a libspdlog - la a lista sai de TARGET_RUNTIME_DLLS mais o que o objdump
# apontar, nao de lista copiada de outro projeto.

# O programa procura o iotrail.conf no proprio diretorio (tarefa 1.4), entao
# esta copia nao e' conveniencia: sem ela nao ha config ao rodar do build. A
# fonte e' o iotrail.conf da raiz; a copia e' descartavel.
add_custom_command(TARGET iotrail POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/iotrail.conf" "$<TARGET_FILE_DIR:iotrail>"
    VERBATIM)

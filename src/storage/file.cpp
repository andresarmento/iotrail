/*
 *  IoTrail - sistema leve de persistencia e streaming de eventos para IoT.
 *  Copyright Andre Sarmento - 2026
 *
 *  Camada de plataforma: descritor de arquivo com sync e truncate
 */
#ifndef _WIN32
// Tem que vir antes de qualquer header do sistema, inclusive dos que o file.h
// puxa. Com -std=c++17 (e nao gnu++17) o __STRICT_ANSI__ fica definido e a
// glibc esconde fsync/fdatasync/ftruncate; e sem _FILE_OFFSET_BITS o off_t de
// um ARM 32 bits tem 32 bits, o que limitaria o arquivo a 2 GiB.
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#endif

#include "file.h"

#include <cerrno>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

    // Pedaco maximo por chamada: o _write do Windows recebe unsigned int e o
    // write do POSIX nao promete escrever mais que SSIZE_MAX de uma vez. Os
    // nossos lotes ficam muito abaixo disso, mas o laco de read/write nao custa
    // nada e tira o teto da conversa.
    constexpr size_t io_chunk_max = 1u << 30;

    int open_fd(const fs::path& path, bool exclusive) {
#ifdef _WIN32
        // _O_BINARY e' obrigatorio: sem ele o CRT traduz \n em \r\n na escrita
        // e desfaz na leitura, corrompendo todo 0x0A do payload. E' _wopen e
        // nao _open porque no Windows o path guarda wchar_t nativo (decidido na
        // 1.4, paths.h:13) - caminho acentuado nao sobrevive a conversao pra
        // codepage.
        const int flags = _O_RDWR | _O_BINARY | (exclusive ? (_O_CREAT | _O_EXCL) : 0);
        return _wopen(path.c_str(), flags, _S_IREAD | _S_IWRITE);
#else
        // Sem O_APPEND de proposito. Ele forcaria toda escrita pro fim do
        // arquivo, e a varredura da 3.8 trunca o rabo corrompido e volta a
        // gravar exatamente em pos - alem de reescrever o header de 14 bytes no
        // byte 0 quando o base_offset diverge (FORMATO.md secao 8).
        const int flags = O_RDWR | (exclusive ? (O_CREAT | O_EXCL) : 0);
        return ::open(path.c_str(), flags, 0644);
#endif
    }

    void close_fd(int fd) {
        // Erro de close e' ignorado: em teoria ele pode reportar falha de
        // writeback atrasada, mas o writer sempre chama sync() antes de fechar,
        // entao essa falha ja apareceu la - onde da pra fazer algo com ela.
#ifdef _WIN32
        _close(fd);
#else
        ::close(fd);
#endif
    }
}

namespace storage {

    file::~file() {
        close();
    }

    file::file(file&& other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }

    file& file::operator=(file&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    void file::close() {
        if (fd_ >= 0) {
            close_fd(fd_);
            fd_ = -1;
        }
    }

    bool file::open(const fs::path& path) {
        close();

        // O_EXCL primeiro pra saber se fomos NOS que criamos o arquivo, sem uma
        // janela entre um exists() e o open. So quem cria precisa sincronizar o
        // diretorio; reabrir arquivo que ja existia nao mexe na entrada dele.
        bool created = true;
        fd_ = open_fd(path, true);
        if (fd_ < 0 && errno == EEXIST) {
            created = false;
            fd_ = open_fd(path, false);
        }
        if (fd_ < 0) {
            return false;
        }

        // Falha aqui derruba o open inteiro em vez de virar aviso: o arquivo
        // existe mas a entrada dele nao esta garantida, e seguir gravando seria
        // prometer durabilidade que o disco nao confirmou.
        if (created && !sync_dir(path.parent_path())) {
            const int saved = errno;
            close();
            errno = saved;
            return false;
        }
        return true;
    }

    int64_t file::size() const {
        // fstat e nao lseek(SEEK_END): o lseek moveria a posicao, e a varredura
        // da 3.8 pede o tamanho no meio da leitura.
#ifdef _WIN32
        struct _stat64 st;
        if (_fstat64(fd_, &st) != 0) {
            return -1;
        }
#else
        struct stat st;
        if (::fstat(fd_, &st) != 0) {
            return -1;
        }
#endif
        return static_cast<int64_t>(st.st_size);
    }

    bool file::seek(int64_t pos) {
#ifdef _WIN32
        return _lseeki64(fd_, pos, SEEK_SET) >= 0;
#else
        return ::lseek(fd_, static_cast<off_t>(pos), SEEK_SET) >= 0;
#endif
    }

    bool file::read(void* buf, size_t len) {
        auto* p = static_cast<unsigned char*>(buf);
        while (len > 0) {
            const size_t chunk = len < io_chunk_max ? len : io_chunk_max;
#ifdef _WIN32
            const int n = _read(fd_, p, static_cast<unsigned int>(chunk));
#else
            const ssize_t n = ::read(fd_, p, chunk);
#endif
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            if (n == 0) {
                // Fim de arquivo com bytes faltando. A varredura confere os
                // tamanhos antes de ler (FORMATO.md secao 8), entao chegar aqui
                // e' o arquivo ter encolhido debaixo de nos.
                errno = EIO;
                return false;
            }
            p += n;
            len -= static_cast<size_t>(n);
        }
        return true;
    }

    bool file::write(const void* data, size_t len) {
        const auto* p = static_cast<const unsigned char*>(data);
        while (len > 0) {
            const size_t chunk = len < io_chunk_max ? len : io_chunk_max;
#ifdef _WIN32
            const int n = _write(fd_, p, static_cast<unsigned int>(chunk));
#else
            const ssize_t n = ::write(fd_, p, chunk);
#endif
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            if (n == 0) {
                errno = EIO;
                return false;
            }
            p += n;
            len -= static_cast<size_t>(n);
        }
        return true;
    }

    bool file::sync() {
        // NUNCA repetir depois de falha. No Linux o erro de writeback e'
        // entregue uma vez so e as paginas sujas ja foram descartadas: um
        // segundo sync devolve 0 sem que nada tenha ido pro disco. Foi o que
        // derrubou o PostgreSQL em 2018. EINTR e' outra coisa - ali o sync nem
        // chegou a acontecer, e refazer e' o certo.
#ifdef _WIN32
        return _commit(fd_) == 0;
#else
        // fdatasync e nao fsync: num append o unico metadado que precisa ir
        // junto do conteudo e' o tamanho novo do arquivo, e esse o fdatasync
        // garante. O que ele pula e' mtime/atime, que nao muda a releitura.
        while (::fdatasync(fd_) != 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        return true;
#endif
    }

    bool file::truncate(int64_t len) {
#ifdef _WIN32
        // _chsize_s devolve o errno em vez de setar a variavel global.
        const int err = _chsize_s(fd_, len);
        if (err != 0) {
            errno = err;
            return false;
        }
        return true;
#else
        while (::ftruncate(fd_, static_cast<off_t>(len)) != 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        return true;
#endif
    }

    bool sync_dir(const fs::path& dir) {
#ifdef _WIN32
        (void)dir;
        return true;
#else
        // fsync e nao fdatasync: aqui e' o metadado que interessa, que e'
        // justamente o que o fdatasync tem licenca pra pular.
        const int fd = ::open(dir.c_str(), O_RDONLY);
        if (fd < 0) {
            return false;
        }
        bool ok = true;
        while (::fsync(fd) != 0) {
            if (errno == EINTR) {
                continue;
            }
            ok = false;
            break;
        }
        const int saved = errno;
        ::close(fd);
        errno = saved;
        return ok;
#endif
    }
}

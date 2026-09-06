#include "cmdline.h"

#include "logging.h"
#include "paths.h"

#include <algorithm>
#include <string>

namespace cmdline {
    static constexpr char config_name[] = "iotrail.conf";
    static constexpr char usage[] = "uso: iotrail [-c <arquivo.conf>] [-v|-vv]";
    static constexpr int max_verbose = 2;

    static bool is_verbose_flag(const std::string& arg) {
        return arg.size() >= 2 && arg[0] == '-' &&
               arg.find_first_not_of('v', 1) == std::string::npos;
    }

    std::optional<options> parse(int argc, char* argv[]) {
        options opts;

        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];

            if (arg == "-c") {
                if (i + 1 >= argc) {
                    logging::error("-c exige o caminho do arquivo. {}", usage);
                    return std::nullopt;
                }
                // argv e' narrow e no Windows vem na codepage ANSI: caminho
                // acentuado aqui pode nao sobreviver. Saida: CommandLineToArgvW.
                opts.config = argv[++i];
            } else if (is_verbose_flag(arg)) {
                const int count = static_cast<int>(arg.size()) - 1;
                opts.verbose = std::min(opts.verbose + count, max_verbose);
            } else {
                logging::error("argumento desconhecido: {}. {}", arg, usage);
                return std::nullopt;
            }
        }

        if (opts.config.empty()) {
            // Ao lado do .exe, nao no diretorio de trabalho, que muda conforme
            // quem chama.
            const std::filesystem::path dir = paths::exe_dir();
            if (dir.empty()) {
                return std::nullopt;
            }
            opts.config = dir / config_name;
        }

        return opts;
    }
}

// kv-cli -- one command, one answer, exit.
//
// Thin on purpose. It builds a Client, calls one of the three KVStore methods,
// and prints the result: it does not know the wire format exists, because the
// Client is a KVStore and that is the whole benefit of having made it one.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "kvstore/client.hpp"
#include "kvstore/status.hpp"

namespace {

struct Args {
    std::string host = "127.0.0.1";
    std::uint16_t port = 7379;
    std::vector<std::string> command;
    bool help = false;
};

void print_usage() {
    std::fprintf(stderr,
                 "usage: kv-cli [--host ADDR] [--port N] <command>\n"
                 "\n"
                 "  get KEY\n"
                 "  put KEY VALUE\n"
                 "  delete KEY\n"
                 "\n"
                 "The value of a successful get goes to stdout as raw bytes with\n"
                 "no trailing newline, so `kv-cli get k > file` writes exactly\n"
                 "what was stored. Everything else goes to stderr.\n"
                 "\n"
                 "Exit status: 0 ok, 1 not found, 2 error.\n");
}

bool parse_args(int argc, char** argv, Args* args) {
    int i = 1;
    for (; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool has_value = (i + 1) < argc;

        if (arg == "--help" || arg == "-h") {
            args->help = true;
            return true;
        }
        if (arg == "--host" && has_value) {
            args->host = argv[++i];
        } else if (arg == "--port" && has_value) {
            const long value = std::strtol(argv[++i], nullptr, 10);
            if (value < 1 || value > 65535) {
                std::fprintf(stderr, "kv-cli: port out of range: %ld\n", value);
                return false;
            }
            args->port = static_cast<std::uint16_t>(value);
        } else if (arg.rfind("--", 0) == 0) {
            std::fprintf(stderr, "kv-cli: unrecognised option: %s\n", arg.c_str());
            return false;
        } else {
            break;  // First non-option argument: the command starts here.
        }
    }
    for (; i < argc; ++i) {
        args->command.emplace_back(argv[i]);
    }
    return true;
}

// Raw bytes, no formatting, no newline. A value may contain anything -- NULs,
// 0x0A, invalid UTF-8 -- and this is a byte store, not a text one.
void write_value(const std::string& value) {
    if (!value.empty()) {
        std::fwrite(value.data(), 1, value.size(), stdout);
    }
    std::fflush(stdout);
    // The newline goes to stderr so that a terminal looks tidy while a
    // redirected stdout stays byte-exact.
    std::fputc('\n', stderr);
}

int exit_code_for(const kvstore::Status& status) {
    if (status.is_ok()) {
        return 0;
    }
    return status.is_not_found() ? 1 : 2;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // Without this the CRT rewrites every 0x0A on the way out as 0x0D 0x0A and
    // silently corrupts any value containing one. Precisely the same problem
    // O_BINARY solves for the record format on the way to disk -- and it has to
    // be solved again here, because it is a property of the stream, not the data.
    (void)_setmode(_fileno(stdout), _O_BINARY);
#endif

    Args args;
    if (!parse_args(argc, argv, &args)) {
        print_usage();
        return 2;
    }
    if (args.help || args.command.empty()) {
        print_usage();
        return args.help ? 0 : 2;
    }

    const std::string& verb = args.command[0];

    // Checked before connecting: no point opening a socket to be told the
    // command was wrong.
    const std::size_t expected_args = (verb == "put") ? 3 : 2;
    if ((verb != "get" && verb != "put" && verb != "delete") ||
        args.command.size() != expected_args) {
        std::fprintf(stderr, "kv-cli: bad command\n");
        print_usage();
        return 2;
    }

    auto connected = kvstore::Client::connect(args.host, args.port);
    if (!connected.is_ok()) {
        std::fprintf(stderr, "kv-cli: %s\n", connected.status().to_string().c_str());
        return 2;
    }
    std::unique_ptr<kvstore::Client> client = connected.take();

    const std::string& key = args.command[1];

    if (verb == "get") {
        std::string value;
        const kvstore::Status status = client->get(key, &value);
        if (status.is_ok()) {
            write_value(value);
        } else {
            std::fprintf(stderr, "%s\n", status.to_string().c_str());
        }
        return exit_code_for(status);
    }

    const kvstore::Status status = (verb == "put") ? client->put(key, args.command[2])
                                                   : client->remove(key);
    std::fprintf(stderr, "%s\n", status.to_string().c_str());
    return exit_code_for(status);
}

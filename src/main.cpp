#include "server.hpp"

#include "banner.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc, char* argv[]) {
    print_startup_banner("lazyverilog-lsp", argc, argv);

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0) {
            std::cout << "lazyverilog-lsp " << LAZYVERILOG_VERSION << "\n";
            return 0;
        }
    }

#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    {
        LazyVerilogServer server;
        server.run();
    }
    // Not `return`: exit() flushes every stdio stream, stdin included, and the
    // transport's detached reader thread holds stdin's lock while it blocks in
    // read().  With glibc's locking _IO_flush_all that waits until the client
    // closes the pipe, so `exit` never ends the process.  Everything that had
    // to finish was joined by ~LazyVerilogServer; flush what we wrote and go.
    std::cout.flush();
    std::cerr.flush();
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(0);
}

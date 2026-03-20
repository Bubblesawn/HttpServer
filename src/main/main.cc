#include <iostream>
#include <csignal>
#include <cstdlib>
#include <getopt.h>
#include "../server/http_server.h"

static HttpServer* g_server = nullptr;

void signalHandler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        std::cout << "\nShutting down server..." << std::endl;
        if (g_server) {
            g_server->stop();
        }
        exit(0);
    }
}

void printUsage(const char* programName) {
    std::cout << "Usage: " << programName << " [OPTIONS]\n"
              << "Options:\n"
              << "  -p, --port PORT        Server port (default: 8080)\n"
              << "  -d, --doc-root DIR     Document root directory (default: ./html_docs)\n"
              << "  -t, --threads NUM      Number of threads (default: 4)\n"
              << "  -h, --help            Show this help message\n"
              << "  -v, --version         Show version information\n";
}

void printVersion() {
    std::cout << "CppHttpServer version 1.0.0\n";
}

int main(int argc, char* argv[]) {
    int port = 8080;
    std::string docRoot = "./html_docs";
    int numThreads = 4;

    static struct option longOptions[] = {
        {"port", required_argument, 0, 'p'},
        {"doc-root", required_argument, 0, 'd'},
        {"threads", required_argument, 0, 't'},
        {"help", no_argument, 0, 'h'},
        {"version", no_argument, 0, 'v'},
        {0, 0, 0, 0}
    };

    int optionIndex = 0;
    int c;

    while ((c = getopt_long(argc, argv, "p:d:t:hv", longOptions, &optionIndex)) != -1) {
        switch (c) {
            case 'p':
                port = std::atoi(optarg);
                if (port <= 0 || port > 65535) {
                    std::cerr << "Invalid port number: " << optarg << std::endl;
                    return 1;
                }
                break;
            case 'd':
                docRoot = optarg;
                break;
            case 't':
                numThreads = std::atoi(optarg);
                if (numThreads <= 0) {
                    std::cerr << "Invalid thread number: " << optarg << std::endl;
                    return 1;
                }
                break;
            case 'h':
                printUsage(argv[0]);
                return 0;
            case 'v':
                printVersion();
                return 0;
            default:
                printUsage(argv[0]);
                return 1;
        }
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    HttpServer server("0.0.0.0", port);
    server.setDocRoot(docRoot);
    server.setNumThreads(numThreads);

    g_server = &server;

    if (!server.start()) {
        std::cerr << "Failed to start server" << std::endl;
        return 1;
    }

    std::cout << "Server is running. Press Ctrl+C to stop." << std::endl;

    while (server.isRunning()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}

#include <csignal>
#include <iostream>

#include "http_server/config.hpp"
#include "http_server/server.hpp"

namespace {
http_server::Server* g_server = nullptr;

void signalHandler(int /*signum*/) {
    if (g_server) g_server->stop();
}
}  // namespace

int main() {
    http_server::Config cfg = http_server::loadConfig();
    http_server::Server server(cfg);
    g_server = &server;

    // Graceful shutdown on Ctrl-C / SIGTERM
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    std::cout << "[INFO] Starting http-server on port " << cfg.port
              << ", static dir '" << cfg.static_dir << "'." << std::endl;

    return server.run();
}

#include <atomic>
#include <csignal>
#include <exception>
#include <iostream>

#include "http_server/config.hpp"
#include "http_server/server.hpp"

namespace {

static_assert(std::atomic<http_server::Server*>::is_always_lock_free,
              "signal-safe server pointer requires a lock-free atomic pointer");

std::atomic<http_server::Server*> g_server{nullptr};

void signalHandler(int /*signum*/) {
    http_server::Server* server =
        g_server.load(std::memory_order_relaxed);
    if (server) server->stop();
}

}  // namespace

int main() {
    // A client may close while a response is being written. sendAll() already
    // reports EPIPE/ECONNRESET; ignoring SIGPIPE keeps one peer from killing
    // the entire server. This is portable across Linux and macOS.
    std::signal(SIGPIPE, SIG_IGN);

    try {
        http_server::Config cfg = http_server::loadConfig();
        http_server::Server server(cfg);
        g_server.store(&server, std::memory_order_relaxed);

        struct SignalStateGuard {
            ~SignalStateGuard() {
                g_server.store(nullptr, std::memory_order_relaxed);
            }
        } signal_state_guard;

        // Graceful shutdown on Ctrl-C / SIGTERM
        std::signal(SIGINT, signalHandler);
        std::signal(SIGTERM, signalHandler);

        std::cout << "[INFO] Starting http-server on port " << cfg.port
                  << ", static dir '" << cfg.static_dir << "'." << std::endl;

        const int result = server.run();
        if (result != 0) {
            std::cerr << "[FATAL] Socket setup failed." << std::endl;
        }
        return result;
    } catch (const std::exception& error) {
        g_server.store(nullptr, std::memory_order_relaxed);
        std::cerr << "[FATAL] Initialization failed: " << error.what()
                  << std::endl;
        return 2;
    } catch (...) {
        g_server.store(nullptr, std::memory_order_relaxed);
        std::cerr << "[FATAL] Initialization failed with unknown error."
                  << std::endl;
        return 2;
    }
}

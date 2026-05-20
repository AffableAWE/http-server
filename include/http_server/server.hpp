#ifndef HTTP_SERVER_SERVER_HPP
#define HTTP_SERVER_SERVER_HPP

#include <atomic>

#include "http_server/config.hpp"
#include "http_server/logger.hpp"
#include "http_server/thread_pool.hpp"

namespace http_server {

/*
 * Owns the listening sockets, accept loop, thread pool, and logger.
 * Lifetime: construct -> run() (blocks) -> stop() (typically from a signal).
 */
class Server {
   public:
    explicit Server(const Config& cfg);
    ~Server();

    /*
     * Sets up sockets and enters the accept loop. Blocks until stop().
     * Returns 0 on clean shutdown, non-zero on setup failure.
     */
    int run();

    /*
     * Signals the accept loop to exit. Safe to call from a signal handler
     * (uses atomic flag only).
     */
    void stop();

   private:
    bool setupSockets();
    void acceptLoop();

    Config cfg_;
    Logger logger_;
    int sockd4_ = -1;
    int sockd6_ = -1;
    std::atomic<bool> running_;
};

}  // namespace http_server

#endif  // HTTP_SERVER_SERVER_HPP

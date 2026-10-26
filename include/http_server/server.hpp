#ifndef HTTP_SERVER_SERVER_HPP
#define HTTP_SERVER_SERVER_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "http_server/active_connections.hpp"
#include "http_server/asset_cache.hpp"
#include "http_server/config.hpp"
#include "http_server/connection.hpp"
#include "http_server/connection_limiter.hpp"
#include "http_server/logger.hpp"
#include "http_server/unique_fd.hpp"

namespace http_server {

/*
 * Owns the listening sockets, readiness-driven event loop, and logger.
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
     * Signals the accept loop to exit. Safe to call from a signal handler:
     * it stores an atomic flag and performs one async-signal-safe write to the
     * wake pipe. Repeated calls are safe.
     */
    void stop() noexcept;

   private:
    bool createWakePipe();
    bool setupSockets();
    void eventLoop();
    void acceptConnections(int listener_fd);
    void handleReadable(Connection& connection);
    void handleWritable(Connection& connection);
    void processBufferedInput(Connection& connection);
    void beginRequest(Connection& connection);
    void queueResponse(Connection& connection, int status_code,
                       std::string method, std::string path,
                       std::string response_header,
                       std::shared_ptr<const std::string> response_body,
                       bool close_after,
                       TransferOutcome outcome_override =
                           TransferOutcome::Unknown);
    void queueErrorResponse(Connection& connection, int status_code,
                            const char* status_line, const char* body,
                            TransferOutcome outcome_override =
                                TransferOutcome::Unknown);
    void completeResponse(Connection& connection);
    void failConnection(Connection& connection, TransferOutcome outcome);
    void abandonConnection(Connection& connection);
    void logResponse(const Connection& connection,
                     const PendingResponse& response,
                     TransferOutcome outcome);
    void markLastQueuedResponseClose(Connection& connection);
    void pruneFinishedConnections();
    void markIdleIfReady(Connection& connection);
    bool canRead(const Connection& connection) const;
    short pollEvents(const Connection& connection) const;
    void beginShutdownDrain();
    void forceCloseAllConnections();
    void emitDiagnostics(bool final_report);
    int pollTimeoutMilliseconds() const;

    struct Diagnostics {
        std::uint64_t accepted = 0;
        std::uint64_t admission_rejected = 0;
        std::uint64_t completed_responses = 0;
        std::uint64_t header_timeouts = 0;
        std::uint64_t write_timeouts = 0;
        std::uint64_t peer_eof = 0;
        std::uint64_t peer_resets = 0;
        std::uint64_t read_errors = 0;
        std::uint64_t write_errors = 0;
        std::uint64_t parse_errors = 0;
        std::uint64_t requested_close = 0;
        std::uint64_t max_requests_closed = 0;
        std::uint64_t idle_timeouts = 0;
        std::uint64_t shutdown_closed = 0;
        std::uint64_t poll_errors = 0;
        std::uint64_t event_loops = 0;
        std::uint64_t interval_max_loop_work_us = 0;
    };

    Config cfg_;
    ConnectionLimiter connection_limiter_;
    ActiveConnections active_connections_;
    AssetCache asset_cache_;
    std::unique_ptr<Logger> logger_;
    UniqueFd sockd4_;
    UniqueFd sockd6_;
    UniqueFd wake_read_;
    UniqueFd wake_write_;
    std::atomic<bool> running_;
    bool ipv4_enabled_ = false;
    bool ipv6_enabled_ = false;
    bool draining_ = false;
    std::optional<Deadline> shutdown_deadline_;
    std::vector<std::unique_ptr<Connection>> connections_;
    Diagnostics diagnostics_;
    std::chrono::steady_clock::time_point last_diagnostics_;
    std::uint64_t last_logger_committed_ = 0;
};

}  // namespace http_server

#endif  // HTTP_SERVER_SERVER_HPP

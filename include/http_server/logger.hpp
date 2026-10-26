#ifndef HTTP_SERVER_LOGGER_HPP
#define HTTP_SERVER_LOGGER_HPP

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "http_server/config.hpp"
#include "http_server/deadline.hpp"
#include "http_server/transfer_outcome.hpp"

namespace http_server {

struct LoggerMetrics {
    std::uint64_t offered = 0;
    std::uint64_t accepted = 0;
    std::uint64_t committed = 0;
    std::uint64_t dropped = 0;
    std::uint64_t failed = 0;
    std::uint64_t discarded = 0;
    std::uint64_t in_flight = 0;
    std::size_t queue_depth = 0;
    std::uint64_t peak_queue_depth = 0;
    std::uint64_t batches = 0;
    std::uint64_t total_flush_us = 0;
    std::uint64_t max_flush_us = 0;
};

/*
 * Bounded best-effort SQLite access logger.
 *
 * Records are copied into an owned bounded queue and flushed by one dedicated
 * writer thread that owns the SQLite connection and prepared statement.
 * Queue-full records are dropped newest-first and counted. The writer does not
 * block request workers.
 */
class Logger {
   public:
    explicit Logger(const Config& cfg);
    ~Logger();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    /*
     * Attempts to enqueue one access-log record. Never blocks on SQLite.
     * Inputs are copied; no request buffer is retained.
     */
    void logAccess(const std::string& method, const std::string& path,
                   int status_code, std::size_t bytes_sent,
                   TransferOutcome outcome, const std::string& client_ip);

    /*
     * Stops accepting records, drains queued records until the configured
     * drain deadline, discards any remainder, and joins the writer. Repeated
     * calls are safe.
     */
    void shutdown();

    LoggerMetrics metrics() const;

   private:
    struct LogRecord {
        std::string method;
        std::string path;
        std::string client_ip;
        int status_code = 0;
        std::size_t bytes_sent = 0;
        TransferOutcome outcome = TransferOutcome::Unknown;
    };

    void writerLoop();
    void initializeDatabase();
    bool processBatch(const std::vector<LogRecord>& batch);
    void closeDatabase();
    void discardQueueLocked();
    void handleWriterFailure(const char* message) noexcept;
    void reportMetrics();

    std::string db_path_;
    std::size_t queue_capacity_;
    std::size_t batch_size_;
    int flush_interval_ms_;
    int max_retries_;
    int retry_backoff_ms_;
    int shutdown_drain_ms_;

    std::thread writer_;
    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::condition_variable startup_cv_;
    std::deque<LogRecord> queue_;
    bool accepting_ = false;
    bool stopping_ = false;
    bool startup_done_ = false;
    bool startup_ok_ = false;
    std::string startup_error_;
    std::optional<Deadline> shutdown_deadline_;
    std::optional<Deadline> flush_deadline_;

    void* db_handle_ = nullptr;    // sqlite3*, owned by writer after startup
    void* insert_stmt_ = nullptr;  // sqlite3_stmt*

    std::atomic<std::uint64_t> offered_{0};
    std::atomic<std::uint64_t> accepted_{0};
    std::atomic<std::uint64_t> committed_{0};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> failed_{0};
    std::atomic<std::uint64_t> discarded_{0};
    std::atomic<std::uint64_t> in_flight_{0};
    std::atomic<std::uint64_t> peak_queue_depth_{0};
    std::atomic<std::uint64_t> batches_{0};
    std::atomic<std::uint64_t> total_flush_us_{0};
    std::atomic<std::uint64_t> max_flush_us_{0};
    std::atomic<bool> overflow_warned_{false};
    std::atomic<bool> metrics_reported_{false};

#ifdef HTTP_SERVER_TESTING
   public:
    void failNextPreparationForTesting();

   private:
    std::atomic<bool> fail_next_preparation_{false};
#endif
};

}  // namespace http_server

#endif  // HTTP_SERVER_LOGGER_HPP

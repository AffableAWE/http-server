#include "http_server/logger.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace http_server {

namespace {

constexpr std::size_t MAX_METHOD_BYTES = 32;
constexpr std::size_t MAX_PATH_BYTES = 2048;
constexpr std::size_t MAX_CLIENT_IP_BYTES = 64;
constexpr int SCHEMA_VERSION = 1;

constexpr const char* SCHEMA_SQL = R"sql(
CREATE TABLE IF NOT EXISTS access_logs (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp   TEXT    NOT NULL DEFAULT (datetime('now')),
    client_ip   TEXT    NOT NULL,
    method      TEXT    NOT NULL,
    path        TEXT    NOT NULL,
    status_code INTEGER NOT NULL,
    bytes_sent  INTEGER NOT NULL,
    transfer_outcome TEXT NOT NULL DEFAULT 'unknown'
);

CREATE INDEX IF NOT EXISTS idx_access_logs_timestamp
    ON access_logs(timestamp);

CREATE INDEX IF NOT EXISTS idx_access_logs_status
    ON access_logs(status_code);
)sql";

constexpr const char* INSERT_SQL =
    "INSERT INTO access_logs (client_ip, method, path, status_code, "
    "bytes_sent, transfer_outcome) "
    "VALUES (?, ?, ?, ?, ?, ?);";

struct DatabaseCloser {
    void operator()(sqlite3* db) const noexcept {
        if (db) sqlite3_close(db);
    }
};

struct StatementFinalizer {
    void operator()(sqlite3_stmt* stmt) const noexcept {
        if (stmt) sqlite3_finalize(stmt);
    }
};

struct SqliteMessageFree {
    void operator()(char* message) const noexcept { sqlite3_free(message); }
};

using Database = std::unique_ptr<sqlite3, DatabaseCloser>;
using Statement = std::unique_ptr<sqlite3_stmt, StatementFinalizer>;
using SqliteMessage = std::unique_ptr<char, SqliteMessageFree>;

bool retryable(int rc) {
    return rc == SQLITE_BUSY || rc == SQLITE_LOCKED;
}

std::string sqliteError(sqlite3* db, const char* fallback) {
    if (!db) return fallback;
    const char* message = sqlite3_errmsg(db);
    return message ? message : fallback;
}

void executeSqlOrThrow(sqlite3* db, const char* sql, const char* context) {
    char* raw_message = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &raw_message);
    SqliteMessage message(raw_message);
    if (rc != SQLITE_OK) {
        throw std::runtime_error(
            std::string(context) + ": " +
            (message ? message.get() : "unknown SQLite error"));
    }
}

int readUserVersion(sqlite3* db) {
    sqlite3_stmt* raw_stmt = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &raw_stmt,
                           nullptr) != SQLITE_OK) {
        throw std::runtime_error("Failed to read schema version: " +
                                 sqliteError(db, "unknown SQLite error"));
    }
    Statement stmt(raw_stmt);
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        throw std::runtime_error("Failed to read schema version row");
    }
    return sqlite3_column_int(stmt.get(), 0);
}

bool hasTransferOutcomeColumn(sqlite3* db) {
    sqlite3_stmt* raw_stmt = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA table_info(access_logs);", -1,
                           &raw_stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("Failed to inspect access_logs schema: " +
                                 sqliteError(db, "unknown SQLite error"));
    }
    Statement stmt(raw_stmt);
    int rc = SQLITE_ROW;
    while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
        const unsigned char* name = sqlite3_column_text(stmt.get(), 1);
        if (name && std::string(reinterpret_cast<const char*>(name)) ==
                        "transfer_outcome") {
            return true;
        }
    }
    if (rc != SQLITE_DONE) {
        throw std::runtime_error("Failed to inspect access_logs schema: " +
                                 sqliteError(db, "unknown SQLite error"));
    }
    return false;
}

}  // namespace

Logger::Logger(const Config& cfg)
    : db_path_(cfg.db_path),
      queue_capacity_(static_cast<std::size_t>(cfg.log_queue_capacity)),
      batch_size_(static_cast<std::size_t>(cfg.log_batch_size)),
      flush_interval_ms_(cfg.log_flush_interval_ms),
      max_retries_(cfg.log_max_retries),
      retry_backoff_ms_(cfg.log_retry_backoff_ms),
      shutdown_drain_ms_(cfg.log_shutdown_drain_ms) {
    if (queue_capacity_ == 0) queue_capacity_ = 1;
    if (batch_size_ == 0) batch_size_ = 1;

    writer_ = std::thread([this] { writerLoop(); });

    std::unique_lock<std::mutex> lock(queue_mutex_);
    startup_cv_.wait(lock, [this] { return startup_done_; });
    if (!startup_ok_) {
        const std::string error = startup_error_;
        lock.unlock();
        if (writer_.joinable()) writer_.join();
        throw std::runtime_error(error.empty() ? "Logger startup failed" : error);
    }
}

Logger::~Logger() { shutdown(); }

void Logger::initializeDatabase() {
    sqlite3* raw_db = nullptr;
    const int open_rc = sqlite3_open(db_path_.c_str(), &raw_db);
    Database db(raw_db);
    if (open_rc != SQLITE_OK) {
        throw std::runtime_error(
            "Failed to open SQLite DB '" + db_path_ + "': " +
            (raw_db ? sqlite3_errmsg(raw_db) : "unknown SQLite error"));
    }

    const int schema_version = readUserVersion(db.get());
    if (schema_version > SCHEMA_VERSION) {
        throw std::runtime_error(
            "Unsupported access_logs schema version " +
            std::to_string(schema_version) + " (server supports " +
            std::to_string(SCHEMA_VERSION) + ")");
    }

    executeSqlOrThrow(db.get(),
                      "PRAGMA journal_mode=WAL;"
                      "PRAGMA synchronous=NORMAL;",
                      "Failed to set pragmas");

    bool in_transaction = false;
    Statement stmt;
    try {
        executeSqlOrThrow(db.get(), "BEGIN IMMEDIATE;",
                          "Failed to begin schema migration");

        in_transaction = true;
        executeSqlOrThrow(db.get(), SCHEMA_SQL, "Failed to apply schema");

        if (!hasTransferOutcomeColumn(db.get())) {
            executeSqlOrThrow(
                db.get(),
                "ALTER TABLE access_logs ADD COLUMN transfer_outcome "
                "TEXT NOT NULL DEFAULT 'unknown';",
                "Failed to migrate access_logs schema");
        }
        if (!hasTransferOutcomeColumn(db.get())) {
            throw std::runtime_error(
                "Schema validation failed: transfer_outcome is missing");
        }

        sqlite3_stmt* raw_stmt = nullptr;
        const int prepare_rc =
            sqlite3_prepare_v2(db.get(), INSERT_SQL, -1, &raw_stmt, nullptr);
        stmt.reset(raw_stmt);
        if (prepare_rc != SQLITE_OK) {
            throw std::runtime_error(
                "Failed to prepare insert: " +
                sqliteError(db.get(), "unknown SQLite error"));
        }

        executeSqlOrThrow(db.get(), "PRAGMA user_version = 1;",
                          "Failed to update schema version");
        executeSqlOrThrow(db.get(), "COMMIT;", "Failed to commit schema migration");
        in_transaction = false;
    } catch (...) {
        if (in_transaction) {
            sqlite3_exec(db.get(), "ROLLBACK;", nullptr, nullptr, nullptr);
        }
        throw;
    }

    db_handle_ = db.release();
    insert_stmt_ = stmt.release();

    std::cout << "[INFO] Logger writer initialized (db=" << db_path_ << ")"
              << std::endl;
}

void Logger::writerLoop() {
    try {
        initializeDatabase();

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            accepting_ = true;
            startup_ok_ = true;
            startup_done_ = true;
        }
        startup_cv_.notify_all();

        while (true) {
            std::vector<LogRecord> batch;
            bool exit_writer = false;

            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                while (true) {
#ifdef HTTP_SERVER_TESTING
                    if (fail_next_preparation_.exchange(false)) {
                        lock.unlock();
                        throw std::runtime_error(
                            "injected batch preparation failure");
                    }
#endif
                    if (stopping_ && queue_.empty()) {
                        exit_writer = true;
                        break;
                    }
                    if (stopping_ && shutdown_deadline_ &&
                        shutdown_deadline_->expired()) {
                        discardQueueLocked();
                        exit_writer = true;
                        break;
                    }
                    if (!queue_.empty() &&
                        (stopping_ || queue_.size() >= batch_size_)) {
                        break;
                    }
                    if (queue_.empty()) {
                        queue_cv_.wait(lock);
                        continue;
                    }

                    int wait_ms = flush_interval_ms_;
                    if (flush_deadline_) {
                        wait_ms = flush_deadline_->remainingMilliseconds();
                    }
                    if (wait_ms <= 0) {
                        // The absolute flush deadline for the oldest pending
                        // record has expired.
                        break;
                    }
                    if (stopping_ && shutdown_deadline_) {
                        const int remaining =
                            shutdown_deadline_->remainingMilliseconds();
                        if (remaining <= 0) continue;
                        wait_ms = std::min(wait_ms, remaining);
                    }
                    if (wait_ms <= 0) continue;
                    queue_cv_.wait_for(
                        lock, std::chrono::milliseconds(wait_ms));
                    // Recheck all predicates after a notification or spurious
                    // wakeup. New arrivals do not restart the deadline.
                }

                if (exit_writer) break;

                const std::size_t count =
                    std::min(batch_size_, queue_.size());
                batch.reserve(count);
                for (std::size_t i = 0; i < count; ++i) {
                    batch.push_back(std::move(queue_.front()));
                    queue_.pop_front();
                }
                in_flight_.store(count, std::memory_order_relaxed);
                if (queue_.empty()) flush_deadline_.reset();
            }

            try {
                processBatch(batch);
            } catch (const std::exception& error) {
                if (db_handle_) {
                    sqlite3_exec(static_cast<sqlite3*>(db_handle_),
                                 "ROLLBACK;", nullptr, nullptr, nullptr);
                }
                failed_.fetch_add(batch.size(), std::memory_order_relaxed);
                std::cerr << "[ERROR] Logging batch failed: " << error.what()
                          << std::endl;
            } catch (...) {
                if (db_handle_) {
                    sqlite3_exec(static_cast<sqlite3*>(db_handle_),
                                 "ROLLBACK;", nullptr, nullptr, nullptr);
                }
                failed_.fetch_add(batch.size(), std::memory_order_relaxed);
                std::cerr << "[ERROR] Logging batch failed with unknown error"
                          << std::endl;
            }
            in_flight_.store(0, std::memory_order_relaxed);
        }

        closeDatabase();
    } catch (const std::exception& error) {
        handleWriterFailure(error.what());
    } catch (...) {
        handleWriterFailure("unknown logger writer failure");
    }
}

void Logger::handleWriterFailure(const char* message) noexcept {
    bool startup_failure = false;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (!startup_done_) {
            startup_failure = true;
            try {
                startup_error_ = message ? message : "logger writer failure";
            } catch (...) {
                startup_error_ = "logger writer failure";
            }
            startup_ok_ = false;
            startup_done_ = true;
        } else {
            accepting_ = false;
            stopping_ = true;
            discarded_.fetch_add(queue_.size(), std::memory_order_relaxed);
            queue_.clear();
            flush_deadline_.reset();
            in_flight_.store(0, std::memory_order_relaxed);
        }
    }

    closeDatabase();
    if (startup_failure) {
        startup_cv_.notify_all();
    } else {
        std::cerr << "[ERROR] Logger writer stopped; access logging is now "
                     "disabled: "
                  << (message ? message : "unknown logger writer failure")
                  << std::endl;
        queue_cv_.notify_all();
    }
}

bool Logger::processBatch(const std::vector<LogRecord>& batch) {
    if (batch.empty()) return true;

    const auto started = std::chrono::steady_clock::now();
    auto finish_latency = [&]() {
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started);
        const std::uint64_t micros = static_cast<std::uint64_t>(
            std::max<std::int64_t>(0, elapsed.count()));
        batches_.fetch_add(1, std::memory_order_relaxed);
        total_flush_us_.fetch_add(micros, std::memory_order_relaxed);
        std::uint64_t previous =
            max_flush_us_.load(std::memory_order_relaxed);
        while (micros > previous &&
               !max_flush_us_.compare_exchange_weak(
                   previous, micros, std::memory_order_relaxed)) {
        }
    };

    auto* db = static_cast<sqlite3*>(db_handle_);
    auto* stmt = static_cast<sqlite3_stmt*>(insert_stmt_);

    for (int attempt = 0; attempt <= max_retries_; ++attempt) {
        char* raw_message = nullptr;
        int rc = sqlite3_exec(db, "BEGIN IMMEDIATE;", nullptr, nullptr,
                              &raw_message);
        SqliteMessage message(raw_message);
        if (rc != SQLITE_OK) {
            if (attempt < max_retries_ && retryable(rc)) {
                if (retry_backoff_ms_ > 0) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(retry_backoff_ms_));
                }
                continue;
            }
            failed_.fetch_add(batch.size(), std::memory_order_relaxed);
            finish_latency();
            std::cerr << "[ERROR] Logging BEGIN failed: "
                      << (message ? message.get() : "unknown SQLite error")
                      << std::endl;
            return false;
        }

        bool batch_ok = true;
        std::string insert_error;
        for (const LogRecord& record : batch) {
            const bool bound =
                sqlite3_bind_text(stmt, 1, record.client_ip.c_str(), -1,
                                  SQLITE_TRANSIENT) == SQLITE_OK &&
                sqlite3_bind_text(stmt, 2, record.method.c_str(), -1,
                                  SQLITE_TRANSIENT) == SQLITE_OK &&
                sqlite3_bind_text(stmt, 3, record.path.c_str(), -1,
                                  SQLITE_TRANSIENT) == SQLITE_OK &&
                sqlite3_bind_int(stmt, 4, record.status_code) == SQLITE_OK &&
                sqlite3_bind_int64(
                    stmt, 5,
                    static_cast<sqlite3_int64>(record.bytes_sent)) == SQLITE_OK &&
                sqlite3_bind_text(stmt, 6,
                                  transferOutcomeName(record.outcome), -1,
                                  SQLITE_STATIC) == SQLITE_OK;
            if (!bound) {
                insert_error = sqliteError(db, "bind failed");
                batch_ok = false;
                break;
            }

            rc = sqlite3_step(stmt);
            if (rc != SQLITE_DONE) {
                insert_error = sqliteError(db, "step failed");
                batch_ok = false;
                break;
            }
            if (sqlite3_reset(stmt) != SQLITE_OK ||
                sqlite3_clear_bindings(stmt) != SQLITE_OK) {
                insert_error = sqliteError(db, "reset failed");
                batch_ok = false;
                break;
            }
        }
        if (!batch_ok) {
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
        }

        if (batch_ok) {
            rc = sqlite3_exec(db, "COMMIT;", nullptr, nullptr, &raw_message);
            message.reset(raw_message);
            if (rc == SQLITE_OK) {
                committed_.fetch_add(batch.size(), std::memory_order_relaxed);
                finish_latency();
                return true;
            }

            // A commit that is BUSY/LOCKED did not commit. Roll back and retry.
            // Any other commit error has ambiguous durability: do not retry,
            // because that could double-insert an already committed batch.
            const bool can_retry = retryable(rc) && attempt < max_retries_;
            char* rollback_message = nullptr;
            const int rollback_rc = sqlite3_exec(
                db, "ROLLBACK;", nullptr, nullptr, &rollback_message);
            SqliteMessage rollback(rollback_message);
            if (can_retry && rollback_rc == SQLITE_OK) {
                if (retry_backoff_ms_ > 0) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(retry_backoff_ms_));
                }
                continue;
            }
            failed_.fetch_add(batch.size(), std::memory_order_relaxed);
            finish_latency();
            std::cerr << "[ERROR] Logging COMMIT failed: "
                      << (message ? message.get() : "unknown SQLite error")
                      << "; rollback="
                      << (rollback ? rollback.get() : "ok") << std::endl;
            return false;
        }

        char* rollback_message = nullptr;
        const int rollback_rc = sqlite3_exec(
            db, "ROLLBACK;", nullptr, nullptr, &rollback_message);
        SqliteMessage rollback(rollback_message);
        if (attempt < max_retries_ && rollback_rc == SQLITE_OK) {
            if (retry_backoff_ms_ > 0) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(retry_backoff_ms_));
            }
            continue;
        }
        failed_.fetch_add(batch.size(), std::memory_order_relaxed);
        finish_latency();
        std::cerr << "[ERROR] Logging insert failed: " << insert_error
                  << "; rollback=" << (rollback ? rollback.get() : "ok")
                  << std::endl;
        return false;
    }

    failed_.fetch_add(batch.size(), std::memory_order_relaxed);
    finish_latency();
    return false;
}

void Logger::logAccess(const std::string& method, const std::string& path,
                       int status_code, std::size_t bytes_sent,
                       TransferOutcome outcome, const std::string& client_ip) {
    offered_.fetch_add(1, std::memory_order_relaxed);

    if (method.size() > MAX_METHOD_BYTES || path.size() > MAX_PATH_BYTES ||
        client_ip.size() > MAX_CLIENT_IP_BYTES) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    try {
        LogRecord record;
        record.method = method;
        record.path = path;
        record.client_ip = client_ip;
        record.status_code = status_code;
        record.bytes_sent = bytes_sent;
        record.outcome = outcome;

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (!accepting_ || stopping_) {
                dropped_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (queue_.size() >= queue_capacity_) {
                dropped_.fetch_add(1, std::memory_order_relaxed);
                if (!overflow_warned_.exchange(true)) {
                    std::cerr << "[WARN] Logging queue full; dropping newest "
                                 "records (best-effort logging)"
                              << std::endl;
                }
                return;
            }
            const bool was_empty = queue_.empty();
            queue_.push_back(std::move(record));
            accepted_.fetch_add(1, std::memory_order_relaxed);
            if (was_empty) {
                flush_deadline_ = Deadline::after(
                    std::chrono::milliseconds(flush_interval_ms_));
            }
            const std::uint64_t depth =
                static_cast<std::uint64_t>(queue_.size());
            std::uint64_t previous =
                peak_queue_depth_.load(std::memory_order_relaxed);
            while (depth > previous &&
                   !peak_queue_depth_.compare_exchange_weak(
                       previous, depth, std::memory_order_relaxed)) {
            }
        }
        queue_cv_.notify_one();
    } catch (...) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
    }
}

void Logger::shutdown() {
    if (writer_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (!stopping_) {
                stopping_ = true;
                accepting_ = false;
                shutdown_deadline_ = Deadline::after(
                    std::chrono::milliseconds(shutdown_drain_ms_));
            }
        }
        queue_cv_.notify_all();
        writer_.join();
    }

    if (!metrics_reported_.exchange(true)) {
        reportMetrics();
    }
}

#ifdef HTTP_SERVER_TESTING
void Logger::failNextPreparationForTesting() {
    fail_next_preparation_.store(true, std::memory_order_relaxed);
}
#endif

void Logger::discardQueueLocked() {
    discarded_.fetch_add(queue_.size(), std::memory_order_relaxed);
    queue_.clear();
    flush_deadline_.reset();
}

void Logger::closeDatabase() {
    if (insert_stmt_) {
        sqlite3_finalize(static_cast<sqlite3_stmt*>(insert_stmt_));
        insert_stmt_ = nullptr;
    }
    if (db_handle_) {
        sqlite3_close(static_cast<sqlite3*>(db_handle_));
        db_handle_ = nullptr;
    }
}

LoggerMetrics Logger::metrics() const {
    LoggerMetrics result;
    result.offered = offered_.load(std::memory_order_relaxed);
    result.accepted = accepted_.load(std::memory_order_relaxed);
    result.committed = committed_.load(std::memory_order_relaxed);
    result.dropped = dropped_.load(std::memory_order_relaxed);
    result.failed = failed_.load(std::memory_order_relaxed);
    result.discarded = discarded_.load(std::memory_order_relaxed);
    result.in_flight = in_flight_.load(std::memory_order_relaxed);
    result.peak_queue_depth =
        peak_queue_depth_.load(std::memory_order_relaxed);
    result.batches = batches_.load(std::memory_order_relaxed);
    result.total_flush_us = total_flush_us_.load(std::memory_order_relaxed);
    result.max_flush_us = max_flush_us_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        result.queue_depth = queue_.size();
    }
    return result;
}

void Logger::reportMetrics() {
    const LoggerMetrics snapshot = metrics();
    const double average_flush_ms =
        snapshot.batches == 0
            ? 0.0
            : static_cast<double>(snapshot.total_flush_us) /
                  static_cast<double>(snapshot.batches) / 1000.0;

    std::cout << "[INFO] Logger stopped offered=" << snapshot.offered
              << " accepted=" << snapshot.accepted
              << " committed=" << snapshot.committed
              << " failed=" << snapshot.failed
              << " dropped=" << snapshot.dropped
              << " discarded=" << snapshot.discarded
              << " queued=" << snapshot.queue_depth
              << " peak_queue=" << snapshot.peak_queue_depth
              << " in_flight=" << snapshot.in_flight
              << " batches=" << snapshot.batches
              << " avg_flush_ms=" << average_flush_ms
              << " max_flush_ms="
              << static_cast<double>(snapshot.max_flush_us) / 1000.0
              << std::endl;
}

}  // namespace http_server

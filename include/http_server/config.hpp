#ifndef HTTP_SERVER_CONFIG_HPP
#define HTTP_SERVER_CONFIG_HPP

#include <string>

namespace http_server {

/*
 * Holds server configuration loaded from environment variables.
 * Provides sensible defaults when env vars are missing or invalid.
 */
struct Config {
    std::string static_dir = "static";
    int port = 8080;
    std::string db_path = "data/logs.db";
    bool enable_logging = true;
    // Maximum admitted event-loop connections.
    int max_connections = 1024;
    // Legacy worker-queue setting retained for environment compatibility.
    int max_queued_connections = 256;
    // Idle keep-alive timeout before a persistent connection is closed.
    int keepalive_timeout_ms = 5000;
    // Maximum requests accepted on one connection.
    int max_requests_per_connection = 1000;
    // Maximum pending responses/requests per connection.
    int max_pipelined_requests = 8;
    // Maximum buffered bytes retained for one connection's request stream.
    int max_connection_input_bytes = 128 * 1024;
    // Legacy worker-count setting retained for environment compatibility.
    int worker_threads = 0;
    // Deprecated. Retained for environment compatibility; no longer used.
    int recv_timeout_ms = 5000;
    // Deprecated. Retained for environment compatibility; no longer used.
    int send_timeout_ms = 5000;
    // Total time allowed to receive one request's headers, in milliseconds.
    int header_timeout_ms = 10000;
    // Total time allowed to write one response, in milliseconds.
    int write_timeout_ms = 10000;
    // One global grace period for already-active socket work at shutdown.
    int shutdown_grace_ms = 5000;
    // Maximum size of one preloaded static asset, in bytes.
    int max_asset_bytes = 16 * 1024 * 1024;
    // Maximum total bytes of unique preloaded asset bodies.
    int max_asset_cache_bytes = 64 * 1024 * 1024;
    // Bounded asynchronous log queue capacity.
    int log_queue_capacity = 8192;
    // Maximum records per transaction.
    int log_batch_size = 256;
    // Maximum wait to accumulate a low-traffic batch, in milliseconds.
    int log_flush_interval_ms = 10;
    // Bounded retries for a failed batch.
    int log_max_retries = 2;
    // Delay between batch retries, in milliseconds.
    int log_retry_backoff_ms = 10;
    // Maximum time to drain queued records during shutdown, in milliseconds.
    int log_shutdown_drain_ms = 2000;
    // Periodic aggregate diagnostics interval; 0 disables diagnostics.
    int diagnostics_interval_ms = 0;
};

/*
 * Reads STATIC_DIR, PORT, DB_PATH, ENABLE_LOGGING, MAX_CONNECTIONS,
 * MAX_QUEUED_CONNECTIONS, WORKER_THREADS, KEEPALIVE_TIMEOUT_MS,
 * MAX_REQUESTS_PER_CONNECTION, MAX_PIPELINED_REQUESTS,
 * MAX_CONNECTION_INPUT_BYTES, DIAGNOSTICS_INTERVAL_MS, HEADER_TIMEOUT_MS,
 * WRITE_TIMEOUT_MS,
 * SHUTDOWN_GRACE_MS, MAX_ASSET_BYTES,
 * MAX_ASSET_CACHE_BYTES, LOG_QUEUE_CAPACITY, LOG_BATCH_SIZE,
 * LOG_FLUSH_INTERVAL_MS, LOG_MAX_RETRIES, LOG_RETRY_BACKOFF_MS, and
 * LOG_SHUTDOWN_DRAIN_MS from the environment. Falls back to defaults for missing
 * or invalid values. MAX_QUEUED_CONNECTIONS, WORKER_THREADS, RECV_TIMEOUT_MS,
 * and SEND_TIMEOUT_MS are accepted only for compatibility and no longer
 * enforce request behavior.
 * Input:  none
 * Returns: A populated Config struct.
 */
Config loadConfig();

}  // namespace http_server

#endif  // HTTP_SERVER_CONFIG_HPP

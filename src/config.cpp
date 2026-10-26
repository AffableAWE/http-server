#include "http_server/config.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace http_server {

namespace {

int loadBoundedInt(const char* name, int fallback, int minimum, int maximum) {
    const char* raw = std::getenv(name);
    if (!raw) return fallback;

    char* end = nullptr;
    const long parsed = std::strtol(raw, &end, 10);
    if (*raw == '\0' || *end != '\0' || parsed < minimum ||
        parsed > maximum) {
        std::cerr << "[WARNING] Invalid " << name << " '" << raw
                  << "'. Using " << fallback << "." << std::endl;
        return fallback;
    }
    return static_cast<int>(parsed);
}

}  // namespace

Config loadConfig() {
    Config cfg;

    const char* env_dir = std::getenv("STATIC_DIR");
    if (env_dir) cfg.static_dir = env_dir;

    const char* env_port = std::getenv("PORT");
    if (env_port) {
        char* end;
        long parsed = std::strtol(env_port, &end, 10);
        if (*end == '\0' && parsed > 0 && parsed <= 65535) {
            cfg.port = static_cast<int>(parsed);
        } else {
            std::cerr << "[WARNING] Invalid PORT '" << env_port << "'. Using "
                      << cfg.port << "." << std::endl;
        }
    }

    const char* env_db = std::getenv("DB_PATH");
    if (env_db) cfg.db_path = env_db;

    const char* env_log = std::getenv("ENABLE_LOGGING");
    if (env_log) {
        std::string val(env_log);
        if (val == "0" || val == "false" || val == "no") {
            cfg.enable_logging = false;
        }
    }

    cfg.max_connections =
        loadBoundedInt("MAX_CONNECTIONS", cfg.max_connections, 1, 65535);
    cfg.max_queued_connections = loadBoundedInt(
        "MAX_QUEUED_CONNECTIONS", cfg.max_queued_connections, 1, 65535);
    cfg.worker_threads =
        loadBoundedInt("WORKER_THREADS", cfg.worker_threads, 0, 1024);
    if (cfg.max_queued_connections > cfg.max_connections) {
        std::cerr << "[WARNING] MAX_QUEUED_CONNECTIONS is greater than "
                     "MAX_CONNECTIONS. Using "
                  << cfg.max_connections << "." << std::endl;
        cfg.max_queued_connections = cfg.max_connections;
    }

    cfg.recv_timeout_ms =
        loadBoundedInt("RECV_TIMEOUT_MS", cfg.recv_timeout_ms, 1, 600000);
    cfg.send_timeout_ms =
        loadBoundedInt("SEND_TIMEOUT_MS", cfg.send_timeout_ms, 1, 600000);
    cfg.header_timeout_ms =
        loadBoundedInt("HEADER_TIMEOUT_MS", cfg.header_timeout_ms, 1, 600000);
    cfg.write_timeout_ms =
        loadBoundedInt("WRITE_TIMEOUT_MS", cfg.write_timeout_ms, 1, 600000);
    cfg.shutdown_grace_ms = loadBoundedInt(
        "SHUTDOWN_GRACE_MS", cfg.shutdown_grace_ms, 1, 600000);
    cfg.max_asset_bytes = loadBoundedInt(
        "MAX_ASSET_BYTES", cfg.max_asset_bytes, 1, 1024 * 1024 * 1024);
    cfg.max_asset_cache_bytes = loadBoundedInt(
        "MAX_ASSET_CACHE_BYTES", cfg.max_asset_cache_bytes, 1,
        1024 * 1024 * 1024);
    cfg.keepalive_timeout_ms = loadBoundedInt(
        "KEEPALIVE_TIMEOUT_MS", cfg.keepalive_timeout_ms, 1, 600000);
    cfg.max_requests_per_connection = loadBoundedInt(
        "MAX_REQUESTS_PER_CONNECTION", cfg.max_requests_per_connection, 1,
        1000000);
    cfg.max_pipelined_requests = loadBoundedInt(
        "MAX_PIPELINED_REQUESTS", cfg.max_pipelined_requests, 1, 1024);
    cfg.max_connection_input_bytes = loadBoundedInt(
        "MAX_CONNECTION_INPUT_BYTES", cfg.max_connection_input_bytes, 4096,
        16 * 1024 * 1024);
    cfg.log_queue_capacity = loadBoundedInt(
        "LOG_QUEUE_CAPACITY", cfg.log_queue_capacity, 1, 1000000);
    cfg.log_batch_size =
        loadBoundedInt("LOG_BATCH_SIZE", cfg.log_batch_size, 1, 10000);
    cfg.log_flush_interval_ms = loadBoundedInt(
        "LOG_FLUSH_INTERVAL_MS", cfg.log_flush_interval_ms, 1, 60000);
    cfg.log_max_retries =
        loadBoundedInt("LOG_MAX_RETRIES", cfg.log_max_retries, 0, 100);
    cfg.log_retry_backoff_ms = loadBoundedInt(
        "LOG_RETRY_BACKOFF_MS", cfg.log_retry_backoff_ms, 0, 10000);
    cfg.log_shutdown_drain_ms = loadBoundedInt(
        "LOG_SHUTDOWN_DRAIN_MS", cfg.log_shutdown_drain_ms, 1, 600000);
    cfg.diagnostics_interval_ms = loadBoundedInt(
        "DIAGNOSTICS_INTERVAL_MS", cfg.diagnostics_interval_ms, 0, 600000);

    return cfg;
}

}  // namespace http_server

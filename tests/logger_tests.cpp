#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

#include "http_server/logger.hpp"

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::string test_database_path() {
    return "/tmp/http_server_logger_test_" + std::to_string(::getpid()) +
           ".db";
}

void remove_database_files(const std::string& path) {
    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());
}

void test_preparation_failure_is_contained() {
    const std::string path = test_database_path();
    remove_database_files(path);

    http_server::Config cfg;
    cfg.db_path = path;
    cfg.log_queue_capacity = 8;
    cfg.log_batch_size = 100;
    cfg.log_flush_interval_ms = 1000;
    cfg.log_max_retries = 0;
    cfg.log_shutdown_drain_ms = 100;

    {
        http_server::Logger logger(cfg);
        logger.failNextPreparationForTesting();
        logger.logAccess("GET", "/failure", 200, 12,
                         http_server::TransferOutcome::Complete, "127.0.0.1");
        logger.shutdown();

        const http_server::LoggerMetrics stopped = logger.metrics();
        check(stopped.accepted == 1, "injected record was not enqueued");
        check(stopped.discarded == 1,
              "queued record was not accounted as discarded");
        check(stopped.committed == 0, "failure path committed a record");
        check(stopped.failed == 0, "preparation failure was counted as failed");
        check(stopped.queue_depth == 0, "queue was not cleared after writer failure");
        check(stopped.in_flight == 0, "in-flight count was not reset");

        logger.logAccess("GET", "/after-failure", 200, 0,
                         http_server::TransferOutcome::Failed, "127.0.0.1");
        const http_server::LoggerMetrics after = logger.metrics();
        check(after.offered == 2, "post-failure offer was not counted");
        check(after.dropped == 1, "post-failure record was not dropped");
        check(after.accepted == 1,
              "post-failure record was incorrectly accepted");
    }

    remove_database_files(path);
}

void test_startup_failure_wakes_constructor() {
    http_server::Config cfg;
    cfg.db_path = "/definitely/missing/http-server-parent/logs.db";
    cfg.log_queue_capacity = 8;
    cfg.log_batch_size = 1;
    cfg.log_flush_interval_ms = 10;
    cfg.log_shutdown_drain_ms = 10;

    bool threw = false;
    try {
        http_server::Logger logger(cfg);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    check(threw, "startup failure did not wake and fail the constructor");
}

}  // namespace

int main() {
    test_preparation_failure_is_contained();
    test_startup_failure_wakes_constructor();

    if (failures != 0) {
        std::cerr << failures << " logger test(s) failed\n";
        return 1;
    }
    std::cout << "all logger tests passed\n";
    return 0;
}

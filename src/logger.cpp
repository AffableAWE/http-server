#include "http_server/logger.hpp"

#include <iostream>

namespace http_server {

Logger::Logger(const std::string& db_path) : db_path_(db_path) {
    // TODO(sqlite): open db, run init_db.sql, prepare insert statement.
    std::cout << "[INFO] Logger initialized (stub, db_path=" << db_path_ << ")"
              << std::endl;
}

Logger::~Logger() {
    // TODO(sqlite): finalize statement, close db.
}

void Logger::logAccess(const std::string& method, const std::string& path,
                       int status_code, size_t bytes_sent,
                       const std::string& client_ip) {
    std::lock_guard<std::mutex> lock(mtx_);
    // Stub: print one line per access. SQLite INSERT replaces this later.
    std::cout << "[ACCESS] " << client_ip << " " << method << " " << path << " "
              << status_code << " " << bytes_sent << "b" << std::endl;
}

}  // namespace http_server

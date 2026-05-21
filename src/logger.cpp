#include "http_server/logger.hpp"

#include <sqlite3.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace http_server {

namespace {
// Path is relative to where the binary is launched from, but the
// schema is part of the repo so we ship it alongside the code.
constexpr const char* SCHEMA_PATH = "scripts/init_db.sql";

constexpr const char* INSERT_SQL =
    "INSERT INTO access_logs (client_ip, method, path, status_code, "
    "bytes_sent) "
    "VALUES (?, ?, ?, ?, ?);";
}  // namespace

Logger::Logger(const std::string& db_path) : db_path_(db_path) {
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
        std::string err = sqlite3_errmsg(db);
        sqlite3_close(db);
        throw std::runtime_error("Failed to open SQLite DB '" + db_path +
                                 "': " + err);
    }
    db_handle_ = db;

    // Performance: WAL allows readers and writers to coexist; NORMAL sync
    // is durable enough for access logs (we don't need fsync per insert).
    char* errmsg = nullptr;
    const char* pragmas =
        "PRAGMA journal_mode=WAL;"
        "PRAGMA synchronous=NORMAL;";
    if (sqlite3_exec(db, pragmas, nullptr, nullptr, &errmsg) != SQLITE_OK) {
        std::string err = errmsg ? errmsg : "unknown error";
        sqlite3_free(errmsg);
        sqlite3_close(db);
        db_handle_ = nullptr;
        throw std::runtime_error("Failed to set pragmas: " + err);
    }

    // Run the schema file. Safe to run on every startup (IF NOT EXISTS).
    std::ifstream f(SCHEMA_PATH);
    if (!f.is_open()) {
        sqlite3_close(db);
        db_handle_ = nullptr;
        throw std::runtime_error(std::string("Cannot open schema file '") +
                                 SCHEMA_PATH +
                                 "'. Run the server from the repo root, or set "
                                 "the working directory accordingly.");
    }
    std::stringstream ss;
    ss << f.rdbuf();
    std::string schema = ss.str();

    if (sqlite3_exec(db, schema.c_str(), nullptr, nullptr, &errmsg) !=
        SQLITE_OK) {
        std::string err = errmsg ? errmsg : "unknown error";
        sqlite3_free(errmsg);
        sqlite3_close(db);
        db_handle_ = nullptr;
        throw std::runtime_error("Failed to apply schema: " + err);
    }

    // Prepare the INSERT once and reuse it for every log entry.
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, INSERT_SQL, -1, &stmt, nullptr) != SQLITE_OK) {
        std::string err = sqlite3_errmsg(db);
        sqlite3_close(db);
        db_handle_ = nullptr;
        throw std::runtime_error("Failed to prepare insert: " + err);
    }
    insert_stmt_ = stmt;

    std::cout << "[INFO] Logger initialized (db=" << db_path_ << ")"
              << std::endl;
}

Logger::~Logger() {
    if (insert_stmt_) {
        sqlite3_finalize(static_cast<sqlite3_stmt*>(insert_stmt_));
    }
    if (db_handle_) {
        sqlite3_close(static_cast<sqlite3*>(db_handle_));
    }
}

void Logger::logAccess(const std::string& method, const std::string& path,
                       int status_code, size_t bytes_sent,
                       const std::string& client_ip) {
    std::lock_guard<std::mutex> lock(mtx_);

    auto* stmt = static_cast<sqlite3_stmt*>(insert_stmt_);
    if (!stmt) return;

    // Bind parameters (1-indexed in SQLite's API).
    sqlite3_bind_text(stmt, 1, client_ip.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, method.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, status_code);
    sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(bytes_sent));

    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        std::cerr << "[ERROR] logAccess insert failed: "
                  << sqlite3_errmsg(static_cast<sqlite3*>(db_handle_))
                  << std::endl;
    }
    // Reset to reuse the prepared statement for the next call.
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
}

}  // namespace http_server

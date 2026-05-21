#ifndef HTTP_SERVER_LOGGER_HPP
#define HTTP_SERVER_LOGGER_HPP

#include <mutex>
#include <string>

namespace http_server {

/*
 * Writes structured access-log entries to a SQLite database.
 * Thread-safe: an internal mutex serializes writes across worker threads.
 */
class Logger {
   public:
    /*
     * Opens (or creates) the database at db_path and ensures the schema exists.
     * Input:  db_path - Filesystem path to the SQLite database file.
     */
    explicit Logger(const std::string& db_path);
    ~Logger();

    // Non-copyable (owns a DB handle)
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    /*
     * Records one access-log entry.
     * Input:  method        - HTTP method (e.g. "GET")
     *         path          - Request path
     *         status_code   - HTTP status code returned
     *         bytes_sent    - Number of body bytes sent
     *         client_ip     - Remote address as a string
     */
    void logAccess(const std::string& method, const std::string& path,
                   int status_code, size_t bytes_sent,
                   const std::string& client_ip);

   private:
    std::mutex mtx_;
    std::string db_path_;
    void* db_handle_ = nullptr;    // sqlite3*
    void* insert_stmt_ = nullptr;  // sqlite3_stmt*
};

}  // namespace http_server

#endif  // HTTP_SERVER_LOGGER_HPP

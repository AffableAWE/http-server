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
};

/*
 * Reads STATIC_DIR, PORT, and DB_PATH from the environment.
 * Falls back to defaults for missing or invalid values.
 * Input:  none
 * Returns: A populated Config struct.
 */
Config loadConfig();

}  // namespace http_server

#endif  // HTTP_SERVER_CONFIG_HPP

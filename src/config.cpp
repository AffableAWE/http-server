#include "http_server/config.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace http_server {

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

    return cfg;
}

}  // namespace http_server

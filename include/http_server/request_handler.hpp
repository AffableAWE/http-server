#ifndef HTTP_SERVER_REQUEST_HANDLER_HPP
#define HTTP_SERVER_REQUEST_HANDLER_HPP

#include <string>

#include "http_server/asset_cache.hpp"
#include "http_server/config.hpp"
#include "http_server/connection_task.hpp"

namespace http_server {

class Logger;

/*
 * Handles one admitted connection. The task retains descriptor and admission
 * ownership for the duration of the call; the caller must keep it alive until
 * after the connection has been removed from any cancellation registry.
 * Input:  task      - Socket, admission permit, and acceptance-time deadline.
 *         cfg       - Static directory, logging, and timeout settings.
 *         cache     - Immutable preloaded assets and fallback response.
 *         client_ip - Remote IP, used for access logging.
 *         logger    - Optional logger; null when logging is disabled.
 */
void handleRequest(ConnectionTask& task, const Config& cfg,
                   const AssetCache& cache, const std::string& client_ip,
                   Logger* logger);

/*
 * Builds a full HTTP/1.1 response string with headers and body.
 * extra_headers is inserted verbatim before Connection; it must contain
 * complete CRLF-terminated header lines or be empty.
 */
std::string constructResponse(const std::string& status,
                              const std::string& contentType,
                              const std::string& body,
                              const std::string& extra_headers = "");

}  // namespace http_server

#endif  // HTTP_SERVER_REQUEST_HANDLER_HPP

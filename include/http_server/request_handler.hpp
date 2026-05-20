#ifndef HTTP_SERVER_REQUEST_HANDLER_HPP
#define HTTP_SERVER_REQUEST_HANDLER_HPP

#include <string>

namespace http_server {

class Logger;

/*
 * Reads one HTTP request from client_fd, serves the routed static file
 * (or an error response), logs the access, and closes the connection.
 * Input:  client_fd  - Connected client socket.
 *         static_dir - Root directory for static files.
 *         client_ip  - Remote IP, used for access logging.
 *         logger     - Logger to record the access in.
 */
void handleRequest(int client_fd, const std::string& static_dir,
                   const std::string& client_ip, Logger& logger);

/*
 * Builds a full HTTP/1.1 response string with headers and body.
 */
std::string constructResponse(const std::string& status,
                              const std::string& contentType,
                              const std::string& body);

}  // namespace http_server

#endif  // HTTP_SERVER_REQUEST_HANDLER_HPP

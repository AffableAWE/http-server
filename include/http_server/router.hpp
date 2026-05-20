#ifndef HTTP_SERVER_ROUTER_HPP
#define HTTP_SERVER_ROUTER_HPP

#include <string>

namespace http_server {

/*
 * Maps HTTP request paths to file paths in the static directory.
 * Falls back to the 404 page for unknown routes.
 * Input:  path        - The requested HTTP path, e.g. "/about".
 *         static_dir  - The root directory for served files.
 * Returns: The corresponding file path in the static directory.
 */
std::string routeToFile(const std::string& path, const std::string& static_dir);

}  // namespace http_server

#endif  // HTTP_SERVER_ROUTER_HPP

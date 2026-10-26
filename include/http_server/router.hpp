#ifndef HTTP_SERVER_ROUTER_HPP
#define HTTP_SERVER_ROUTER_HPP

#include <string>
#include <vector>

namespace http_server {

/*
 * A resolved allowlisted route. file_path is the asset to serve and
 * status_code is the HTTP status that must accompany it. Unknown routes
 * resolve to the fallback page with a 404 status.
 */
struct Route {
    std::string file_path;
    int status_code;
};

/*
 * Maps allowlisted HTTP paths to a static asset and the status to return.
 * Paths are expected to have their query string removed by the parser.
 * Input:  path        - The requested HTTP path, e.g. "/about".
 *         static_dir  - The root directory for served files.
 * Returns: The asset path and its response status.
 */
Route routeRequest(const std::string& path, const std::string& static_dir);

/*
 * Returns the fixed allowlisted request paths used to preload the cache.
 */
const std::vector<std::string>& allowlistedPaths();

}  // namespace http_server

#endif  // HTTP_SERVER_ROUTER_HPP

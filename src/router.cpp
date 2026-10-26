#include "http_server/router.hpp"

namespace http_server {

const std::vector<std::string>& allowlistedPaths() {
    static const std::vector<std::string> paths = {
        "/", "/about", "/404", "/aboutStyle.css", "/404Style.css"};
    return paths;
}

Route routeRequest(const std::string& path, const std::string& static_dir) {
    // Main pages
    if (path == "/") return {static_dir + "/index.html", 200};
    if (path == "/about") return {static_dir + "/about.html", 200};
    if (path == "/404") return {static_dir + "/404.html", 200};

    // CSS assets
    if (path == "/aboutStyle.css") return {static_dir + "/aboutStyle.css", 200};
    if (path == "/404Style.css") return {static_dir + "/404Style.css", 200};

    // Unknown route -> 404 page with an actual 404 status.
    return {static_dir + "/404.html", 404};
}

}  // namespace http_server

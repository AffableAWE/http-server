#include "http_server/router.hpp"

namespace http_server {

std::string routeToFile(const std::string& path,
                        const std::string& static_dir) {
    // Main pages
    if (path == "/") return static_dir + "/index.html";
    if (path == "/about") return static_dir + "/about.html";
    if (path == "/404") return static_dir + "/404.html";

    // CSS assets
    if (path == "/aboutStyle.css") return static_dir + "/aboutStyle.css";
    if (path == "/404Style.css") return static_dir + "/404Style.css";

    // Unknown route -> 404 page
    return static_dir + "/404.html";
}

}  // namespace http_server

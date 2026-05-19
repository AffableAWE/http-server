#include "http_server/utils.hpp"

#include <unordered_map>

namespace http_server {

std::string trim(const std::string& input) {
    // Find the first position which is not whitespace
    size_t start = input.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";

    // Find the last position which is not whitespace
    size_t end = input.find_last_not_of(" \t\r\n");
    if (end == std::string::npos) return "";

    // Return the trimmed substring
    return input.substr(start, end - start + 1);
}

std::string getContentType(const std::string& path) {
    static const std::unordered_map<std::string, std::string> mime_types = {
        {"html", "text/html"},
        {"css", "text/css"},
        {"js", "application/javascript"},
        {"json", "application/json"},
        {"png", "image/png"},
        {"jpg", "image/jpeg"},
        {"jpeg", "image/jpeg"}};

    size_t dotPosition = path.rfind('.');
    if (dotPosition == std::string::npos) return "application/octet-stream";

    std::string ext = path.substr(dotPosition + 1);
    auto it = mime_types.find(ext);
    return (it != mime_types.end()) ? it->second : "application/octet-stream";
}

}  // namespace http_server

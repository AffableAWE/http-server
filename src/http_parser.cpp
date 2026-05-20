#include "http_server/http_parser.hpp"

#include <sstream>

#include "http_server/utils.hpp"

namespace http_server {

ParsedRequest parseRequest(const std::string& accumulated_data) {
    ParsedRequest req;

    size_t request_line_end = accumulated_data.find("\r\n");
    if (request_line_end == std::string::npos) return req;

    size_t headers_end = accumulated_data.find("\r\n\r\n");
    if (headers_end == std::string::npos) return req;

    // Parse the request line: METHOD PATH HTTP_VERSION
    std::string request_line = accumulated_data.substr(0, request_line_end);
    std::istringstream iss(request_line);
    iss >> req.method >> req.path >> req.http_version;
    if (req.method.empty() || req.path.empty()) return req;

    // Parse headers section line by line
    size_t headers_start = request_line_end + 2;
    std::string headers_part =
        accumulated_data.substr(headers_start, headers_end - headers_start);

    size_t line_start = 0;
    while (line_start < headers_part.size()) {
        size_t line_end = headers_part.find("\r\n", line_start);
        if (line_end == std::string::npos) break;

        std::string line =
            headers_part.substr(line_start, line_end - line_start);
        line_start = line_end + 2;

        size_t colon_pos = line.find(":");
        if (colon_pos != std::string::npos) {
            std::string key = trim(line.substr(0, colon_pos));
            std::string value = trim(line.substr(colon_pos + 1));
            req.headers[key] = value;
        }
    }

    req.valid = true;
    return req;
}

}  // namespace http_server

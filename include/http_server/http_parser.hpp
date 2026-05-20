#ifndef HTTP_SERVER_HTTP_PARSER_HPP
#define HTTP_SERVER_HTTP_PARSER_HPP

#include <string>
#include <unordered_map>

namespace http_server {

/*
 * Holds the parsed components of an HTTP request line plus headers.
 */
struct ParsedRequest {
    std::string method;
    std::string path;
    std::string http_version;
    std::unordered_map<std::string, std::string> headers;
    bool valid = false;
};

/*
 * Parses the request line and headers from accumulated request data.
 * Expects the data to contain at least one full "\r\n\r\n" terminator.
 * Input:  accumulated_data - The raw bytes received from the client.
 * Returns: A ParsedRequest. .valid is false if parsing failed.
 */
ParsedRequest parseRequest(const std::string& accumulated_data);

}  // namespace http_server

#endif  // HTTP_SERVER_HTTP_PARSER_HPP

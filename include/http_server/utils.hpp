#ifndef HTTP_SERVER_UTILS_HPP
#define HTTP_SERVER_UTILS_HPP

#include <string>

namespace http_server {

/*
 * Removes leading and trailing whitespace (spaces, tabs, newlines)
 * from the given string.
 * Input:  input - The string to trim.
 * Returns: A new string without leading and trailing whitespace.
 */
std::string trim(const std::string& input);

/*
 * Maps file extensions to MIME types for the Content-Type header.
 * Input:  path - The file path to analyze for extension.
 * Returns: A string representing the MIME type, e.g., "text/html".
 *          Falls back to "application/octet-stream" for unknown extensions.
 */
std::string getContentType(const std::string& path);

}  // namespace http_server

#endif  // HTTP_SERVER_UTILS_HPP

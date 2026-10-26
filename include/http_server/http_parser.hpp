#ifndef HTTP_SERVER_HTTP_PARSER_HPP
#define HTTP_SERVER_HTTP_PARSER_HPP

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>

namespace http_server {

/*
 * Parser limits are deliberately checked against the bytes on the wire before
 * any routing or file-system work happens. max_header_bytes includes the
 * request line and the terminating CRLFCRLF.
 */
struct ParserLimits {
    std::size_t max_request_line_bytes = 8 * 1024;
    std::size_t max_header_bytes = 64 * 1024;
    std::size_t max_header_count = 100;
    std::size_t max_header_name_bytes = 256;
    std::size_t max_header_value_bytes = 8 * 1024;
};

enum class ParseError {
    None,
    BadRequest,
    UriTooLong,
    HeadersTooLarge,
    UnsupportedTransferEncoding,
    BodyNotSupported,
};

/*
 * Holds the parsed components of an HTTP request line plus headers.
 * Header names are stored lower-case so lookups are case-insensitive.
 * The request target is split into an origin-form path and an optional query
 * string; neither component is percent-decoded.
 */
struct ParsedRequest {
    std::string method;
    std::string path;
    std::string query;
    std::string http_version;
    std::unordered_map<std::string, std::string> headers;
    ParseError error = ParseError::BadRequest;
    bool keep_alive = false;
    bool valid = false;
};

enum class ParseStatus {
    NeedMore,
    Complete,
    Error,
};

/*
 * Result of parsing one request from a buffer that may also contain bytes for
 * a following request. consumed_bytes is set only when status is Complete;
 * the caller retains the unread suffix.
 */
struct ParseResult {
    ParseStatus status = ParseStatus::NeedMore;
    std::size_t consumed_bytes = 0;
    ParsedRequest request;
};

/*
 * Parses exactly one request from the beginning of data.
 * Returns NeedMore when no complete header block is available, Complete with
 * consumed_bytes when one request is valid, or Error when the request must be
 * rejected and the connection closed. Bytes after the current request are not
 * parsed or rejected; this is the persistent-connection entry point.
 */
ParseResult parseRequestFromBuffer(std::string_view data,
                                   const ParserLimits& limits = ParserLimits{});

/*
 * Compatibility wrapper for callers that expect exactly one request and no
 * trailing bytes. This preserves the previous close-per-request behavior.
 * .valid is false when the request is rejected or extra bytes are present.
 */
ParsedRequest parseRequest(const std::string& accumulated_data,
                           const ParserLimits& limits = ParserLimits{});

}  // namespace http_server

#endif  // HTTP_SERVER_HTTP_PARSER_HPP

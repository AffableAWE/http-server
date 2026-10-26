#include "http_server/http_parser.hpp"

namespace http_server {

namespace {

bool isTokenChar(unsigned char c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9')) {
        return true;
    }
    switch (c) {
        case '!':
        case '#':
        case '$':
        case '%':
        case '&':
        case '\'':
        case '*':
        case '+':
        case '-':
        case '.':
        case '^':
        case '_':
        case '`':
        case '|':
        case '~':
            return true;
        default:
            return false;
    }
}

bool isVisibleTargetChar(unsigned char c) {
    // Origin-form request targets are ASCII; spaces and controls are invalid.
    return c >= 0x21 && c <= 0x7e;
}

bool isHeaderValueChar(unsigned char c) {
    // HTAB and visible/obs-text bytes are allowed; other controls are not.
    return c == '\t' || (c >= 0x20 && c != 0x7f);
}

bool isValidHostValue(const std::string& value) {
    if (value.empty()) return false;
    for (unsigned char c : value) {
        if (c <= 0x20 || c >= 0x7f || c == ',' || c == '/' || c == '\\') {
            return false;
        }
    }
    return true;
}

std::string lowerAscii(std::string value) {
    for (char& c : value) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return value;
}

std::string trimOws(const std::string& value) {
    const std::size_t start = value.find_first_not_of(" \t");
    if (start == std::string::npos) return "";
    const std::size_t end = value.find_last_not_of(" \t");
    return value.substr(start, end - start + 1);
}

bool isAllDigits(const std::string& value) {
    if (value.empty()) return false;
    for (unsigned char c : value) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

bool isZeroDecimal(const std::string& value) {
    for (char c : value) {
        if (c != '0') return false;
    }
    return !value.empty();
}

bool containsConnectionToken(const std::string& value, const char* token) {
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t comma = value.find(',', start);
        const std::string part =
            (comma == std::string::npos)
                ? trimOws(value.substr(start))
                : trimOws(value.substr(start, comma - start));
        if (lowerAscii(part) == token) return true;
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return false;
}

}  // namespace

ParseResult parseRequestFromBuffer(std::string_view data,
                                   const ParserLimits& limits) {
    ParseResult result;
    ParsedRequest& req = result.request;

    const std::size_t headers_end = data.find("\r\n\r\n");
    if (headers_end == std::string::npos) {
        const std::size_t request_line_end = data.find("\r\n");
        if ((request_line_end != std::string::npos &&
             request_line_end > limits.max_request_line_bytes) ||
            (request_line_end == std::string::npos &&
             data.size() > limits.max_request_line_bytes)) {
            result.status = ParseStatus::Error;
            req.error = ParseError::UriTooLong;
            return result;
        }
        if (data.size() > limits.max_header_bytes) {
            result.status = ParseStatus::Error;
            req.error = ParseError::HeadersTooLarge;
            return result;
        }
        result.status = ParseStatus::NeedMore;
        return result;
    }

    // The limit includes the final CRLFCRLF, so a boundary just beyond the
    // limit must not be accepted based on an earlier read.
    if (headers_end + 4 > limits.max_header_bytes) {
        result.status = ParseStatus::Error;
        req.error = ParseError::HeadersTooLarge;
        return result;
    }

    const std::size_t request_line_end = data.find("\r\n");
    if (request_line_end == std::string::npos ||
        request_line_end > headers_end) {
        result.status = ParseStatus::Error;
        req.error = ParseError::BadRequest;
        return result;
    }
    if (request_line_end > limits.max_request_line_bytes) {
        result.status = ParseStatus::Error;
        req.error = ParseError::UriTooLong;
        return result;
    }

    const std::string request_line(data.substr(0, request_line_end));
    const std::size_t first_space = request_line.find(' ');
    if (first_space == std::string::npos || first_space == 0) {
        result.status = ParseStatus::Error;
        req.error = ParseError::BadRequest;
        return result;
    }
    const std::size_t second_space = request_line.find(' ', first_space + 1);
    if (second_space == std::string::npos ||
        second_space == first_space + 1) {
        result.status = ParseStatus::Error;
        req.error = ParseError::BadRequest;
        return result;
    }
    if (request_line.find(' ', second_space + 1) != std::string::npos) {
        result.status = ParseStatus::Error;
        req.error = ParseError::BadRequest;
        return result;
    }

    req.method = request_line.substr(0, first_space);
    const std::string target =
        request_line.substr(first_space + 1, second_space - first_space - 1);
    req.http_version = request_line.substr(second_space + 1);

    for (unsigned char c : req.method) {
        if (!isTokenChar(c)) {
            result.status = ParseStatus::Error;
            req.error = ParseError::BadRequest;
            return result;
        }
    }

    if (req.http_version != "HTTP/1.0" && req.http_version != "HTTP/1.1") {
        result.status = ParseStatus::Error;
        req.error = ParseError::BadRequest;
        return result;
    }

    if (target.empty() || target[0] != '/') {
        result.status = ParseStatus::Error;
        req.error = ParseError::BadRequest;
        return result;
    }
    for (unsigned char c : target) {
        if (!isVisibleTargetChar(c) || c == '#') {
            result.status = ParseStatus::Error;
            req.error = ParseError::BadRequest;
            return result;
        }
    }

    const std::size_t query_pos = target.find('?');
    if (query_pos == std::string::npos) {
        req.path = target;
    } else {
        req.path = target.substr(0, query_pos);
        req.query = target.substr(query_pos + 1);
    }

    const std::size_t headers_start = request_line_end + 2;
    const std::string headers_part =
        (headers_end > headers_start)
            ? std::string(data.substr(headers_start,
                                      headers_end - headers_start))
            : std::string();

    std::size_t header_count = 0;
    std::size_t host_count = 0;
    std::size_t content_length_count = 0;
    std::size_t transfer_encoding_count = 0;
    bool has_content_length = false;
    bool has_transfer_encoding = false;
    bool content_length_is_zero = true;

    std::size_t line_start = 0;
    while (line_start < headers_part.size()) {
        const std::size_t line_end = headers_part.find("\r\n", line_start);
        const std::string line =
            (line_end == std::string::npos)
                ? headers_part.substr(line_start)
                : headers_part.substr(line_start, line_end - line_start);
        line_start = (line_end == std::string::npos) ? headers_part.size()
                                                     : line_end + 2;

        if (line.empty()) {
            result.status = ParseStatus::Error;
            req.error = ParseError::BadRequest;
            return result;
        }

        // Obsolete line folding is not accepted.
        if (line[0] == ' ' || line[0] == '\t') {
            result.status = ParseStatus::Error;
            req.error = ParseError::BadRequest;
            return result;
        }

        const std::size_t colon = line.find(':');
        if (colon == std::string::npos || colon == 0) {
            result.status = ParseStatus::Error;
            req.error = ParseError::BadRequest;
            return result;
        }

        const std::string name = line.substr(0, colon);
        if (name.size() > limits.max_header_name_bytes) {
            result.status = ParseStatus::Error;
            req.error = ParseError::HeadersTooLarge;
            return result;
        }
        for (unsigned char c : name) {
            if (!isTokenChar(c)) {
                result.status = ParseStatus::Error;
                req.error = ParseError::BadRequest;
                return result;
            }
        }

        const std::string value = trimOws(line.substr(colon + 1));
        if (value.size() > limits.max_header_value_bytes) {
            result.status = ParseStatus::Error;
            req.error = ParseError::HeadersTooLarge;
            return result;
        }
        for (unsigned char c : value) {
            if (!isHeaderValueChar(c)) {
                result.status = ParseStatus::Error;
                req.error = ParseError::BadRequest;
                return result;
            }
        }

        ++header_count;
        if (header_count > limits.max_header_count) {
            result.status = ParseStatus::Error;
            req.error = ParseError::HeadersTooLarge;
            return result;
        }

        const std::string lower_name = lowerAscii(name);
        if (lower_name == "host") {
            ++host_count;
            if (host_count > 1 || !isValidHostValue(value)) {
                result.status = ParseStatus::Error;
                req.error = ParseError::BadRequest;
                return result;
            }
        } else if (lower_name == "content-length") {
            ++content_length_count;
            if (content_length_count > 1 || !isAllDigits(value)) {
                result.status = ParseStatus::Error;
                req.error = ParseError::BadRequest;
                return result;
            }
            has_content_length = true;
            content_length_is_zero = isZeroDecimal(value);
        } else if (lower_name == "transfer-encoding") {
            ++transfer_encoding_count;
            if (transfer_encoding_count > 1 || value.empty()) {
                result.status = ParseStatus::Error;
                req.error = ParseError::BadRequest;
                return result;
            }
            has_transfer_encoding = true;
        }

        // Header names are normalized. Generic duplicates are combined as a
        // comma-separated field value; framing fields are rejected above.
        auto existing = req.headers.find(lower_name);
        if (existing == req.headers.end()) {
            req.headers.emplace(lower_name, value);
        } else {
            existing->second += ", ";
            existing->second += value;
        }
    }

    if (req.http_version == "HTTP/1.1" && host_count != 1) {
        result.status = ParseStatus::Error;
        req.error = ParseError::BadRequest;
        return result;
    }
    if (has_transfer_encoding && has_content_length) {
        result.status = ParseStatus::Error;
        req.error = ParseError::BadRequest;
        return result;
    }
    if (has_transfer_encoding) {
        result.status = ParseStatus::Error;
        req.error = ParseError::UnsupportedTransferEncoding;
        return result;
    }
    if (has_content_length && !content_length_is_zero) {
        result.status = ParseStatus::Error;
        req.error = ParseError::BodyNotSupported;
        return result;
    }

    const auto connection = req.headers.find("connection");
    const bool connection_close =
        connection != req.headers.end() &&
        containsConnectionToken(connection->second, "close");
    const bool connection_keep_alive =
        connection != req.headers.end() &&
        containsConnectionToken(connection->second, "keep-alive");

    if (req.http_version == "HTTP/1.1") {
        req.keep_alive = !connection_close;
    } else {
        req.keep_alive = connection_keep_alive && !connection_close;
    }

    req.valid = true;
    req.error = ParseError::None;
    result.status = ParseStatus::Complete;
    result.consumed_bytes = headers_end + 4;
    return result;
}

ParsedRequest parseRequest(const std::string& accumulated_data,
                           const ParserLimits& limits) {
    ParseResult result = parseRequestFromBuffer(accumulated_data, limits);
    if (result.status != ParseStatus::Complete) {
        ParsedRequest invalid;
        invalid.error = (result.status == ParseStatus::Error)
                            ? result.request.error
                            : ParseError::BadRequest;
        return invalid;
    }
    if (result.consumed_bytes != accumulated_data.size()) {
        // Preserve the close-per-request behavior for existing callers.
        result.request.valid = false;
        result.request.error = ParseError::BadRequest;
    }
    return result.request;
}

}  // namespace http_server

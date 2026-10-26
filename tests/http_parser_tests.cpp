#include "http_server/http_parser.hpp"

#include <iostream>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const char* expression, int line) {
    if (condition) return;
    std::cerr << "FAIL line " << line << ": " << expression << "\n";
    ++failures;
}

#define CHECK(expression) check((expression), #expression, __LINE__)

const char* errorName(http_server::ParseError error) {
    using http_server::ParseError;
    switch (error) {
        case ParseError::None:
            return "None";
        case ParseError::BadRequest:
            return "BadRequest";
        case ParseError::UriTooLong:
            return "UriTooLong";
        case ParseError::HeadersTooLarge:
            return "HeadersTooLarge";
        case ParseError::UnsupportedTransferEncoding:
            return "UnsupportedTransferEncoding";
        case ParseError::BodyNotSupported:
            return "BodyNotSupported";
    }
    return "unknown";
}

void expectError(const std::string& input, http_server::ParseError expected,
                 int line) {
    const http_server::ParsedRequest request = http_server::parseRequest(input);
    if (request.valid || request.error != expected) {
        std::cerr << "FAIL line " << line << ": expected "
                  << errorName(expected) << ", got "
                  << (request.valid ? "valid" : errorName(request.error))
                  << "\n";
        ++failures;
    }
}

#define EXPECT_ERROR(input, error) expectError((input), (error), __LINE__)

void testValidRequestAndFinalHeader() {
    const auto request = http_server::parseRequest(
        "GET / HTTP/1.1\r\nHost: example.com\r\nX-Last: value\r\n\r\n");
    CHECK(request.valid);
    CHECK(request.error == http_server::ParseError::None);
    CHECK(request.method == "GET");
    CHECK(request.path == "/");
    CHECK(request.query.empty());
    CHECK(request.http_version == "HTTP/1.1");
    CHECK(request.headers.at("host") == "example.com");
    CHECK(request.headers.at("x-last") == "value");
}

void testCaseInsensitiveHeaderNames() {
    const auto request = http_server::parseRequest(
        "GET / HTTP/1.1\r\nhOsT: example.com\r\nX-MiXeD-CaSe: yes\r\n\r\n");
    CHECK(request.valid);
    CHECK(request.headers.count("HOST") == 0);
    CHECK(request.headers.at("host") == "example.com");
    CHECK(request.headers.at("x-mixed-case") == "yes");
}

void testQuerySeparationWithoutDecoding() {
    const auto request = http_server::parseRequest(
        "GET /about?name=a%20b&x=1 HTTP/1.1\r\nHost: example.com\r\n\r\n");
    CHECK(request.valid);
    CHECK(request.path == "/about");
    CHECK(request.query == "name=a%20b&x=1");
}

void testHttp10AndHostRules() {
    CHECK(http_server::parseRequest("GET / HTTP/1.0\r\n\r\n").valid);

    const auto with_host = http_server::parseRequest(
        "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n");
    CHECK(with_host.valid);
    CHECK(with_host.headers.at("host") == "example.com");

    EXPECT_ERROR("GET / HTTP/1.1\r\n\r\n", http_server::ParseError::BadRequest);
    EXPECT_ERROR(
        "GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n",
        http_server::ParseError::BadRequest);
    EXPECT_ERROR("GET / HTTP/1.1\r\nHost:\r\n\r\n",
                 http_server::ParseError::BadRequest);
    EXPECT_ERROR("GET / HTTP/1.1\r\nHost: a b\r\n\r\n",
                 http_server::ParseError::BadRequest);
}

void testRequestLineValidation() {
    EXPECT_ERROR("GET /\r\nHost: example.com\r\n\r\n",
                 http_server::ParseError::BadRequest);
    EXPECT_ERROR("GET  / HTTP/1.1\r\nHost: example.com\r\n\r\n",
                 http_server::ParseError::BadRequest);
    EXPECT_ERROR("GET / HTTP/1.1 extra\r\nHost: example.com\r\n\r\n",
                 http_server::ParseError::BadRequest);
    EXPECT_ERROR("GET\t/ HTTP/1.1\r\nHost: example.com\r\n\r\n",
                 http_server::ParseError::BadRequest);
    EXPECT_ERROR("G@T / HTTP/1.1\r\nHost: example.com\r\n\r\n",
                 http_server::ParseError::BadRequest);
    EXPECT_ERROR("GET / HTTP/9.9\r\nHost: example.com\r\n\r\n",
                 http_server::ParseError::BadRequest);
    EXPECT_ERROR("GET / http/1.1\r\nHost: example.com\r\n\r\n",
                 http_server::ParseError::BadRequest);
}

void testRequestTargetValidation() {
    EXPECT_ERROR(
        "GET http://example.com/ HTTP/1.1\r\nHost: example.com\r\n\r\n",
        http_server::ParseError::BadRequest);
    EXPECT_ERROR("GET * HTTP/1.1\r\nHost: example.com\r\n\r\n",
                 http_server::ParseError::BadRequest);
    EXPECT_ERROR("GET about HTTP/1.1\r\nHost: example.com\r\n\r\n",
                 http_server::ParseError::BadRequest);
    EXPECT_ERROR("GET /about#fragment HTTP/1.1\r\nHost: example.com\r\n\r\n",
                 http_server::ParseError::BadRequest);
    EXPECT_ERROR(std::string("GET /about\x01 HTTP/1.1\r\n") +
                     "Host: example.com\r\n\r\n",
                 http_server::ParseError::BadRequest);
}

void testHeaderSyntaxValidation() {
    EXPECT_ERROR("GET / HTTP/1.1\r\nHost example.com\r\n\r\n",
                 http_server::ParseError::BadRequest);
    EXPECT_ERROR("GET / HTTP/1.1\r\nHost : example.com\r\n\r\n",
                 http_server::ParseError::BadRequest);
    EXPECT_ERROR("GET / HTTP/1.1\r\nHost: example.com\r\n folded\r\n\r\n",
                 http_server::ParseError::BadRequest);
    EXPECT_ERROR("GET / HTTP/1.1\r\n: value\r\nHost: example.com\r\n\r\n",
                 http_server::ParseError::BadRequest);
    EXPECT_ERROR(std::string("GET / HTTP/1.1\r\nHost: example.com\r\nX: a") +
                     '\x01' + "\r\n\r\n",
                 http_server::ParseError::BadRequest);
    EXPECT_ERROR("GET / HTTP/1.1\r\nHost: example.com,other\r\n\r\n",
                 http_server::ParseError::BadRequest);
}

void testContentLengthAndTransferEncoding() {
    CHECK(http_server::parseRequest(
              "GET / HTTP/1.1\r\nHost: example.com\r\nContent-Length: 0\r\n\r\n")
              .valid);
    CHECK(http_server::parseRequest(
              "GET / HTTP/1.1\r\nHost: example.com\r\nContent-Length: 00\r\n\r\n")
              .valid);

    EXPECT_ERROR(
        "GET / HTTP/1.1\r\nHost: example.com\r\nContent-Length: 5\r\n\r\n",
        http_server::ParseError::BodyNotSupported);
    EXPECT_ERROR(
        "GET / HTTP/1.1\r\nHost: example.com\r\nContent-Length: nope\r\n\r\n",
        http_server::ParseError::BadRequest);
    EXPECT_ERROR(
        "GET / HTTP/1.1\r\nHost: example.com\r\nContent-Length: 0\r\n"
        "Content-Length: 0\r\n\r\n",
        http_server::ParseError::BadRequest);
    EXPECT_ERROR(
        "GET / HTTP/1.1\r\nHost: example.com\r\nTransfer-Encoding: chunked\r\n"
        "\r\n",
        http_server::ParseError::UnsupportedTransferEncoding);
    EXPECT_ERROR(
        "GET / HTTP/1.1\r\nHost: example.com\r\nTransfer-Encoding:\r\n\r\n",
        http_server::ParseError::BadRequest);
    EXPECT_ERROR(
        "GET / HTTP/1.1\r\nHost: example.com\r\nContent-Length: 0\r\n"
        "Transfer-Encoding: chunked\r\n\r\n",
        http_server::ParseError::BadRequest);
    EXPECT_ERROR(
        "GET / HTTP/1.1\r\nHost: example.com\r\nContent-Length: 0\r\n\r\n"
        "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n",
        http_server::ParseError::BadRequest);
}

void testLimits() {
    http_server::ParserLimits limits;
    limits.max_request_line_bytes = 10;
    CHECK(!http_server::parseRequest(
               "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n", limits)
               .valid);
    CHECK(http_server::parseRequest(
              "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n", limits)
              .error == http_server::ParseError::UriTooLong);

    limits = http_server::ParserLimits{};
    limits.max_header_bytes = 10;
    CHECK(http_server::parseRequest(
              "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n", limits)
              .error == http_server::ParseError::HeadersTooLarge);

    limits = http_server::ParserLimits{};
    limits.max_header_count = 1;
    CHECK(http_server::parseRequest(
              "GET / HTTP/1.1\r\nHost: example.com\r\nX: y\r\n\r\n", limits)
              .error == http_server::ParseError::HeadersTooLarge);

    limits = http_server::ParserLimits{};
    limits.max_header_name_bytes = 3;
    CHECK(http_server::parseRequest(
              "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n", limits)
              .error == http_server::ParseError::HeadersTooLarge);

    limits = http_server::ParserLimits{};
    limits.max_header_value_bytes = 3;
    CHECK(http_server::parseRequest(
              "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n", limits)
              .error == http_server::ParseError::HeadersTooLarge);
}


void testIncrementalAndPipelinedRequests() {
    const std::string first =
        "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n";
    const std::string second =
        "GET /about?x=1 HTTP/1.1\r\nHost: example.com\r\n\r\n";

    for (std::size_t split = 1; split < first.size(); ++split) {
        const auto fragmented =
            http_server::parseRequestFromBuffer(first.substr(0, split));
        CHECK(fragmented.status == http_server::ParseStatus::NeedMore);
    }

    const auto complete = http_server::parseRequestFromBuffer(first);
    CHECK(complete.status == http_server::ParseStatus::Complete);
    CHECK(complete.consumed_bytes == first.size());
    CHECK(complete.request.path == "/");

    const std::string coalesced = first + second;
    const auto first_result =
        http_server::parseRequestFromBuffer(coalesced);
    CHECK(first_result.status == http_server::ParseStatus::Complete);
    CHECK(first_result.consumed_bytes == first.size());

    const auto second_result = http_server::parseRequestFromBuffer(
        std::string_view(coalesced).substr(first_result.consumed_bytes));
    CHECK(second_result.status == http_server::ParseStatus::Complete);
    CHECK(second_result.consumed_bytes == second.size());
    CHECK(second_result.request.path == "/about");
    CHECK(second_result.request.query == "x=1");

    // The strict compatibility wrapper must still reject extra bytes.
    const auto strict = http_server::parseRequest(coalesced);
    CHECK(!strict.valid);
    CHECK(strict.error == http_server::ParseError::BadRequest);
}

void testKeepAlivePolicy() {
    const auto http11 = http_server::parseRequestFromBuffer(
        "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n");
    CHECK(http11.status == http_server::ParseStatus::Complete);
    CHECK(http11.request.keep_alive);

    const auto http11_close = http_server::parseRequestFromBuffer(
        "GET / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n");
    CHECK(http11_close.status == http_server::ParseStatus::Complete);
    CHECK(!http11_close.request.keep_alive);

    const auto http10 = http_server::parseRequestFromBuffer(
        "GET / HTTP/1.0\r\n\r\n");
    CHECK(http10.status == http_server::ParseStatus::Complete);
    CHECK(!http10.request.keep_alive);

    const auto http10_keep_alive = http_server::parseRequestFromBuffer(
        "GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n");
    CHECK(http10_keep_alive.status == http_server::ParseStatus::Complete);
    CHECK(http10_keep_alive.request.keep_alive);

    const auto close_wins = http_server::parseRequestFromBuffer(
        "GET / HTTP/1.1\r\nHost: example.com\r\n"
        "Connection: keep-alive, close\r\n\r\n");
    CHECK(close_wins.status == http_server::ParseStatus::Complete);
    CHECK(!close_wins.request.keep_alive);

    const auto repeated_connection = http_server::parseRequestFromBuffer(
        "GET / HTTP/1.1\r\nHost: example.com\r\nConnection: keep-alive\r\n"
        "Connection: close\r\n\r\n");
    CHECK(repeated_connection.status == http_server::ParseStatus::Complete);
    CHECK(!repeated_connection.request.keep_alive);
}

void testCoalescedBodyFramingIsRejected() {
    const auto body = http_server::parseRequestFromBuffer(
        "GET / HTTP/1.1\r\nHost: example.com\r\nContent-Length: 4\r\n\r\n"
        "body");
    CHECK(body.status == http_server::ParseStatus::Error);
    CHECK(body.request.error == http_server::ParseError::BodyNotSupported);
    CHECK(body.consumed_bytes == 0);

    const auto chunked = http_server::parseRequestFromBuffer(
        "GET / HTTP/1.1\r\nHost: example.com\r\n"
        "Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n");
    CHECK(chunked.status == http_server::ParseStatus::Error);
    CHECK(chunked.request.error ==
          http_server::ParseError::UnsupportedTransferEncoding);
}

void testIncompleteAndEmptyInput() {
    EXPECT_ERROR("", http_server::ParseError::BadRequest);
    EXPECT_ERROR("GET / HTTP/1.1\r\nHost: example.com\r\n",
                 http_server::ParseError::BadRequest);
}

}  // namespace

int main() {
    testValidRequestAndFinalHeader();
    testCaseInsensitiveHeaderNames();
    testQuerySeparationWithoutDecoding();
    testHttp10AndHostRules();
    testRequestLineValidation();
    testRequestTargetValidation();
    testHeaderSyntaxValidation();
    testContentLengthAndTransferEncoding();
    testIncrementalAndPipelinedRequests();
    testKeepAlivePolicy();
    testCoalescedBodyFramingIsRejected();
    testLimits();
    testIncompleteAndEmptyInput();

    if (failures != 0) {
        std::cerr << failures << " parser test(s) failed\n";
        return 1;
    }
    std::cout << "all parser tests passed\n";
    return 0;
}

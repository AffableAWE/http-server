#include "http_server/request_handler.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>

#include "http_server/asset_cache.hpp"
#include "http_server/deadline.hpp"
#include "http_server/http_parser.hpp"
#include "http_server/logger.hpp"

namespace http_server {

namespace {

constexpr int BUFFER_SIZE = 4096;

struct ParseErrorResponse {
    int status_code;
    const char* status_line;
    const char* body;
};

ParseErrorResponse parseErrorResponse(ParseError error) {
    switch (error) {
        case ParseError::UriTooLong:
            return {414, "414 URI Too Long", "URI Too Long"};
        case ParseError::HeadersTooLarge:
            return {431, "431 Request Header Fields Too Large",
                    "Request Header Fields Too Large"};
        case ParseError::UnsupportedTransferEncoding:
            return {501, "501 Not Implemented",
                    "Transfer-Encoding is not implemented"};
        case ParseError::BodyNotSupported:
            return {400, "400 Bad Request", "Request body is not supported"};
        case ParseError::BadRequest:
        case ParseError::None:
        default:
            return {400, "400 Bad Request", "Bad Request"};
    }
}

enum class WaitResult { Ready, Timeout, Error };
enum class ReadResult { Ok, Timeout, Disconnected, Error };

struct WriteResult {
    std::size_t written = 0;
    TransferFailure failure = TransferFailure::None;

    bool complete() const { return failure == TransferFailure::None; }
};

WaitResult waitFor(int fd, short events, const Deadline& total_deadline,
                   int per_wait_timeout_ms, short& revents) {
    const int total_remaining = total_deadline.remainingMilliseconds();
    if (total_remaining <= 0) return WaitResult::Timeout;

    const int wait_ms = std::min(per_wait_timeout_ms, total_remaining);
    const Deadline wait_deadline =
        Deadline::after(std::chrono::milliseconds(wait_ms));

    while (true) {
        const int remaining =
            std::min(wait_deadline.remainingMilliseconds(),
                     total_deadline.remainingMilliseconds());
        if (remaining <= 0) return WaitResult::Timeout;

        pollfd pfd{fd, events, 0};
        const int ready = ::poll(&pfd, 1, remaining);
        if (ready > 0) {
            revents = pfd.revents;
            if (revents & POLLNVAL) return WaitResult::Error;
            return WaitResult::Ready;
        }
        if (ready == 0) return WaitResult::Timeout;
        if (errno == EINTR) continue;
        return WaitResult::Error;
    }
}

TransferFailure socketErrnoFailure() {
    if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN) {
        return TransferFailure::Disconnected;
    }
    return TransferFailure::Error;
}

ReadResult readSome(int fd, char* buffer, std::size_t capacity,
                    const Deadline& deadline, int recv_timeout_ms,
                    std::size_t& received) {
    while (true) {
        short revents = 0;
        const WaitResult wait =
            waitFor(fd, POLLIN, deadline, recv_timeout_ms, revents);
        if (wait == WaitResult::Timeout) return ReadResult::Timeout;
        if (wait == WaitResult::Error) return ReadResult::Error;

        const ssize_t n = ::recv(fd, buffer, capacity, MSG_DONTWAIT);
        if (n > 0) {
            received = static_cast<std::size_t>(n);
            return ReadResult::Ok;
        }
        if (n == 0) return ReadResult::Disconnected;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
        return socketErrnoFailure() == TransferFailure::Disconnected
                   ? ReadResult::Disconnected
                   : ReadResult::Error;
    }
}

WriteResult writeAll(int fd, const char* data, std::size_t length,
                     const Deadline& deadline, int send_timeout_ms) {
    WriteResult result;
    while (result.written < length) {
        short revents = 0;
        const WaitResult wait =
            waitFor(fd, POLLOUT, deadline, send_timeout_ms, revents);
        if (wait == WaitResult::Timeout) {
            result.failure = TransferFailure::Timeout;
            return result;
        }
        if (wait == WaitResult::Error) {
            result.failure = TransferFailure::Error;
            return result;
        }

        const ssize_t n = ::send(fd, data + result.written,
                                 length - result.written, MSG_DONTWAIT);
        if (n > 0) {
            result.written += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        result.failure =
            n == 0 ? TransferFailure::Disconnected : socketErrnoFailure();
        return result;
    }
    return result;
}

WriteResult sendResponse(int fd, const std::string& response,
                         const Config& cfg) {
    const Deadline deadline =
        Deadline::after(std::chrono::milliseconds(cfg.write_timeout_ms));
    return writeAll(fd, response.data(), response.size(), deadline,
                    cfg.send_timeout_ms);
}

WriteResult send408BestEffort(int fd) {
    static constexpr char response[] =
        "HTTP/1.1 408 Request Timeout\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 15\r\n"
        "Connection: close\r\n"
        "\r\n"
        "Request timeout";
    // Single nonblocking write. No retry: a 408 must not become another
    // unbounded write.
    const ssize_t n =
        ::send(fd, response, sizeof(response) - 1, MSG_DONTWAIT);
    WriteResult result;
    if (n > 0) {
        result.written = static_cast<std::size_t>(n);
        if (result.written < sizeof(response) - 1) {
            result.failure = TransferFailure::Error;
        }
        return result;
    }
    result.failure =
            n == 0 ? TransferFailure::Disconnected : socketErrnoFailure();
    return result;
}

bool cancellationRequested(const ConnectionTask& task) {
    return task.cancelled &&
           task.cancelled->load(std::memory_order_relaxed);
}

}  // anonymous namespace

std::string constructResponse(const std::string& status,
                              const std::string& contentType,
                              const std::string& body,
                              const std::string& extra_headers) {
    return "HTTP/1.1 " + status + "\r\n" + "Content-Type: " + contentType +
           "\r\n" + "Content-Length: " + std::to_string(body.size()) + "\r\n" +
           extra_headers + "Connection: close\r\n\r\n" + body;
}

void handleRequest(ConnectionTask& task, const Config& cfg,
                   const AssetCache& cache, const std::string& client_ip,
                   Logger* logger) {
    const int client_fd = task.socket.get();
    char buffer[BUFFER_SIZE];
    std::string accumulated_data;
    accumulated_data.reserve(BUFFER_SIZE);

    int status_code = 0;
    std::size_t bytes_sent = 0;
    TransferOutcome outcome = TransferOutcome::Unknown;
    std::string log_method = "-";
    std::string log_path = "-";
    const ParserLimits parser_limits;
    const Deadline header_deadline = task.header_deadline;

    const auto cancelled_now = [&task]() {
        return cancellationRequested(task);
    };

    // Queue wait is part of the request budget. Do not grant an expired task a
    // fresh header timeout when a worker finally picks it up.
    if (header_deadline.expired()) {
        const WriteResult write = send408BestEffort(client_fd);
        bytes_sent = write.written;
        status_code = 408;
        outcome = cancelled_now() ? TransferOutcome::ShutdownCancelled
                                  : TransferOutcome::Timeout;
        goto done;
    }

    // 1) Read until the header boundary is found, but validate the boundary
    // position after every read so a single oversized read cannot slip past
    // the limit.
    while (true) {
        const std::size_t headers_end = accumulated_data.find("\r\n\r\n");
        if (headers_end != std::string::npos) {
            if (headers_end + 4 > parser_limits.max_header_bytes) {
                std::string resp = constructResponse(
                    "431 Request Header Fields Too Large", "text/plain",
                    "Request Header Fields Too Large");
                const WriteResult write = sendResponse(client_fd, resp, cfg);
                bytes_sent = write.written;
                status_code = 431;
                outcome = classifyTransferOutcome(write.written, write.failure, cancelled_now());
                goto done;
            }
            break;
        }
        if (accumulated_data.size() >= parser_limits.max_header_bytes) {
            std::string resp = constructResponse(
                "431 Request Header Fields Too Large", "text/plain",
                "Request Header Fields Too Large");
            const WriteResult write = sendResponse(client_fd, resp, cfg);
            bytes_sent = write.written;
            status_code = 431;
            outcome = classifyTransferOutcome(write.written, write.failure, cancelled_now());
            goto done;
        }

        std::size_t bytes_received = 0;
        const ReadResult read_result =
            readSome(client_fd, buffer, BUFFER_SIZE, header_deadline,
                     cfg.recv_timeout_ms, bytes_received);
        if (read_result == ReadResult::Timeout) {
            // Best-effort, single nonblocking write. The descriptor will be
            // closed by RAII after logging.
            const WriteResult write = send408BestEffort(client_fd);
            bytes_sent = write.written;
            status_code = 408;
            outcome = cancelled_now() ? TransferOutcome::ShutdownCancelled
                                      : TransferOutcome::Timeout;
            goto done;
        }
        if (read_result == ReadResult::Disconnected) {
            outcome = cancelled_now() ? TransferOutcome::ShutdownCancelled
                                      : TransferOutcome::Disconnected;
            goto done;
        }
        if (read_result == ReadResult::Error) {
            outcome = cancelled_now() ? TransferOutcome::ShutdownCancelled
                                      : TransferOutcome::Failed;
            goto done;
        }
        accumulated_data.append(buffer, bytes_received);
    }

    // 2) Parse the request.
    {
        ParsedRequest req = parseRequest(accumulated_data, parser_limits);
        if (!req.valid) {
            const ParseErrorResponse error = parseErrorResponse(req.error);
            std::string resp = constructResponse(error.status_line, "text/plain",
                                                 error.body);
            const WriteResult write = sendResponse(client_fd, resp, cfg);
            bytes_sent = write.written;
            status_code = error.status_code;
            outcome = classifyTransferOutcome(write.written, write.failure, cancelled_now());
            goto done;
        }

        log_method = req.method;
        log_path = req.path;

        if (req.method != "GET") {
            std::string resp = constructResponse(
                "405 Method Not Allowed", "text/plain", "Method Not Allowed",
                "Allow: GET\r\n");
            const WriteResult write = sendResponse(client_fd, resp, cfg);
            bytes_sent = write.written;
            status_code = 405;
            outcome = classifyTransferOutcome(write.written, write.failure, cancelled_now());
            goto done;
        }

        // 3) Look up the immutable preloaded response.
        const CachedAsset& asset = cache.lookup(req.path);
        const std::string headers =
            asset.header_prefix + "Connection: close\r\n\r\n";

        const Deadline write_deadline =
            Deadline::after(std::chrono::milliseconds(cfg.write_timeout_ms));
        const WriteResult header_write =
            writeAll(client_fd, headers.data(), headers.size(), write_deadline,
                     cfg.send_timeout_ms);
        bytes_sent += header_write.written;
        if (!header_write.complete()) {
            status_code = asset.status_code;
            outcome = classifyTransferOutcome(header_write.written, header_write.failure, cancelled_now());
            goto done;
        }

        if (!asset.body->empty()) {
            const WriteResult body_write =
                writeAll(client_fd, asset.body->data(), asset.body->size(),
                         write_deadline, cfg.send_timeout_ms);
            bytes_sent += body_write.written;
            if (!body_write.complete()) {
                status_code = asset.status_code;
                // Headers may already be on the wire. Classify the whole
                // response, not just the body fragment, so a generic
                // zero-body-byte send failure is Partial rather than Failed.
                outcome = classifyTransferOutcome(
                    bytes_sent, body_write.failure, cancelled_now());
                goto done;
            }
        }
        status_code = asset.status_code;
        outcome = TransferOutcome::Complete;
    }

done:
    if (outcome == TransferOutcome::Unknown) {
        outcome = cancelled_now() ? TransferOutcome::ShutdownCancelled
                                  : TransferOutcome::Failed;
    }
    if (cfg.enable_logging && logger) {
        logger->logAccess(log_method, log_path, status_code, bytes_sent,
                          outcome, client_ip);
    }
    // client_socket closes the descriptor when this function returns.
}

}  // namespace http_server

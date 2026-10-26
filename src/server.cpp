#include "http_server/server.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "http_server/deadline.hpp"
#include "http_server/http_parser.hpp"
#include "http_server/request_handler.hpp"
#include "http_server/transfer_outcome.hpp"

namespace http_server {

static_assert(std::atomic<bool>::is_always_lock_free,
              "signal-safe shutdown requires a lock-free atomic bool");

namespace {

constexpr std::size_t READ_CHUNK = 16 * 1024;
constexpr int MAX_ACCEPTS_PER_ITERATION = 64;
constexpr int MAX_PARSES_PER_ITERATION = 2;
constexpr int POLL_TIMEOUT_MS = 500;
constexpr const char* KEEPALIVE_HEADER = "Connection: keep-alive";
constexpr const char* CLOSE_HEADER = "Connection: close";

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

std::string addrToString(const sockaddr_storage& ss) {
    char buf[INET6_ADDRSTRLEN] = {0};
    if (ss.ss_family == AF_INET) {
        const auto* a = reinterpret_cast<const sockaddr_in*>(&ss);
        inet_ntop(AF_INET, &a->sin_addr, buf, sizeof(buf));
    } else if (ss.ss_family == AF_INET6) {
        const auto* a = reinterpret_cast<const sockaddr_in6*>(&ss);
        inet_ntop(AF_INET6, &a->sin6_addr, buf, sizeof(buf));
    }
    return buf[0] ? std::string(buf) : std::string("-");
}

bool makeNonBlocking(int fd) noexcept {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool familyUnavailable(int error) {
    return error == EAFNOSUPPORT || error == EPROTONOSUPPORT;
}

void sendOverloadResponse(int client_fd) noexcept {
    static constexpr char response[] =
        "HTTP/1.1 503 Service Unavailable\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 17\r\n"
        "Retry-After: 1\r\n"
        "Connection: close\r\n"
        "\r\n"
        "Server overloaded";
    (void)::send(client_fd, response, sizeof(response) - 1, MSG_DONTWAIT);
}

TransferFailure socketErrnoFailure() {
    if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN) {
        return TransferFailure::Disconnected;
    }
    return TransferFailure::Error;
}

bool cancelledNow(const Connection& connection) {
    return connection.cancelled &&
           connection.cancelled->load(std::memory_order_relaxed);
}

enum class ListenerResult { Ready, Unavailable, Error };

ListenerResult setupListener(int family, int port, UniqueFd& out,
                            const char* label) {
    const int fd = ::socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        const int error = errno;
        if (familyUnavailable(error)) {
            std::cerr << "[WARNING] " << label << " unavailable ("
                      << std::strerror(error)
                      << "); continuing with the other family." << std::endl;
            return ListenerResult::Unavailable;
        }
        std::cerr << "[ERROR] " << label << " socket: "
                  << std::strerror(error) << std::endl;
        return ListenerResult::Error;
    }

    UniqueFd candidate(fd);
    if (!makeNonBlocking(candidate.get())) {
        std::perror("listener nonblocking");
        return ListenerResult::Error;
    }

    int reuse = 1;
    if (::setsockopt(candidate.get(), SOL_SOCKET, SO_REUSEADDR, &reuse,
                     sizeof(reuse)) < 0) {
        std::perror("SO_REUSEADDR");
        return ListenerResult::Error;
    }

    if (family == AF_INET6) {
        int v6only = 1;
        if (::setsockopt(candidate.get(), IPPROTO_IPV6, IPV6_V6ONLY, &v6only,
                         sizeof(v6only)) < 0) {
            std::perror("IPV6_V6ONLY");
            return ListenerResult::Error;
        }
    }

    if (family == AF_INET) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        addr.sin_addr.s_addr = INADDR_ANY;
        if (::bind(candidate.get(), reinterpret_cast<sockaddr*>(&addr),
                   sizeof(addr)) < 0) {
            std::perror("IPv4 bind");
            return ListenerResult::Error;
        }
    } else {
        sockaddr_in6 addr{};
        addr.sin6_family = AF_INET6;
        addr.sin6_port = htons(static_cast<uint16_t>(port));
        addr.sin6_addr = in6addr_any;
        if (::bind(candidate.get(), reinterpret_cast<sockaddr*>(&addr),
                   sizeof(addr)) < 0) {
            std::perror("IPv6 bind");
            return ListenerResult::Error;
        }
    }

    if (::listen(candidate.get(), SOMAXCONN) < 0) {
        std::perror("listen");
        return ListenerResult::Error;
    }

    out = std::move(candidate);
    std::cout << "[INFO] " << label << " listener ready." << std::endl;
    return ListenerResult::Ready;
}

std::string cachedHeader(const CachedAsset& asset, bool keep_alive) {
    return asset.header_prefix +
           (keep_alive ? KEEPALIVE_HEADER : CLOSE_HEADER) + "\r\n\r\n";
}

TransferOutcome queuedFailureOutcome(TransferOutcome current) {
    if (current == TransferOutcome::ShutdownCancelled) {
        return TransferOutcome::ShutdownCancelled;
    }
    if (current == TransferOutcome::Disconnected) {
        return TransferOutcome::Disconnected;
    }
    return TransferOutcome::Failed;
}

}  // anonymous namespace

Server::Server(const Config& cfg)
    : cfg_(cfg),
      connection_limiter_(static_cast<std::size_t>(cfg.max_connections)),
      asset_cache_(cfg.static_dir,
                   AssetCacheLimits{
                       static_cast<std::size_t>(cfg.max_asset_bytes),
                       static_cast<std::size_t>(cfg.max_asset_cache_bytes)}),
      running_(false) {
    if (cfg.enable_logging) {
        logger_ = std::make_unique<Logger>(cfg);
    }
    if (!createWakePipe()) {
        std::cerr << "[WARNING] Could not create shutdown wake pipe; shutdown "
                     "will be observed on the next poll tick."
                  << std::endl;
    }
}

Server::~Server() = default;

void Server::stop() noexcept {
    const int saved_errno = errno;
    running_.store(false, std::memory_order_relaxed);
    if (wake_write_.valid()) {
        const char byte = 1;
        (void)::write(wake_write_.get(), &byte, sizeof(byte));
    }
    errno = saved_errno;
}

bool Server::createWakePipe() {
    int fds[2];
    if (::pipe(fds) != 0) return false;
    for (int fd : fds) {
        if (!makeNonBlocking(fd)) {
            ::close(fds[0]);
            ::close(fds[1]);
            return false;
        }
    }
    wake_read_ = UniqueFd(fds[0]);
    wake_write_ = UniqueFd(fds[1]);
    return true;
}

bool Server::setupSockets() {
    const ListenerResult ipv4 =
        setupListener(AF_INET, cfg_.port, sockd4_, "IPv4");
    if (ipv4 == ListenerResult::Error) return false;
    ipv4_enabled_ = ipv4 == ListenerResult::Ready;

    const ListenerResult ipv6 =
        setupListener(AF_INET6, cfg_.port, sockd6_, "IPv6");
    if (ipv6 == ListenerResult::Error) return false;
    ipv6_enabled_ = ipv6 == ListenerResult::Ready;

    if (!ipv4_enabled_ && !ipv6_enabled_) {
        std::cerr << "[ERROR] Neither IPv4 nor IPv6 is available on this host."
                  << std::endl;
        return false;
    }

    std::cout << "[INFO] Listening on ";
    if (ipv4_enabled_) std::cout << "IPv4";
    if (ipv4_enabled_ && ipv6_enabled_) std::cout << " and ";
    if (ipv6_enabled_) std::cout << "IPv6";
    std::cout << " port " << cfg_.port << "." << std::endl;
    return true;
}

void Server::beginRequest(Connection& connection) {
    if (connection.finished) return;
    if (!connection.request_in_progress) {
        connection.request_in_progress = true;
        connection.idle_deadline.reset();
        connection.header_deadline = Deadline::after(
            std::chrono::milliseconds(cfg_.header_timeout_ms));
    } else if (!connection.header_deadline) {
        connection.header_deadline = Deadline::after(
            std::chrono::milliseconds(cfg_.header_timeout_ms));
    }
}

void Server::markIdleIfReady(Connection& connection) {
    if (connection.finished || connection.request_in_progress ||
        !connection.responses.empty() || !connection.input.empty() ||
        connection.stop_reading || connection.input_closed) {
        return;
    }
    if (!connection.idle_deadline) {
        connection.idle_deadline = Deadline::after(
            std::chrono::milliseconds(cfg_.keepalive_timeout_ms));
    }
}

bool Server::canRead(const Connection& connection) const {
    if (connection.finished || connection.stop_reading ||
        connection.input_closed) {
        return false;
    }
    if (connection.responses.size() >=
        static_cast<std::size_t>(cfg_.max_pipelined_requests)) {
        return false;
    }
    return connection.input.size() <
           static_cast<std::size_t>(cfg_.max_connection_input_bytes);
}

short Server::pollEvents(const Connection& connection) const {
    short events = 0;
    if (canRead(connection)) events |= POLLIN;
    if (!connection.responses.empty()) events |= POLLOUT;
    return events;
}

void Server::queueResponse(Connection& connection, int status_code,
                           std::string method, std::string path,
                           std::string response_header,
                           std::shared_ptr<const std::string> response_body,
                           bool close_after, TransferOutcome outcome_override) {
    connection.responses.emplace_back(
        status_code, std::move(method), std::move(path),
        std::move(response_header), std::move(response_body),
        Deadline::after(std::chrono::milliseconds(cfg_.write_timeout_ms)),
        outcome_override);
    connection.idle_deadline.reset();

    if (close_after) {
        connection.stop_reading = true;
        connection.close_after_responses = true;
        connection.input.clear();
        connection.request_in_progress = false;
        connection.header_deadline.reset();
        connection.idle_deadline.reset();
    }
}

void Server::queueErrorResponse(Connection& connection, int status_code,
                                const char* status_line, const char* body,
                                TransferOutcome outcome_override) {
    const std::string response =
        constructResponse(status_line, "text/plain", body);
    queueResponse(connection, status_code, "-", "-", response, nullptr, true,
                  outcome_override);
}

void Server::processBufferedInput(Connection& connection) {
    if (connection.finished) return;

    const ParserLimits limits;
    const std::size_t max_input =
        static_cast<std::size_t>(cfg_.max_connection_input_bytes);

    for (int parsed = 0; parsed < MAX_PARSES_PER_ITERATION; ++parsed) {
        if (connection.finished) return;

        if (connection.input.empty()) {
            connection.request_in_progress = false;
            connection.header_deadline.reset();
            if (connection.responses.empty() &&
                (connection.stop_reading || connection.input_closed)) {
                abandonConnection(connection);
                return;
            }
            markIdleIfReady(connection);
            return;
        }

        if (connection.responses.size() >=
            static_cast<std::size_t>(cfg_.max_pipelined_requests)) {
            const ParseResult peek =
                parseRequestFromBuffer(connection.input, limits);
            if (peek.status == ParseStatus::NeedMore &&
                !connection.input_closed) {
                beginRequest(connection);
            }
            return;
        }

        if (!connection.request_in_progress) {
            const ParseResult peek =
                parseRequestFromBuffer(connection.input, limits);
            if (peek.status == ParseStatus::NeedMore &&
                !connection.input_closed) {
                beginRequest(connection);
            }
        }

        const ParseResult result =
            parseRequestFromBuffer(connection.input, limits);
        if (result.status == ParseStatus::NeedMore) {
            if (connection.input_closed) {
                connection.input.clear();
                connection.request_in_progress = false;
                connection.header_deadline.reset();
                connection.stop_reading = true;
                connection.close_after_responses = true;
                if (connection.responses.empty()) {
                    abandonConnection(connection);
                }
                return;
            }
            if (connection.input.size() >= max_input) {
                ++diagnostics_.parse_errors;
                queueErrorResponse(
                    connection, 431,
                    "431 Request Header Fields Too Large",
                    "Request Header Fields Too Large");
                return;
            }
            if (!connection.request_in_progress) beginRequest(connection);
            return;
        }

        if (result.status == ParseStatus::Error) {
            ++diagnostics_.parse_errors;
            const ParseErrorResponse error =
                parseErrorResponse(result.request.error);
            queueErrorResponse(connection, error.status_code,
                               error.status_line, error.body);
            return;
        }

        connection.input.erase(0, result.consumed_bytes);
        connection.request_in_progress = false;
        connection.header_deadline.reset();
        connection.idle_deadline.reset();
        ++connection.requests_started;

        const ParsedRequest& request = result.request;

        if (request.method != "GET") {
            const std::string response = constructResponse(
                "405 Method Not Allowed", "text/plain", "Method Not Allowed",
                "Allow: GET\r\n");
            queueResponse(connection, 405, request.method, request.path,
                          response, nullptr, true);
            return;
        }

        bool has_more = false;
        if (!connection.input.empty()) {
            const ParseResult next =
                parseRequestFromBuffer(connection.input, limits);
            has_more = next.status != ParseStatus::NeedMore;
        }

        const bool max_requests =
            connection.requests_started >=
            static_cast<std::size_t>(cfg_.max_requests_per_connection);
        if (!request.keep_alive) ++diagnostics_.requested_close;
        if (max_requests) ++diagnostics_.max_requests_closed;
        const bool close_after =
            !request.keep_alive || max_requests ||
            (connection.input_closed && !has_more);

        const CachedAsset& asset = asset_cache_.lookup(request.path);
        queueResponse(connection, asset.status_code, request.method,
                      request.path,
                      cachedHeader(asset, !close_after), asset.body,
                      close_after);

        if (close_after) return;

        if (connection.input.empty()) {
            connection.request_in_progress = false;
            connection.header_deadline.reset();
            return;
        }

        const ParseResult next =
            parseRequestFromBuffer(connection.input, limits);
        if (next.status == ParseStatus::NeedMore && !connection.input_closed) {
            beginRequest(connection);
        } else {
            connection.request_in_progress = false;
            connection.header_deadline.reset();
        }

        if (connection.responses.size() >=
            static_cast<std::size_t>(cfg_.max_pipelined_requests)) {
            return;
        }
    }
}

void Server::acceptConnections(int listener_fd) {
    for (int accepted = 0;
         accepted < MAX_ACCEPTS_PER_ITERATION && running_.load();
         ++accepted) {
        sockaddr_storage peer{};
        socklen_t peer_length = sizeof(peer);
        const int fd = ::accept(listener_fd, reinterpret_cast<sockaddr*>(&peer),
                                &peer_length);
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == ECONNABORTED) continue;
            std::perror("accept");
            break;
        }

        UniqueFd client_socket(fd);
        if (!makeNonBlocking(client_socket.get())) {
            std::perror("accept nonblocking");
            continue;
        }

        auto permit = connection_limiter_.tryAcquire();
        if (!permit) {
            ++diagnostics_.admission_rejected;
            sendOverloadResponse(client_socket.get());
            continue;
        }
        ++diagnostics_.accepted;

        const int accepted_fd = client_socket.get();
        auto cancelled = std::make_shared<std::atomic<bool>>(false);
        auto registration = active_connections_.add(accepted_fd, cancelled);
        auto connection = std::make_unique<Connection>(
            std::move(client_socket), std::move(*permit),
            std::move(registration), std::move(cancelled),
            addrToString(peer));
        connection->input.reserve(4096);
        beginRequest(*connection);
        connections_.push_back(std::move(connection));
    }
}

void Server::handleReadable(Connection& connection) {
    if (connection.finished) return;
    if (!canRead(connection)) {
        processBufferedInput(connection);
        return;
    }

    if (!connection.request_in_progress) beginRequest(connection);

    const std::size_t max_input =
        static_cast<std::size_t>(cfg_.max_connection_input_bytes);
    if (connection.input.size() >= max_input) {
        processBufferedInput(connection);
        if (!connection.finished && connection.input.size() >= max_input) {
            ++diagnostics_.parse_errors;
            queueErrorResponse(connection, 431,
                               "431 Request Header Fields Too Large",
                               "Request Header Fields Too Large");
        }
        return;
    }

    const std::size_t capacity = std::min(
        READ_CHUNK, max_input - connection.input.size());
    char buffer[READ_CHUNK];
    ssize_t received = 0;
    do {
        received = ::recv(connection.socket.get(), buffer, capacity,
                          MSG_DONTWAIT);
    } while (received < 0 && errno == EINTR);

    if (received > 0) {
        connection.input.append(buffer, static_cast<std::size_t>(received));
        processBufferedInput(connection);
        return;
    }

    if (received == 0) {
        ++diagnostics_.peer_eof;
        connection.input_closed = true;
        connection.stop_reading = true;
        connection.close_after_responses = true;
        connection.idle_deadline.reset();
        processBufferedInput(connection);

        if (connection.finished) return;
        if (connection.input.empty() && !connection.responses.empty()) {
            markLastQueuedResponseClose(connection);
        }
        if (connection.input.empty() && connection.responses.empty()) {
            abandonConnection(connection);
        }
        return;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) return;
    const TransferFailure failure = socketErrnoFailure();
    if (failure == TransferFailure::Disconnected) {
        ++diagnostics_.peer_resets;
    } else {
        ++diagnostics_.read_errors;
    }
    failConnection(connection,
                   classifyTransferOutcome(connection.responses.empty()
                                               ? 0
                                               : connection.responses.front()
                                                     .bytes_sent,
                                           failure,
                                           cancelledNow(connection)));
}

void Server::handleWritable(Connection& connection) {
    if (connection.finished || connection.responses.empty()) return;
    if (cancelledNow(connection)) {
        failConnection(connection, TransferOutcome::ShutdownCancelled);
        return;
    }

    PendingResponse& response = connection.responses.front();
    if (response.write_deadline && response.write_deadline->expired()) {
        failConnection(connection,
                       cancelledNow(connection)
                           ? TransferOutcome::ShutdownCancelled
                           : TransferOutcome::Timeout);
        return;
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        const char* data = nullptr;
        std::size_t length = 0;
        std::size_t* offset = nullptr;

        if (response.header_offset < response.header.size()) {
            data = response.header.data();
            length = response.header.size();
            offset = &response.header_offset;
        } else if (response.body &&
                   response.body_offset < response.body->size()) {
            data = response.body->data();
            length = response.body->size();
            offset = &response.body_offset;
        } else {
            completeResponse(connection);
            return;
        }

        ssize_t sent = 0;
        do {
            sent = ::send(connection.socket.get(), data + *offset,
                          length - *offset, MSG_DONTWAIT);
        } while (sent < 0 && errno == EINTR);

        if (sent > 0) {
            *offset += static_cast<std::size_t>(sent);
            response.bytes_sent += static_cast<std::size_t>(sent);
            if (*offset < length) return;
            continue;
        }
        if (sent == 0) {
            failConnection(connection, TransferOutcome::Disconnected);
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;

        const TransferFailure failure = socketErrnoFailure();
        if (failure == TransferFailure::Disconnected) {
            ++diagnostics_.peer_resets;
        } else {
            ++diagnostics_.write_errors;
        }
        const TransferOutcome outcome =
            response.outcome_override != TransferOutcome::Unknown
                ? response.outcome_override
                : classifyTransferOutcome(response.bytes_sent, failure,
                                          cancelledNow(connection));
        failConnection(connection, outcome);
        return;
    }
}

void Server::completeResponse(Connection& connection) {
    if (connection.responses.empty()) return;

    PendingResponse& response = connection.responses.front();
    ++diagnostics_.completed_responses;
    const TransferOutcome outcome =
        response.outcome_override != TransferOutcome::Unknown
            ? response.outcome_override
            : TransferOutcome::Complete;
    logResponse(connection, response, outcome);
    connection.responses.pop_front();

    if (!connection.responses.empty()) return;

    if (!connection.input.empty()) {
        processBufferedInput(connection);
        if (connection.finished || !connection.responses.empty()) return;
    }

    if (connection.close_after_responses || connection.stop_reading ||
        connection.input_closed) {
        connection.finished = true;
        return;
    }

    if (!connection.request_in_progress && connection.input.empty()) {
        markIdleIfReady(connection);
    }
}

void Server::logResponse(const Connection& connection,
                         const PendingResponse& response,
                         TransferOutcome outcome) {
    if (!logger_) return;
    logger_->logAccess(response.method, response.path, response.status_code,
                       response.bytes_sent, outcome, connection.peer_ip);
}

void Server::markLastQueuedResponseClose(Connection& connection) {
    if (connection.responses.empty()) return;
    PendingResponse& last = connection.responses.back();
    if (last.header_offset != 0) return;
    const std::size_t position = last.header.find(KEEPALIVE_HEADER);
    if (position != std::string::npos) {
        last.header.replace(position, std::strlen(KEEPALIVE_HEADER),
                            CLOSE_HEADER);
    }
}

void Server::failConnection(Connection& connection, TransferOutcome outcome) {
    if (connection.finished) return;
    connection.finished = true;

    if (connection.responses.empty()) {
        connection.input.clear();
        connection.request_in_progress = false;
        connection.header_deadline.reset();
        connection.idle_deadline.reset();
        return;
    }

    bool first = true;
    for (const PendingResponse& response : connection.responses) {
        TransferOutcome response_outcome =
            first ? outcome : queuedFailureOutcome(outcome);
        if (first && outcome != TransferOutcome::ShutdownCancelled &&
            response.outcome_override != TransferOutcome::Unknown) {
            response_outcome = response.outcome_override;
        }
        logResponse(connection, response, response_outcome);
        first = false;
    }
    connection.responses.clear();
    connection.input.clear();
    connection.request_in_progress = false;
    connection.header_deadline.reset();
    connection.idle_deadline.reset();
}

void Server::abandonConnection(Connection& connection) {
    if (connection.finished) return;
    connection.finished = true;
    connection.responses.clear();
    connection.input.clear();
    connection.request_in_progress = false;
    connection.header_deadline.reset();
    connection.idle_deadline.reset();
}

void Server::pruneFinishedConnections() {
    std::size_t kept = 0;
    for (std::size_t i = 0; i < connections_.size(); ++i) {
        if (connections_[i]->finished) continue;
        if (kept != i) connections_[kept] = std::move(connections_[i]);
        ++kept;
    }
    connections_.resize(kept);
}

void Server::beginShutdownDrain() {
    draining_ = true;
    shutdown_deadline_ = Deadline::after(
        std::chrono::milliseconds(cfg_.shutdown_grace_ms));

    for (auto& connection : connections_) {
        if (connection->finished) continue;
        if (connection->responses.empty()) {
            abandonConnection(*connection);
            continue;
        }
        connection->input.clear();
        connection->request_in_progress = false;
        connection->header_deadline.reset();
        connection->idle_deadline.reset();
        connection->stop_reading = true;
        connection->input_closed = true;
        connection->close_after_responses = true;
        markLastQueuedResponseClose(*connection);
    }
    pruneFinishedConnections();

    std::cout << "[INFO] Shutdown requested; draining "
              << connections_.size() << " connection(s)." << std::endl;
}

void Server::forceCloseAllConnections() {
    active_connections_.cancelAll();
    for (auto& connection : connections_) {
        if (!connection->finished) {
            ++diagnostics_.shutdown_closed;
            failConnection(*connection, TransferOutcome::ShutdownCancelled);
        }
    }
    connections_.clear();
    shutdown_deadline_.reset();
}

int Server::pollTimeoutMilliseconds() const {
    int timeout = POLL_TIMEOUT_MS;
    auto consider = [&timeout](const Deadline& deadline) {
        timeout = std::min(timeout, deadline.remainingMilliseconds());
    };

    if (draining_ && shutdown_deadline_) consider(*shutdown_deadline_);
    for (const auto& connection : connections_) {
        if (connection->finished) continue;
        if (connection->request_in_progress && connection->header_deadline) {
            consider(*connection->header_deadline);
        }
        if (!connection->responses.empty() &&
            connection->responses.front().write_deadline) {
            consider(*connection->responses.front().write_deadline);
        }
        if (connection->idle_deadline) consider(*connection->idle_deadline);
    }
    return std::max(0, timeout);
}

void Server::emitDiagnostics(bool final_report) {
    const LoggerMetrics metrics = logger_ ? logger_->metrics() : LoggerMetrics{};
    const double avg_flush_ms =
        metrics.batches == 0
            ? 0.0
            : static_cast<double>(metrics.total_flush_us) /
                  static_cast<double>(metrics.batches) / 1000.0;
    const std::uint64_t committed_delta =
        metrics.committed >= last_logger_committed_
            ? metrics.committed - last_logger_committed_
            : metrics.committed;

    std::cout << "[DIAG] final=" << (final_report ? 1 : 0)
              << " accepted=" << diagnostics_.accepted
              << " admission_rejected=" << diagnostics_.admission_rejected
              << " responses=" << diagnostics_.completed_responses
              << " header_timeouts=" << diagnostics_.header_timeouts
              << " write_timeouts=" << diagnostics_.write_timeouts
              << " peer_eof=" << diagnostics_.peer_eof
              << " peer_resets=" << diagnostics_.peer_resets
              << " read_errors=" << diagnostics_.read_errors
              << " write_errors=" << diagnostics_.write_errors
              << " parse_errors=" << diagnostics_.parse_errors
              << " requested_close=" << diagnostics_.requested_close
              << " max_requests=" << diagnostics_.max_requests_closed
              << " idle_timeouts=" << diagnostics_.idle_timeouts
              << " shutdown_closed=" << diagnostics_.shutdown_closed
              << " poll_errors=" << diagnostics_.poll_errors
              << " loops=" << diagnostics_.event_loops
              << " loop_max_ms="
              << static_cast<double>(diagnostics_.interval_max_loop_work_us) /
                     1000.0
              << " logger_queue=" << metrics.queue_depth
              << " logger_peak_queue=" << metrics.peak_queue_depth
              << " logger_committed=" << metrics.committed
              << " logger_commit_delta=" << committed_delta
              << " logger_dropped=" << metrics.dropped
              << " logger_failed=" << metrics.failed
              << " logger_discarded=" << metrics.discarded
              << " logger_avg_flush_ms=" << avg_flush_ms
              << " logger_max_flush_ms="
              << static_cast<double>(metrics.max_flush_us) / 1000.0
              << std::endl;

    last_logger_committed_ = metrics.committed;
    diagnostics_.interval_max_loop_work_us = 0;
    last_diagnostics_ = std::chrono::steady_clock::now();
}

void Server::eventLoop() {
    std::cout << "[INFO] Event loop started." << std::endl;
    last_diagnostics_ = std::chrono::steady_clock::now();
    std::vector<pollfd> pfds;
    std::vector<Connection*> polled;

    while (true) {
        const auto loop_started = std::chrono::steady_clock::now();
        ++diagnostics_.event_loops;
        if (!running_.load(std::memory_order_relaxed) && !draining_) {
            beginShutdownDrain();
        }
        if (draining_ && connections_.empty()) break;
        if (draining_ && shutdown_deadline_ &&
            shutdown_deadline_->expired()) {
            forceCloseAllConnections();
            break;
        }

        for (auto& connection : connections_) {
            if (!connection->finished && !connection->input.empty()) {
                processBufferedInput(*connection);
            }
        }
        for (auto& connection : connections_) {
            if (connection->finished) continue;
            if (connection->input.empty() && connection->responses.empty() &&
                (connection->stop_reading || connection->input_closed)) {
                abandonConnection(*connection);
                continue;
            }
            if (connection->input.empty() && connection->responses.empty() &&
                !connection->request_in_progress) {
                markIdleIfReady(*connection);
            }
        }
        pruneFinishedConnections();
        if (draining_ && connections_.empty()) break;

        pfds.clear();
        polled.clear();

        int listener_fds[2] = {-1, -1};
        std::size_t listener_count = 0;
        if (running_.load()) {
            if (ipv4_enabled_ && sockd4_.valid()) {
                listener_fds[listener_count++] = sockd4_.get();
                pfds.push_back({sockd4_.get(), POLLIN, 0});
            }
            if (ipv6_enabled_ && sockd6_.valid()) {
                listener_fds[listener_count++] = sockd6_.get();
                pfds.push_back({sockd6_.get(), POLLIN, 0});
            }
        }

        const std::size_t wake_index = pfds.size();
        const bool wake_added = wake_read_.valid();
        if (wake_added) {
            pfds.push_back({wake_read_.get(), POLLIN, 0});
        }

        const std::size_t connection_start = pfds.size();
        for (auto& connection : connections_) {
            pfds.push_back({connection->socket.get(),
                            pollEvents(*connection), 0});
            polled.push_back(connection.get());
        }

        const auto before_poll = std::chrono::steady_clock::now();
        const int timeout = pollTimeoutMilliseconds();
        const int result = ::poll(
            pfds.data(), static_cast<nfds_t>(pfds.size()), timeout);
        const auto after_poll = std::chrono::steady_clock::now();
        if (result < 0) {
            if (errno == EINTR) continue;
            ++diagnostics_.poll_errors;
            std::perror("poll");
            forceCloseAllConnections();
            break;
        }

        if (result > 0) {
            if (wake_added && (pfds[wake_index].revents & POLLIN)) {
                char drain[64];
                while (::read(wake_read_.get(), drain, sizeof(drain)) > 0) {
                }
            }

            if (running_.load()) {
                for (std::size_t i = 0; i < listener_count; ++i) {
                    if (pfds[i].revents & (POLLIN | POLLERR)) {
                        acceptConnections(listener_fds[i]);
                    }
                }
            }

            for (std::size_t i = 0; i < polled.size(); ++i) {
                const short revents = pfds[connection_start + i].revents;
                if (revents == 0) continue;
                Connection& connection = *polled[i];
                if (connection.finished) continue;

                if (revents & POLLNVAL) {
                    failConnection(connection, TransferOutcome::Failed);
                    continue;
                }

                if (canRead(connection) &&
                    (revents & (POLLIN | POLLHUP | POLLERR))) {
                    handleReadable(connection);
                }
                if (connection.finished) continue;

                if (!connection.responses.empty() &&
                    (revents & (POLLIN | POLLOUT | POLLHUP | POLLERR))) {
                    handleWritable(connection);
                }
                if (connection.finished) continue;

                if (!connection.input.empty()) {
                    processBufferedInput(connection);
                }
            }
        }

        for (auto& connection : connections_) {
            if (connection->finished) continue;

            if (connection->request_in_progress &&
                connection->header_deadline &&
                connection->header_deadline->expired()) {
                ++diagnostics_.header_timeouts;
                queueErrorResponse(*connection, 408,
                                   "408 Request Timeout",
                                   "Request timeout",
                                   TransferOutcome::Timeout);
                continue;
            }

            if (!connection->responses.empty() &&
                connection->responses.front().write_deadline &&
                connection->responses.front().write_deadline->expired()) {
                ++diagnostics_.write_timeouts;
                failConnection(
                    *connection,
                    cancelledNow(*connection)
                        ? TransferOutcome::ShutdownCancelled
                        : TransferOutcome::Timeout);
                continue;
            }

            if (connection->idle_deadline &&
                connection->idle_deadline->expired() &&
                !connection->request_in_progress &&
                connection->responses.empty() &&
                connection->input.empty() &&
                !connection->input_closed &&
                !connection->stop_reading) {
                ++diagnostics_.idle_timeouts;
                abandonConnection(*connection);
            }
        }

        pruneFinishedConnections();

        const auto loop_ended = std::chrono::steady_clock::now();
        const auto work_us =
            std::chrono::duration_cast<std::chrono::microseconds>(
                before_poll - loop_started).count() +
            std::chrono::duration_cast<std::chrono::microseconds>(
                loop_ended - after_poll).count();
        if (work_us > 0) {
            diagnostics_.interval_max_loop_work_us = std::max(
                diagnostics_.interval_max_loop_work_us,
                static_cast<std::uint64_t>(work_us));
        }

        if (cfg_.diagnostics_interval_ms > 0 &&
            std::chrono::steady_clock::now() - last_diagnostics_ >=
                std::chrono::milliseconds(cfg_.diagnostics_interval_ms)) {
            emitDiagnostics(false);
        }
    }

    if (!connections_.empty()) {
        forceCloseAllConnections();
    }
    if (logger_) logger_->shutdown();
    if (cfg_.diagnostics_interval_ms > 0) emitDiagnostics(true);
    std::cout << "[INFO] Event loop stopped." << std::endl;
}

int Server::run() {
    if (!setupSockets()) return 1;
    running_.store(true);
    eventLoop();
    return 0;
}

}  // namespace http_server

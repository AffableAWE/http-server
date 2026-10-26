#ifndef HTTP_SERVER_CONNECTION_HPP
#define HTTP_SERVER_CONNECTION_HPP

#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "http_server/active_connections.hpp"
#include "http_server/connection_limiter.hpp"
#include "http_server/deadline.hpp"
#include "http_server/transfer_outcome.hpp"
#include "http_server/unique_fd.hpp"

namespace http_server {

/*
 * One queued HTTP response. Header text is small; response bodies are shared
 * from the immutable cache (or a bounded error response) and are not copied
 * per request.
 */
struct PendingResponse {
    int status_code = 0;
    std::string method = "-";
    std::string path = "-";
    std::string header;
    std::shared_ptr<const std::string> body;
    std::size_t header_offset = 0;
    std::size_t body_offset = 0;
    std::size_t bytes_sent = 0;
    std::optional<Deadline> write_deadline;
    TransferOutcome outcome_override = TransferOutcome::Unknown;
    bool logged = false;

    PendingResponse() = default;

    PendingResponse(int status, std::string request_method,
                    std::string request_path, std::string response_header,
                    std::shared_ptr<const std::string> response_body,
                    Deadline deadline,
                    TransferOutcome forced_outcome = TransferOutcome::Unknown)
        : status_code(status),
          method(std::move(request_method)),
          path(std::move(request_path)),
          header(std::move(response_header)),
          body(std::move(response_body)),
          write_deadline(deadline),
          outcome_override(forced_outcome) {}

    PendingResponse(PendingResponse&&) noexcept = default;
    PendingResponse& operator=(PendingResponse&&) noexcept = default;
    PendingResponse(const PendingResponse&) = delete;
    PendingResponse& operator=(const PendingResponse&) = delete;
};

/*
 * One admitted persistent connection owned exclusively by the event loop.
 *
 * Descriptor, admission, and cancellation ownership live here until the event
 * loop erases the connection. Registration is destroyed before the descriptor.
 * Input is bounded by the server's per-connection limit. Responses are queued
 * FIFO so pipelined request ordering is preserved.
 */
struct Connection {
    UniqueFd socket;
    ConnectionLimiter::Permit permit;
    ActiveConnections::Registration registration;
    ActiveConnections::CancellationFlag cancelled;

    std::string peer_ip;
    std::string input;
    std::deque<PendingResponse> responses;

    std::optional<Deadline> header_deadline;
    std::optional<Deadline> idle_deadline;
    std::size_t requests_started = 0;

    bool request_in_progress = false;
    bool input_closed = false;
    bool stop_reading = false;
    bool close_after_responses = false;
    bool finished = false;

    Connection(UniqueFd accepted_socket, ConnectionLimiter::Permit admission,
               ActiveConnections::Registration registry_registration,
               ActiveConnections::CancellationFlag cancellation_flag,
               std::string remote_ip)
        : socket(std::move(accepted_socket)),
          permit(std::move(admission)),
          registration(std::move(registry_registration)),
          cancelled(std::move(cancellation_flag)),
          peer_ip(std::move(remote_ip)) {}

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
};

}  // namespace http_server

#endif  // HTTP_SERVER_CONNECTION_HPP

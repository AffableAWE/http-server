#ifndef HTTP_SERVER_CONNECTION_TASK_HPP
#define HTTP_SERVER_CONNECTION_TASK_HPP

#include "http_server/active_connections.hpp"
#include "http_server/connection_limiter.hpp"
#include "http_server/deadline.hpp"
#include "http_server/unique_fd.hpp"

namespace http_server {

/*
 * One admitted connection moving from the accept loop to a worker.
 * The task owns both the descriptor and the admission permit. Destroying a
 * queued task closes the descriptor and releases the slot exactly once, even
 * if shutdown discards the task before any handler runs.
 *
 * header_deadline is created at acceptance, so queue wait consumes the request
 * budget instead of granting a fresh one when a worker starts.
 */
struct ConnectionTask {
    UniqueFd socket;
    ConnectionLimiter::Permit permit;
    Deadline header_deadline;
    ActiveConnections::CancellationFlag cancelled;
    // Destroyed first, so the registry no longer references the fd before the
    // socket owner closes it.
    ActiveConnections::Registration registration;

    ConnectionTask(ConnectionTask&&) noexcept = default;
    // Move assignment would have to replace the registry token before closing
    // the destination socket. No current use needs it; deleting it prevents a
    // future ordering mistake.
    ConnectionTask& operator=(ConnectionTask&&) = delete;
    ConnectionTask(const ConnectionTask&) = delete;
    ConnectionTask& operator=(const ConnectionTask&) = delete;
};

}  // namespace http_server

#endif  // HTTP_SERVER_CONNECTION_TASK_HPP

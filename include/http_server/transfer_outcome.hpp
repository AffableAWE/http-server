#ifndef HTTP_SERVER_TRANSFER_OUTCOME_HPP
#define HTTP_SERVER_TRANSFER_OUTCOME_HPP

#include <cstddef>

namespace http_server {

/*
 * How the response transfer ended. status_code remains the intended HTTP
 * status; this value describes only what happened while writing the response.
 *
 * Unknown is used only for historical database rows or records where no
 * response was attempted.
 */
enum class TransferOutcome {
    Unknown,
    Complete,
    Partial,
    Disconnected,
    Timeout,
    ShutdownCancelled,
    Failed,
};

enum class TransferFailure {
    None,
    Timeout,
    Disconnected,
    Error,
};

/*
 * Classifies a response write. Complete writes win; cancellation is checked
 * before specific failures; an incomplete non-specific error is Partial when
 * some bytes were accepted and Failed when none were.
 */
inline TransferOutcome classifyTransferOutcome(std::size_t bytes_written,
                                               TransferFailure failure,
                                               bool cancelled) {
    if (failure == TransferFailure::None) return TransferOutcome::Complete;
    if (cancelled) return TransferOutcome::ShutdownCancelled;
    switch (failure) {
        case TransferFailure::Timeout:
            return TransferOutcome::Timeout;
        case TransferFailure::Disconnected:
            return TransferOutcome::Disconnected;
        case TransferFailure::Error:
            return bytes_written > 0 ? TransferOutcome::Partial
                                     : TransferOutcome::Failed;
        case TransferFailure::None:
            return TransferOutcome::Complete;
    }
    return TransferOutcome::Failed;
}

inline const char* transferOutcomeName(TransferOutcome outcome) {
    switch (outcome) {
        case TransferOutcome::Unknown:
            return "unknown";
        case TransferOutcome::Complete:
            return "complete";
        case TransferOutcome::Partial:
            return "partial";
        case TransferOutcome::Disconnected:
            return "disconnected";
        case TransferOutcome::Timeout:
            return "timeout";
        case TransferOutcome::ShutdownCancelled:
            return "shutdown_cancelled";
        case TransferOutcome::Failed:
            return "failed";
    }
    return "unknown";
}

}  // namespace http_server

#endif  // HTTP_SERVER_TRANSFER_OUTCOME_HPP

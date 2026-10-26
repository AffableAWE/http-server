#include "http_server/connection_limiter.hpp"
#include "http_server/connection_task.hpp"
#include "http_server/deadline.hpp"
#include "http_server/thread_pool.hpp"
#include "http_server/transfer_outcome.hpp"
#include "http_server/unique_fd.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <type_traits>

namespace {

using namespace std::chrono_literals;

static_assert(std::is_move_constructible<http_server::ConnectionTask>::value,
              "ConnectionTask must be move-constructible");
static_assert(
    !std::is_move_assignable<http_server::ConnectionTask>::value,
    "ConnectionTask move assignment is intentionally deleted");

int failures = 0;

void check(bool condition, const char* expression, int line) {
    if (condition) return;
    std::cerr << "FAIL line " << line << ": " << expression << "\n";
    ++failures;
}

#define CHECK(expression) check((expression), #expression, __LINE__)

void testTransferOutcomeClassification() {
    using http_server::TransferFailure;
    using http_server::TransferOutcome;
    CHECK(http_server::classifyTransferOutcome(
              0, TransferFailure::None, false) == TransferOutcome::Complete);
    CHECK(http_server::classifyTransferOutcome(
              10, TransferFailure::Error, false) ==
          TransferOutcome::Partial);
    // Response headers were accepted, then the body send failed generically
    // with zero body bytes accepted. The whole-response byte count matters.
    CHECK(http_server::classifyTransferOutcome(
              88, TransferFailure::Error, false) ==
          TransferOutcome::Partial);
    CHECK(http_server::classifyTransferOutcome(
              88, TransferFailure::Timeout, false) ==
          TransferOutcome::Timeout);
    CHECK(http_server::classifyTransferOutcome(
              88, TransferFailure::Disconnected, false) ==
          TransferOutcome::Disconnected);
    CHECK(http_server::classifyTransferOutcome(
              0, TransferFailure::Error, false) == TransferOutcome::Failed);
    CHECK(http_server::classifyTransferOutcome(
              10, TransferFailure::Disconnected, false) ==
          TransferOutcome::Disconnected);
    CHECK(http_server::classifyTransferOutcome(
              10, TransferFailure::Timeout, false) ==
          TransferOutcome::Timeout);
    CHECK(http_server::classifyTransferOutcome(
              10, TransferFailure::Error, true) ==
          TransferOutcome::ShutdownCancelled);
}

void testUniqueFdOwnership() {
    int fds[2];
    CHECK(::pipe(fds) == 0);

    const int read_fd = fds[0];
    {
        http_server::UniqueFd first(read_fd);
        CHECK(first.valid());

        http_server::UniqueFd second(std::move(first));
        CHECK(!first.valid());
        CHECK(second.get() == read_fd);

        http_server::UniqueFd third;
        third = std::move(second);
        CHECK(!second.valid());
        CHECK(third.get() == read_fd);
    }

    CHECK(::fcntl(read_fd, F_GETFD) == -1);
    CHECK(::close(fds[1]) == 0);
}

http_server::ConnectionTask makeTask(http_server::ConnectionLimiter& limiter,
                                     http_server::ActiveConnections& active,
                                     int fd) {
    auto permit = limiter.tryAcquire();
    CHECK(permit.has_value());
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    auto registration = active.add(fd, cancelled);
    return http_server::ConnectionTask{
        http_server::UniqueFd(fd), std::move(*permit),
        http_server::Deadline::after(1000ms), std::move(cancelled),
        std::move(registration)};
}

void testConnectionLimiter() {
    http_server::ConnectionLimiter limiter(2);
    CHECK(limiter.active() == 0);

    auto first = limiter.tryAcquire();
    auto second = limiter.tryAcquire();
    auto third = limiter.tryAcquire();
    CHECK(first.has_value());
    CHECK(second.has_value());
    CHECK(!third.has_value());
    CHECK(limiter.active() == 2);

    first->reset();
    CHECK(limiter.active() == 1);
    auto replacement = limiter.tryAcquire();
    CHECK(replacement.has_value());
    CHECK(limiter.active() == 2);

    second->reset();
    replacement->reset();
    CHECK(limiter.active() == 0);
    first->reset();  // Already reset; must not underflow.
    CHECK(limiter.active() == 0);
}

void testBoundedThreadPoolAndQueueDiscard() {
    int fds[2];
    CHECK(::pipe(fds) == 0);

    std::atomic<bool> started{false};
    std::atomic<bool> release{false};
    http_server::ConnectionLimiter limiter(8);
    http_server::ActiveConnections active;
    http_server::ThreadPool pool(
        1, 2,
        [&started, &release](http_server::ConnectionTask task) {
            (void)task;
            started.store(true);
            while (!release.load()) {
                std::this_thread::sleep_for(1ms);
            }
        });

    auto first = makeTask(limiter, active, ::dup(fds[0]));
    CHECK(pool.submit(first));
    CHECK(!first.socket.valid());

    for (int i = 0; i < 500 && !started.load(); ++i) {
        std::this_thread::sleep_for(1ms);
    }
    CHECK(started.load());

    const int second_fd = ::dup(fds[0]);
    const int third_fd = ::dup(fds[0]);
    auto second = makeTask(limiter, active, second_fd);
    auto third = makeTask(limiter, active, third_fd);
    CHECK(pool.submit(second));
    CHECK(pool.submit(third));
    CHECK(!second.socket.valid());
    CHECK(!third.socket.valid());

    auto fourth = makeTask(limiter, active, ::dup(fds[0]));
    CHECK(!pool.submit(fourth));
    CHECK(fourth.socket.valid());
    CHECK(limiter.active() == 4);

    // Queued tasks are discarded, releasing their permits. The active handler
    // still owns one slot, and the rejected fourth task owns another.
    pool.beginShutdown();
    CHECK(limiter.active() == 2);
    CHECK(::fcntl(second_fd, F_GETFD) == -1);
    CHECK(::fcntl(third_fd, F_GETFD) == -1);
    CHECK(!pool.waitForIdle(http_server::Deadline::after(10ms)));

    release.store(true);
    CHECK(pool.waitForIdle(http_server::Deadline::after(1000ms)));
    pool.join();
    CHECK(limiter.active() == 1);  // fourth task is still in scope.

    auto after_shutdown = makeTask(limiter, active, ::dup(fds[0]));
    CHECK(!pool.submit(after_shutdown));
    CHECK(after_shutdown.socket.valid());
    CHECK(limiter.active() == 2);

    CHECK(::close(fds[0]) == 0);
    CHECK(::close(fds[1]) == 0);
}

}  // namespace

int main() {
    testTransferOutcomeClassification();
    testUniqueFdOwnership();
    testConnectionLimiter();
    testBoundedThreadPoolAndQueueDiscard();

    if (failures != 0) {
        std::cerr << failures << " resource test(s) failed\n";
        return 1;
    }
    std::cout << "all resource tests passed\n";
    return 0;
}

#ifndef HTTP_SERVER_THREAD_POOL_HPP
#define HTTP_SERVER_THREAD_POOL_HPP

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "http_server/connection_task.hpp"
#include "http_server/deadline.hpp"

namespace http_server {

/*
 * Fixed-size pool of worker threads pulling owned connection tasks from a
 * bounded queue. Rejected submissions leave ownership with the caller.
 *
 * Shutdown is split into beginShutdown(), waitForIdle(), and join() so the
 * server can apply one global grace deadline and cancel active sockets before
 * joining. beginShutdown() discards queued tasks; their ConnectionTask
 * destructors close descriptors and release admission permits.
 */
class ThreadPool {
   public:
    using Handler = std::function<void(ConnectionTask)>;

    /*
     * Spawns num_threads worker threads.
     * Input:  num_threads - Worker count. If 0, falls back to
     * hardware_concurrency(). max_queue   - Maximum tasks waiting for a worker;
     * must be positive. handler     - Called once per admitted connection.
     */
    ThreadPool(unsigned num_threads, std::size_t max_queue, Handler handler);
    ~ThreadPool();

    // Non-copyable
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    /*
     * Attempts to enqueue task. On success ownership moves into the queue and
     * task.socket becomes invalid. On rejection (queue full or shutdown)
     * ownership remains with the caller and false is returned.
     */
    bool submit(ConnectionTask& task);

    /*
     * Stops accepting submissions and discards queued tasks. Active handlers
     * are allowed to continue. Idempotent.
     */
    void beginShutdown();

    /*
     * Waits until no handler is active or the deadline expires.
     * Returns true when idle, false on deadline expiry.
     */
    bool waitForIdle(const Deadline& deadline);

    /*
     * Joins all worker threads. Call only after beginShutdown().
     * Safe to call more than once.
     */
    void join();

    /*
     * Convenience shutdown without a grace period: beginShutdown() + join().
     * Queued tasks are discarded; active handlers must already be finished or
     * otherwise unblocked.
     */
    void shutdown();

   private:
    void worker_loop();

    Handler handler_;
    std::size_t max_queue_;
    std::vector<std::thread> workers_;
    std::queue<ConnectionTask> queue_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::condition_variable idle_cv_;
    std::atomic<bool> running_;
    std::size_t active_handlers_ = 0;
};

}  // namespace http_server

#endif  // HTTP_SERVER_THREAD_POOL_HPP

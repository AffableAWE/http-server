#ifndef HTTP_SERVER_THREAD_POOL_HPP
#define HTTP_SERVER_THREAD_POOL_HPP

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace http_server {

/*
 * Fixed-size pool of worker threads pulling client file descriptors from
 * a shared queue. The caller supplies a handler function which the workers
 * invoke for each fd they pick up.
 */
class ThreadPool {
   public:
    using Handler = std::function<void(int /*client_fd*/)>;

    /*
     * Spawns num_threads worker threads.
     * Input:  num_threads - Worker count. If 0, falls back to
     * hardware_concurrency(). handler     - Function called once per accepted
     * client fd.
     */
    ThreadPool(unsigned num_threads, Handler handler);
    ~ThreadPool();

    // Non-copyable
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    /*
     * Pushes a client fd into the queue and wakes one worker.
     */
    void submit(int client_fd);

    /*
     * Signals all workers to stop and joins them.
     * Safe to call multiple times.
     */
    void shutdown();

   private:
    void worker_loop();

    Handler handler_;
    std::vector<std::thread> workers_;
    std::queue<int> queue_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::atomic<bool> running_;
};

}  // namespace http_server

#endif  // HTTP_SERVER_THREAD_POOL_HPP

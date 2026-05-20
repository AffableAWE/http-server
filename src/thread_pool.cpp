#include "http_server/thread_pool.hpp"

#include <unistd.h>  // close()

namespace http_server {

ThreadPool::ThreadPool(unsigned num_threads, Handler handler)
    : handler_(std::move(handler)), running_(true) {
    if (num_threads == 0) {
        num_threads = std::thread::hardware_concurrency();
        if (num_threads == 0) num_threads = 2;  // fallback floor
    }
    workers_.reserve(num_threads);
    for (unsigned i = 0; i < num_threads; ++i) {
        workers_.emplace_back([this] { worker_loop(); });
    }
}

ThreadPool::~ThreadPool() { shutdown(); }

void ThreadPool::submit(int client_fd) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        queue_.push(client_fd);
    }
    cv_.notify_one();
}

void ThreadPool::shutdown() {
    bool was_running = running_.exchange(false);
    if (!was_running) return;  // already shut down

    cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }

    // Drain any leftover fds so we don't leak them.
    std::lock_guard<std::mutex> lock(mtx_);
    while (!queue_.empty()) {
        ::close(queue_.front());
        queue_.pop();
    }
}

void ThreadPool::worker_loop() {
    while (true) {
        int client_fd = -1;
        {
            std::unique_lock<std::mutex> lock(mtx_);
            cv_.wait(lock,
                     [this] { return !queue_.empty() || !running_.load(); });

            if (!running_.load() && queue_.empty()) return;

            client_fd = queue_.front();
            queue_.pop();
        }
        if (client_fd >= 0) handler_(client_fd);
    }
}

}  // namespace http_server

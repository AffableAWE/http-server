#include "http_server/thread_pool.hpp"

#include <chrono>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <utility>

namespace http_server {

ThreadPool::ThreadPool(unsigned num_threads, std::size_t max_queue,
                       Handler handler)
    : handler_(std::move(handler)), max_queue_(max_queue), running_(true) {
    if (max_queue_ == 0) {
        throw std::invalid_argument("ThreadPool max_queue must be positive");
    }
    if (num_threads == 0) {
        num_threads = std::thread::hardware_concurrency();
        if (num_threads == 0) num_threads = 2;  // fallback floor
    }

    workers_.reserve(num_threads);
    try {
        for (unsigned i = 0; i < num_threads; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    } catch (...) {
        // A partially constructed thread is joinable; tear it down before
        // propagating the allocation/system failure.
        shutdown();
        throw;
    }
}

ThreadPool::~ThreadPool() { shutdown(); }

bool ThreadPool::submit(ConnectionTask& task) {
    if (!task.socket.valid()) return false;

    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!running_.load(std::memory_order_relaxed) ||
            queue_.size() >= max_queue_) {
            return false;
        }
        queue_.push(std::move(task));
    }
    cv_.notify_one();
    return true;
}

void ThreadPool::beginShutdown() {
    bool first_shutdown = false;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        first_shutdown = running_.exchange(false);
        if (first_shutdown) {
            // Discard queued work. Each ConnectionTask destructor closes its
            // descriptor and releases its admission permit exactly once.
            while (!queue_.empty()) queue_.pop();
        }
    }
    if (first_shutdown) cv_.notify_all();
    idle_cv_.notify_all();
}

bool ThreadPool::waitForIdle(const Deadline& deadline) {
    std::unique_lock<std::mutex> lock(mtx_);
    while (active_handlers_ > 0) {
        const int remaining = deadline.remainingMilliseconds();
        if (remaining <= 0) return false;
        idle_cv_.wait_for(lock,
                          std::chrono::milliseconds(remaining));
    }
    return true;
}

void ThreadPool::join() {
    for (auto& thread : workers_) {
        if (thread.joinable()) thread.join();
    }
}

void ThreadPool::shutdown() {
    beginShutdown();
    join();
}

void ThreadPool::worker_loop() {
    while (true) {
        std::optional<ConnectionTask> task;
        {
            std::unique_lock<std::mutex> lock(mtx_);
            cv_.wait(lock, [this] {
                return !queue_.empty() ||
                       !running_.load(std::memory_order_relaxed);
            });

            if (!running_.load(std::memory_order_relaxed) && queue_.empty()) {
                return;
            }

            task.emplace(std::move(queue_.front()));
            queue_.pop();
            ++active_handlers_;
        }

        try {
            handler_(std::move(*task));
        } catch (const std::exception& error) {
            // One connection must not take down a worker thread. The task's
            // RAII ownership closes the descriptor and releases its slot.
            std::cerr << "[ERROR] Connection handler failed: " << error.what()
                      << std::endl;
        } catch (...) {
            std::cerr << "[ERROR] Connection handler failed with unknown error"
                      << std::endl;
        }

        task.reset();
        {
            std::lock_guard<std::mutex> lock(mtx_);
            --active_handlers_;
        }
        idle_cv_.notify_all();
    }
}

}  // namespace http_server

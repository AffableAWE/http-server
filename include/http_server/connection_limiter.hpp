#ifndef HTTP_SERVER_CONNECTION_LIMITER_HPP
#define HTTP_SERVER_CONNECTION_LIMITER_HPP

#include <atomic>
#include <cstddef>
#include <optional>

namespace http_server {

/*
 * Thread-safe admission counter shared by the accept loop and workers.
 * tryAcquire() returns a move-only permit, or empty when the configured limit
 * is reached. The permit releases its slot exactly once on destruction.
 */
class ConnectionLimiter {
   public:
    class Permit {
       public:
        Permit() noexcept = default;
        ~Permit() { reset(); }

        Permit(const Permit&) = delete;
        Permit& operator=(const Permit&) = delete;

        Permit(Permit&& other) noexcept : limiter_(other.limiter_) {
            other.limiter_ = nullptr;
        }

        Permit& operator=(Permit&& other) noexcept {
            if (this != &other) {
                reset();
                limiter_ = other.limiter_;
                other.limiter_ = nullptr;
            }
            return *this;
        }

        bool valid() const noexcept { return limiter_ != nullptr; }

        void reset() noexcept {
            if (limiter_) {
                limiter_->release();
                limiter_ = nullptr;
            }
        }

       private:
        friend class ConnectionLimiter;

        explicit Permit(ConnectionLimiter* limiter) noexcept
            : limiter_(limiter) {}

        ConnectionLimiter* limiter_ = nullptr;
    };

    explicit ConnectionLimiter(std::size_t max_connections)
        : max_connections_(max_connections) {}

    ConnectionLimiter(const ConnectionLimiter&) = delete;
    ConnectionLimiter& operator=(const ConnectionLimiter&) = delete;

    std::optional<Permit> tryAcquire() {
        std::size_t current = active_.load(std::memory_order_relaxed);
        while (current < max_connections_) {
            if (active_.compare_exchange_weak(
                    current, current + 1, std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                return Permit(this);
            }
        }
        return std::nullopt;
    }

    std::size_t active() const {
        return active_.load(std::memory_order_relaxed);
    }

    std::size_t limit() const { return max_connections_; }

   private:
    void release() noexcept {
        std::size_t current = active_.load(std::memory_order_relaxed);
        while (current > 0) {
            if (active_.compare_exchange_weak(
                    current, current - 1, std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                return;
            }
        }
    }

    std::size_t max_connections_;
    std::atomic<std::size_t> active_{0};
};

}  // namespace http_server

#endif  // HTTP_SERVER_CONNECTION_LIMITER_HPP

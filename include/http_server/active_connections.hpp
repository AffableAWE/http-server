#ifndef HTTP_SERVER_ACTIVE_CONNECTIONS_HPP
#define HTTP_SERVER_ACTIVE_CONNECTIONS_HPP

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

#include <sys/socket.h>

namespace http_server {

/*
 * Tracks descriptors currently owned by worker handlers. Shutdown uses
 * cancelAll() to call shutdown(2), which wakes blocked socket waits without
 * closing or reusing the descriptor. The owning worker still closes it later.
 *
 * The mutex makes cancellation safe with respect to the owner's remove():
 * once remove() succeeds, cancelAll() can no longer see the descriptor.
 */
class ActiveConnections {
   public:
    class Registration {
       public:
        Registration() noexcept = default;
        ~Registration() { reset(); }

        Registration(const Registration&) = delete;
        Registration& operator=(const Registration&) = delete;

        Registration(Registration&& other) noexcept
            : registry_(other.registry_), fd_(other.fd_) {
            other.registry_ = nullptr;
            other.fd_ = -1;
        }

        Registration& operator=(Registration&& other) noexcept {
            if (this != &other) {
                reset();
                registry_ = other.registry_;
                fd_ = other.fd_;
                other.registry_ = nullptr;
                other.fd_ = -1;
            }
            return *this;
        }

        void reset() noexcept {
            if (registry_) {
                registry_->remove(fd_);
                registry_ = nullptr;
                fd_ = -1;
            }
        }

       private:
        friend class ActiveConnections;

        Registration(ActiveConnections* registry, int fd) noexcept
            : registry_(registry), fd_(fd) {}

        ActiveConnections* registry_ = nullptr;
        int fd_ = -1;
    };

    ActiveConnections() = default;

    ActiveConnections(const ActiveConnections&) = delete;
    ActiveConnections& operator=(const ActiveConnections&) = delete;

    using CancellationFlag = std::shared_ptr<std::atomic<bool>>;

    // Registers a descriptor and returns a move-only lifetime token. The
    // caller must keep the token alive until after it stops using the fd.
    Registration add(int fd, CancellationFlag cancelled) {
        std::lock_guard<std::mutex> lock(mtx_);
        fds_.emplace(fd, std::move(cancelled));
        return Registration(this, fd);
    }

    void cancelAll() {
        std::lock_guard<std::mutex> lock(mtx_);
        for (auto& entry : fds_) {
            if (entry.second) {
                entry.second->store(true, std::memory_order_relaxed);
            }
            // Ignore ENOTCONN and similar races where the peer already went
            // away. The worker remains the only owner that closes the fd.
            (void)::shutdown(entry.first, SHUT_RDWR);
        }
    }

   private:
    friend class Registration;

    void remove(int fd) noexcept {
        std::lock_guard<std::mutex> lock(mtx_);
        fds_.erase(fd);
    }

    std::mutex mtx_;
    std::unordered_map<int, CancellationFlag> fds_;
};

}  // namespace http_server

#endif  // HTTP_SERVER_ACTIVE_CONNECTIONS_HPP

#ifndef HTTP_SERVER_UNIQUE_FD_HPP
#define HTTP_SERVER_UNIQUE_FD_HPP

#include <unistd.h>

namespace http_server {

/*
 * Move-only ownership wrapper for a POSIX file descriptor.
 * The descriptor is closed exactly once when the wrapper is destroyed.
 */
class UniqueFd {
   public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}

    ~UniqueFd() { reset(); }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}

    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    int get() const noexcept { return fd_; }
    bool valid() const noexcept { return fd_ >= 0; }

    // Relinquishes ownership without closing the descriptor.
    int release() noexcept {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = fd;
    }

   private:
    int fd_ = -1;
};

}  // namespace http_server

#endif  // HTTP_SERVER_UNIQUE_FD_HPP

#ifndef HTTP_SERVER_DEADLINE_HPP
#define HTTP_SERVER_DEADLINE_HPP

#include <chrono>

namespace http_server {

/*
 * Absolute monotonic deadline. A deadline is never reset by partial progress,
 * EINTR, or additional socket readiness.
 */
class Deadline {
   public:
    using Clock = std::chrono::steady_clock;

    explicit Deadline(Clock::time_point at) : at_(at) {}

    static Deadline after(std::chrono::milliseconds duration) {
        return Deadline(Clock::now() + duration);
    }

    bool expired() const { return Clock::now() >= at_; }

    // Remaining time rounded up to whole milliseconds, or zero when expired.
    int remainingMilliseconds() const {
        const auto now = Clock::now();
        if (now >= at_) return 0;
        const auto remaining =
            std::chrono::duration_cast<std::chrono::microseconds>(at_ - now);
        const auto milliseconds = (remaining.count() + 999) / 1000;
        if (milliseconds <= 0) return 0;
        return static_cast<int>(milliseconds);
    }

   private:
    Clock::time_point at_;
};

}  // namespace http_server

#endif  // HTTP_SERVER_DEADLINE_HPP

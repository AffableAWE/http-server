// LD_PRELOAD shim that makes a Linux host look like it has IPv6 present but
// disabled (net.ipv6.conf.all.disable_ipv6=1), as in many containers and
// IPv4-only clusters: an IPv6 socket can be created and configured, but
// binding it fails with EADDRNOTAVAIL.
//
// Used by tests/http_ipv6_fallback_tests.py. Linux only.
#include <dlfcn.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cerrno>

namespace {
template <typename Fn>
Fn real(const char* name) {
    return reinterpret_cast<Fn>(dlsym(RTLD_NEXT, name));
}
}  // namespace

extern "C" int socket(int domain, int type, int protocol) {
    static auto real_socket = real<int (*)(int, int, int)>("socket");
    // The test host may have no IPv6 stack at all; hand back a socket that
    // exists so the server gets as far as bind().
    return real_socket(domain == AF_INET6 ? AF_INET : domain, type, protocol);
}

extern "C" int setsockopt(int fd, int level, int name, const void* value,
                          socklen_t length) {
    static auto real_setsockopt =
        real<int (*)(int, int, int, const void*, socklen_t)>("setsockopt");
    if (level == IPPROTO_IPV6) return 0;  // IPV6_V6ONLY works on such a host
    return real_setsockopt(fd, level, name, value, length);
}

extern "C" int bind(int fd, const sockaddr* address, socklen_t length) {
    static auto real_bind = real<int (*)(int, const sockaddr*, socklen_t)>("bind");
    if (address != nullptr && address->sa_family == AF_INET6) {
        errno = EADDRNOTAVAIL;
        return -1;
    }
    return real_bind(fd, address, length);
}

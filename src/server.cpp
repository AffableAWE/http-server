#include "http_server/server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>

#include "http_server/request_handler.hpp"

namespace http_server {

namespace {

std::string addrToString(const sockaddr_storage& ss) {
    char buf[INET6_ADDRSTRLEN] = {0};
    if (ss.ss_family == AF_INET) {
        const auto* a = reinterpret_cast<const sockaddr_in*>(&ss);
        inet_ntop(AF_INET, &a->sin_addr, buf, sizeof(buf));
    } else if (ss.ss_family == AF_INET6) {
        const auto* a = reinterpret_cast<const sockaddr_in6*>(&ss);
        inet_ntop(AF_INET6, &a->sin6_addr, buf, sizeof(buf));
    }
    return buf[0] ? std::string(buf) : std::string("-");
}

}  // anonymous namespace

Server::Server(const Config& cfg)
    : cfg_(cfg), logger_(cfg.db_path), running_(false) {}

Server::~Server() {
    if (sockd4_ >= 0) ::close(sockd4_);
    if (sockd6_ >= 0) ::close(sockd6_);
}

void Server::stop() { running_.store(false); }

bool Server::setupSockets() {
    sockd4_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sockd4_ < 0) {
        perror("IPv4 socket");
        return false;
    }

    sockd6_ = ::socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (sockd6_ < 0) {
        perror("IPv6 socket");
        return false;
    }

    std::cout << "[INFO] IPv4 and IPv6 sockets created." << std::endl;

    int reuse = 1;
    if (::setsockopt(sockd4_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) <
        0) {
        perror("IPv4 SO_REUSEADDR");
        return false;
    }
    if (::setsockopt(sockd6_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) <
        0) {
        perror("IPv6 SO_REUSEADDR");
        return false;
    }

    // Force the v6 socket to v6-only so the v4 bind doesn't collide on Linux.
    int v6only = 1;
    if (::setsockopt(sockd6_, IPPROTO_IPV6, IPV6_V6ONLY, &v6only,
                     sizeof(v6only)) < 0) {
        perror("IPv6 V6ONLY");
        return false;
    }

    sockaddr_in addr4{};
    addr4.sin_family = AF_INET;
    addr4.sin_port = htons(cfg_.port);
    addr4.sin_addr.s_addr = INADDR_ANY;
    if (::bind(sockd4_, reinterpret_cast<sockaddr*>(&addr4), sizeof(addr4)) <
        0) {
        perror("IPv4 bind");
        return false;
    }

    sockaddr_in6 addr6{};
    addr6.sin6_family = AF_INET6;
    addr6.sin6_port = htons(cfg_.port);
    addr6.sin6_addr = in6addr_any;
    if (::bind(sockd6_, reinterpret_cast<sockaddr*>(&addr6), sizeof(addr6)) <
        0) {
        perror("IPv6 bind");
        return false;
    }

    std::cout << "[INFO] Bound to port " << cfg_.port << "." << std::endl;

    if (::listen(sockd4_, SOMAXCONN) < 0) {
        perror("IPv4 listen");
        return false;
    }
    if (::listen(sockd6_, SOMAXCONN) < 0) {
        perror("IPv6 listen");
        return false;
    }

    std::cout << "[INFO] Listening for incoming connections." << std::endl;
    return true;
}

void Server::acceptLoop() {
    // Worker handler captures what each request needs.
    ThreadPool pool(0, [this](int client_fd) {
        sockaddr_storage peer{};
        socklen_t plen = sizeof(peer);
        std::string client_ip = "-";
        if (::getpeername(client_fd, reinterpret_cast<sockaddr*>(&peer),
                          &plen) == 0) {
            client_ip = addrToString(peer);
        }
        handleRequest(client_fd, cfg_.static_dir, client_ip, logger_,
                      cfg_.enable_logging);
    });

    std::cout << "[INFO] Thread pool started." << std::endl;

    pollfd fds[2];
    fds[0].fd = sockd4_;
    fds[0].events = POLLIN;
    fds[1].fd = sockd6_;
    fds[1].events = POLLIN;

    while (running_.load()) {
        int ret = ::poll(fds, 2, 500);  // 500ms tick lets us notice stop()
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }
        if (ret == 0) continue;

        for (int i = 0; i < 2; ++i) {
            if (!(fds[i].revents & POLLIN)) continue;
            int client_fd = ::accept(fds[i].fd, nullptr, nullptr);
            if (client_fd < 0) {
                if (errno == EINTR) continue;
                perror("accept");
                continue;
            }
            pool.submit(client_fd);
        }
    }

    std::cout << "[INFO] Accept loop exiting; draining workers." << std::endl;
    pool.shutdown();
}

int Server::run() {
    if (!setupSockets()) return 1;
    running_.store(true);
    acceptLoop();
    return 0;
}

}  // namespace http_server

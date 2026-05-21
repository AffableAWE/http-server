#include "http_server/request_handler.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <fstream>
#include <iostream>

#include "http_server/http_parser.hpp"
#include "http_server/logger.hpp"
#include "http_server/router.hpp"
#include "http_server/utils.hpp"

namespace http_server {

namespace {

constexpr int BUFFER_SIZE = 4096;
constexpr size_t MAX_REQUEST_BYTES = 64 * 1024;  // 64KB header limit

// Sends all bytes in `data`, retrying on partial writes and EINTR.
// Returns true on success, false if the connection broke.
bool sendAll(int fd, const char* data, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = ::send(fd, data + total, len - total, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("send failed");
            return false;
        }
        if (n == 0) return false;
        total += static_cast<size_t>(n);
    }
    return true;
}

bool sendString(int fd, const std::string& s) {
    return sendAll(fd, s.data(), s.size());
}

}  // anonymous namespace

std::string constructResponse(const std::string& status,
                              const std::string& contentType,
                              const std::string& body) {
    return "HTTP/1.1 " + status + "\r\n" + "Content-Type: " + contentType +
           "\r\n" + "Content-Length: " + std::to_string(body.size()) + "\r\n" +
           "Connection: close\r\n\r\n" + body;
}

void handleRequest(int client_fd, const std::string& static_dir,
                   const std::string& client_ip, Logger& logger,
                   bool log_enabled) {
    char buffer[BUFFER_SIZE];
    std::string accumulated_data;
    accumulated_data.reserve(BUFFER_SIZE);

    int status_code = 0;
    size_t bytes_sent = 0;
    std::string log_method = "-";
    std::string log_path = "-";

    // 1) Read until we see the end-of-headers marker, or hit the size cap.
    while (accumulated_data.find("\r\n\r\n") == std::string::npos) {
        if (accumulated_data.size() >= MAX_REQUEST_BYTES) {
            std::string resp =
                constructResponse("413 Payload Too Large", "text/plain",
                                  "Request headers too large");
            sendString(client_fd, resp);
            status_code = 413;
            bytes_sent = resp.size();
            goto done;
        }

        ssize_t bytes_received = ::recv(client_fd, buffer, BUFFER_SIZE, 0);
        if (bytes_received < 0) {
            if (errno == EINTR) continue;
            perror("recv failed");
            goto done;
        }
        if (bytes_received == 0) {
            // Client closed without finishing the request.
            goto done;
        }
        accumulated_data.append(buffer, bytes_received);
    }

    // 2) Parse the request.
    {
        ParsedRequest req = parseRequest(accumulated_data);
        if (!req.valid) {
            std::string resp = constructResponse("400 Bad Request",
                                                 "text/plain", "Bad Request");
            sendString(client_fd, resp);
            status_code = 400;
            bytes_sent = resp.size();
            goto done;
        }

        log_method = req.method;
        log_path = req.path;

        if (req.method != "GET") {
            std::string resp = constructResponse(
                "405 Method Not Allowed", "text/plain", "Method Not Allowed");
            sendString(client_fd, resp);
            status_code = 405;
            bytes_sent = resp.size();
            goto done;
        }

        // 3) Resolve route and try to open the file.
        std::string file_path = routeToFile(req.path, static_dir);
        std::string content_type = getContentType(file_path);

        std::ifstream file(file_path, std::ios::binary);
        if (!file.is_open()) {
            std::string resp = constructResponse(
                "404 Not Found", "text/html",
                "<html><body><h1>404 Not Found</h1></body></html>");
            sendString(client_fd, resp);
            status_code = 404;
            bytes_sent = resp.size();
            goto done;
        }

        // 4) Determine size and send headers.
        file.seekg(0, std::ios::end);
        std::streampos file_size = file.tellg();
        file.seekg(0, std::ios::beg);
        if (file_size < 0) {
            std::string resp =
                constructResponse("500 Internal Server Error", "text/plain",
                                  "Internal Server Error");
            sendString(client_fd, resp);
            status_code = 500;
            bytes_sent = resp.size();
            goto done;
        }

        std::string headers =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: " +
            content_type +
            "\r\n"
            "Content-Length: " +
            std::to_string(file_size) +
            "\r\n"
            "Connection: close\r\n\r\n";

        if (!sendString(client_fd, headers)) {
            status_code = 200;
            bytes_sent = 0;
            goto done;
        }
        bytes_sent += headers.size();

        // 5) Stream the body. Loop on gcount() rather than eof().
        char file_buffer[BUFFER_SIZE];
        while (file.read(file_buffer, sizeof(file_buffer)) ||
               file.gcount() > 0) {
            std::streamsize got = file.gcount();
            if (got <= 0) break;
            if (!sendAll(client_fd, file_buffer, static_cast<size_t>(got)))
                break;
            bytes_sent += static_cast<size_t>(got);
        }
        status_code = 200;
    }

done:
    if (log_enabled) {
        logger.logAccess(log_method, log_path, status_code, bytes_sent,
                         client_ip);
    }
    ::close(client_fd);
}

}  // namespace http_server

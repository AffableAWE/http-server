#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<unistd.h>
#include<iostream>
#include<poll.h>
#include<unordered_map>
#include<string.h>
#include<fstream>
#include<sstream>
#include<thread>
#include<condition_variable>
#include<mutex>
#include<queue>

using namespace std;

string STATIC_DIR = "static";
int PORT = 8080;

void loadConfig() {
    const char* env_dir = getenv("STATIC_DIR");
    if (env_dir) STATIC_DIR = env_dir;

    const char* env_port = getenv("PORT");
    if (env_port) {
        char* end;
        long parsed = strtol(env_port, &end, 10);
        if (*end == '\0' && parsed > 0 && parsed <= 65535) {
            PORT = static_cast<int>(parsed);
        }
        else {
            cerr << "[WARNING] Invalid PORT '" << env_port << "'. Using 8080." << endl;
        }
    } 
}

/*
 * Removes leading and trailing whitespace (spaces, tabs, newlines) from the given string.
 * Input:- input: The string to trim.
 * Returns:- A new string without leading and trailing whitespace.
 */

string trim(const string& input) {
    // Find the first position which is not the whitespace
    size_t start = input.find_first_not_of(" \t\r\n");
    if (start == string::npos) return "";

    // Find the last position that is not a white space
    size_t end = input.find_last_not_of(" \t\r\n");
    if (end == string::npos) return "";

    // Returned the trimmed substring
    return input.substr(start, end - start + 1);
}

/*
 * Parses HTTP headers from the accumulated request data.
 * Extracts the headers section ("\r\n\r\n") and splits each line
 * into key-value pairs for a key-value map.
 * Input:- accumulated_data - A string containing HTTP request data
 * Returns: An unordered_map<string, string> with header key-value pairs
 */

unordered_map<string, string> parseHTTPHeaders(string& accumulated_data) {
    unordered_map<string, string> headers;

    size_t request_line_end = accumulated_data.find("\r\n");
    if (request_line_end != string::npos) {

        // Extract the request line if needed and locate the end of the headers
        size_t headers_end = accumulated_data.find("\r\n\r\n");
        if (headers_end != string::npos) {

            // Extract just the headers section
            size_t headers_start = request_line_end + 2;
            string headers_part = accumulated_data.substr(headers_start, headers_end - headers_start);

            // Parse line by line
            size_t line_start = 0;
            while (line_start < headers_part.size()) {
                size_t line_end = headers_part.find("\r\n", line_start);
                if (line_end == string::npos) break;

                string line = headers_part.substr(line_start, line_end - line_start);
                line_start = line_end + 2; // Move beyond "\r\n"

                // Split into key:value
                size_t colon_pos = line.find(":");
                if (colon_pos != string::npos) {
                    string key = trim(line.substr(0, colon_pos));
                    string value = trim(line.substr(colon_pos + 1));
                    headers[key] = value;
                }
            }
        }
    }

    return headers;
}

/*
 * Maps file extensions to MIME types for the Content-Type header.
 * Input:- path: The file path to analyze for extension.
 * Returns:- A string representing the MIME type, e.g., "text/html".
 */

string getContentType(const string& path) {
    static const unordered_map<string, string> mime_types = {
        {"html", "text/html"},
        {"css", "text/css"},
        {"js", "application/javascript"},
        {"json", "application/json"},
        {"png", "image/png"},
        {"jpg", "image/jpeg"},
        {"jpeg", "image/jpeg"}
    };

    size_t dotPosition = path.rfind('.');
    if (dotPosition == string::npos) return "application/octet-stream";

    string ext = path.substr(dotPosition + 1);
    auto it = mime_types.find(ext);
    return (it != mime_types.end()) ? it->second : "application/octet-stream";
}

/*
 * Maps HTTP request paths to file paths in the static directory.
 * Input:- path: The requested HTTP path, e.g., "/about".
 * Returns:- The corresponding file path in the static directory.
 */
string routeToFile(const string& path) {
    // Serve the main pages
    if (path == "/") {
        return STATIC_DIR + "/index.html";
    }
    else if (path == "/about") {
        return STATIC_DIR + "/about.html";
    }
    else if (path == "/404") {
        return STATIC_DIR + "/404.html";
    }

    if (path == "/aboutStyle.css") {
        return STATIC_DIR + "/aboutStyle.css";
    }
    else if (path == "/404Style.css") {
        return STATIC_DIR + "/404Style.css";
    }

    return STATIC_DIR + "/404.html";
}
/*
 * Constructs a full HTTP response from the status, Content-Type, and body.
 * Input:- 
 *   status: The HTTP status line, e.g., "200 OK".
 *   contentType: The MIME type for the response, e.g., "text/html".
 *   body: The content of the response.
 * Returns:- A full HTTP response as a string.
 */

string constructResponse (const string& status, const string& contentType, const string& body) {
    return "HTTP/1.1 " + status + "\r\n" +
           "Content-Type: " + contentType + "\r\n" +
           "Content-Length: " + to_string(body.size()) + "\r\n" + 
           "Connection: close \r\n\r\n" +
           body;
}

/*
 * handleRequest():
 * Handles a client connection, processes the request, and serves a static file or a 404 error.
 * Input:- client_fd: The file descriptor for the connected client socket.
 */
void handleRequest(int client_fd) {
    const int BUFFER_SIZE = 1024;
    char buffer[BUFFER_SIZE];
    string accumulated_data;

    while (true) {
        // Receive data from the client
        int bytes_received = recv(client_fd, buffer, BUFFER_SIZE, 0);
        if (bytes_received < 0) {
            perror("recv failed");
            break;
        } else if (bytes_received == 0) {
            cout << "Client Disconnected" << endl;
            break;
        }

        // Append received data
        accumulated_data.append(buffer, bytes_received);

        // Check if we have reached the end of the HTTP headers
        size_t headers_end_pos = accumulated_data.find("\r\n\r\n");
        if (headers_end_pos != string::npos) {
            cout << "End of HTTP headers detected." << endl;

            // Parse the request line (e.g., "GET /about HTTP/1.1")
            size_t request_line_end = accumulated_data.find("\r\n");
            string request_line = accumulated_data.substr(0, request_line_end);
            istringstream iss(request_line);

            string method, path, http_version;
            iss >> method >> path >> http_version;

            // Validate the HTTP method
            if (method != "GET") {
                string response = constructResponse("405 Method Not Allowed", "text/plain", "Method Not Allowed");
                send(client_fd, response.c_str(), response.size(), 0);
                break;
            }

            // Map the path to a file
            string file_path = routeToFile(path);
            string content_type = getContentType(file_path);

            // Open the file
            ifstream file(file_path, ios::binary);
            if (file.is_open()) {
                // Read the file size
                file.seekg(0, ios::end);
                size_t file_size = file.tellg();
                file.seekg(0, ios::beg);

                // Send HTTP headers
                string headers = "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: " + content_type + "\r\n"
                                 "Content-Length: " + to_string(file_size) + "\r\n"
                                 "Connection: close\r\n\r\n";
                send(client_fd, headers.c_str(), headers.size(), 0);

                // Stream the file content
                char file_buffer[1024];
                while (!file.eof()) {
                    file.read(file_buffer, sizeof(file_buffer));
                    send(client_fd, file_buffer, file.gcount(), 0);
                }

                file.close();
            } else {
                // Handle missing file (404)
                string response = constructResponse("404 Not Found", "text/html",
                                                    "<html><body><h1>404 Not Found</h1></body></html>");
                send(client_fd, response.c_str(), response.size(), 0);
            }

            break;
        }
    }

    // Close the client socket after handling the request
    close(client_fd);
}

queue<int> client_queue;
mutex queue_mutex;
condition_variable queue_cond;
bool server_running = true;

void worker_thread() {
    while (true) {
        int client_fd = -1;

        {
            unique_lock<mutex> lock(queue_mutex);
            queue_cond.wait(lock, []{
                return !client_queue.empty() || !server_running;
            });

            if (client_queue.empty() || !server_running) break;

            client_fd = client_queue.front();
            client_queue.pop();
        }
        handleRequest(client_fd);
    }
}

int main() {

    loadConfig();

    // Creating an IPv4 Socket
    int sockd4 = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sockd4 < 0) {
        cout << "Error creating IPv4 socket." << endl;
        return -1;
    }

    // Creating an IPv6 Socket
    int sockd6 = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (sockd6 < 0) {
        cout << "Error creating IPv6 socket." << endl;
        return -1;
    }

    cout << "[INFO] IPv4 and IPv6 sockets successfully created." << endl;

    // Set socket options
    int reuse = 1;
    if (setsockopt(sockd4, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("IPv4 socket option setup failed");
        return -1;
    }

    if (setsockopt(sockd6, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("IPv6 socket option setup failed");
        return -1;
    }

    // Binding the sockets
    sockaddr_in server_addr4 = {};
    server_addr4.sin_family = AF_INET;
    server_addr4.sin_port = htons(PORT);
    server_addr4.sin_addr.s_addr = INADDR_ANY;

    if (bind(sockd4, (struct sockaddr*)&server_addr4, sizeof(server_addr4)) < 0) {
        perror("Bind failed for IPv4");
        return -1;
    }

    sockaddr_in6 server_addr6 = {};
    server_addr6.sin6_family = AF_INET6;
    server_addr6.sin6_port = htons(PORT);
    server_addr6.sin6_addr = in6addr_any;

    if (bind(sockd6, (struct sockaddr*)&server_addr6, sizeof(server_addr6)) < 0) {
        perror("Bind failed for IPv6");
        return -1;
    }

    cout << "[INFO] IPv4 and IPv6 sockets bound to port 8080." << endl;

    // Listening for connections
    if (listen(sockd4, SOMAXCONN) < 0) {
        perror("IPv4 listen failed");
        return -1;
    }

    if (listen(sockd6, SOMAXCONN) < 0) {
        perror("IPv6 listen failed");
        return -1;
    }

    cout << "[INFO] Server is now listening for incoming connections." << endl;
 
    // Creating a thread pool
    vector<thread> thread_pool;
    const unsigned num_thread = thread::hardware_concurrency();
    for (unsigned i = 0; i < num_thread; ++i) {
        thread_pool.emplace_back(worker_thread);
    }

    cout<< "[INFO] Thread Pool Created (" << num_thread << " Workers)" << endl;

    // Monitor both sockets
    pollfd fds[2];
    fds[0].fd = sockd4;
    fds[0].events = POLLIN;
    fds[1].fd = sockd6;
    fds[1].events = POLLIN;

    while (true) {
        int ret = poll(fds, 2, -1);
        if (ret < 0) {
            perror("poll failed");
            break;
        }

        // Check IPv4 socket
        if (fds[0].revents & POLLIN) {
            int client_fd = accept(fds[0].fd, NULL, NULL);
            if (client_fd < 0) {
                perror("IPv4 accept failed");
            } else {
                cout << "IPv4 connection accepted." << endl;
                {
                    lock_guard<mutex> lock(queue_mutex);
                    client_queue.push(client_fd);
                }
                queue_cond.notify_one();
            }
        }

        // Check IPv6 socket
        if (fds[1].revents & POLLIN) {
            int client_fd = accept(fds[1].fd, NULL, NULL);
            if (client_fd < 0) {
                perror("IPv6 accept failed");
            } else {
                cout << "IPv6 connection accepted." << endl;
                {
                    lock_guard<mutex> lock(queue_mutex);
                    client_queue.push(client_fd);
                }
                queue_cond.notify_one();
            }
        }
    }

    // Clean Shutdown
    server_running = false;
    queue_cond.notify_all();
    for (auto& t : thread_pool) {
        t.join();
    }

    return 0;
}


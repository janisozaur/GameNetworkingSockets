// Robust C++ Signaling Server Example for GameNetworkingSockets
//
// This server implements a simple identity-based signaling protocol.
// 1. Client connects via TCP.
// 2. Client sends its identity as the first line (ending in \n).
// 3. Client can then send messages: "DEST_IDENTITY HEX_PAYLOAD\n"
// 4. Server forwards "SRC_IDENTITY HEX_PAYLOAD\n" to the destination.
//
// This example uses non-blocking I/O with poll() for efficiency.

#include <iostream>
#include <vector>
#include <string>
#include <map>
#include <algorithm>
#include <cstring>
#include <errno.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <signal.h>

#define DEFAULT_PORT 10000
#define MAX_CLIENTS 1024
#define BUFFER_SIZE 4096

struct Client {
    int fd;
    std::string identity;
    std::string read_buffer;
    std::string write_buffer;
    bool identity_received = false;
};

class SignalingServer {
public:
    SignalingServer(int port) : m_port(port) {
        signal(SIGPIPE, SIG_IGN);
    }

    bool Start() {
        int family = AF_INET6;
        m_listen_fd = socket(AF_INET6, SOCK_STREAM, 0);
        if (m_listen_fd < 0) {
            // Fallback to IPv4 if IPv6 fails
            family = AF_INET;
            m_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
            if (m_listen_fd < 0) {
                perror("socket");
                return false;
            }
        } else {
            int off = 0;
            setsockopt(m_listen_fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
        }

        int opt = 1;
        setsockopt(m_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        SetNonBlocking(m_listen_fd);

        if (family == AF_INET6) {
            sockaddr_in6 addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin6_family = AF_INET6;
            addr.sin6_port = htons(m_port);
            addr.sin6_addr = in6addr_any;

            if (bind(m_listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                perror("bind");
                return false;
            }
        } else {
            sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons(m_port);
            addr.sin_addr.s_addr = INADDR_ANY;

            if (bind(m_listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                perror("bind");
                return false;
            }
        }

        if (listen(m_listen_fd, 10) < 0) {
            perror("listen");
            return false;
        }

        std::cout << "Signaling server listening on port " << m_port << std::endl;
        return true;
    }

    void Run() {
        std::vector<pollfd> poll_fds;
        while (true) {
            poll_fds.clear();
            poll_fds.push_back({m_listen_fd, POLLIN, 0});

            for (auto const& [fd, client] : m_clients) {
                short events = POLLIN;
                if (!client.write_buffer.empty()) {
                    events |= POLLOUT;
                }
                poll_fds.push_back({fd, events, 0});
            }

            int ret = poll(poll_fds.data(), poll_fds.size(), -1);
            if (ret < 0) {
                perror("poll");
                break;
            }

            for (size_t i = 0; i < poll_fds.size(); ++i) {
                int fd = poll_fds[i].fd;
                if (fd == m_listen_fd) {
                    if (poll_fds[i].revents & POLLIN) {
                        AcceptClient();
                    }
                } else {
                    // Check if client still exists before handling events
                    if (m_clients.find(fd) == m_clients.end()) {
                        continue;
                    }

                    bool handled = false;
                    if (poll_fds[i].revents & POLLIN) {
                        ReadClient(fd);
                        handled = true;
                    } else if (poll_fds[i].revents & POLLOUT) {
                        WriteClient(fd);
                        handled = true;
                    }

                    if (!handled && (poll_fds[i].revents & (POLLERR | POLLHUP | POLLNVAL))) {
                        DisconnectClient(fd);
                    }
                }
            }
        }
    }

private:
    int m_port;
    int m_listen_fd;
    std::map<int, Client> m_clients;
    std::map<std::string, int> m_identity_to_fd;

    void SetNonBlocking(int fd) {
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    void AcceptClient() {
        sockaddr_in6 addr;
        socklen_t len = sizeof(addr);
        int client_fd = accept(m_listen_fd, (struct sockaddr*)&addr, &len);
        if (client_fd < 0) return;

        SetNonBlocking(client_fd);
        m_clients[client_fd] = {client_fd, "", "", "", false};
        std::cout << "New connection, fd=" << client_fd << std::endl;
    }

    void DisconnectClient(int fd) {
        auto it = m_clients.find(fd);
        if (it == m_clients.end()) return; // Already disconnected, idempotent

        if (it->second.identity_received) {
            m_identity_to_fd.erase(it->second.identity);
            std::cout << "Client '" << it->second.identity << "' disconnected" << std::endl;
        } else {
            std::cout << "Unidentified client disconnected, fd=" << fd << std::endl;
        }
        close(fd);
        m_clients.erase(it);
    }

    void ReadClient(int fd) {
        auto it = m_clients.find(fd);
        if (it == m_clients.end()) return;

        char buf[BUFFER_SIZE];
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            DisconnectClient(fd);
            return;
        }

        Client& client = it->second;
        client.read_buffer.append(buf, n);

        size_t pos;
        while ((pos = client.read_buffer.find('\n')) != std::string::npos) {
            std::string line = client.read_buffer.substr(0, pos);
            client.read_buffer.erase(0, pos + 1);

            if (!client.identity_received) {
                HandleIdentity(client, line);
            } else {
                HandleForward(client, line);
            }
            // Re-check if client still exists after handling
            if (m_clients.find(fd) == m_clients.end()) return;
        }
    }

    void HandleIdentity(Client& client, const std::string& identity) {
        if (identity.empty() || identity.find(' ') != std::string::npos) {
            DisconnectClient(client.fd);
            return;
        }
        client.identity = identity;
        client.identity_received = true;

        // Replace old connection for same identity if exists
        auto it = m_identity_to_fd.find(identity);
        if (it != m_identity_to_fd.end()) {
            DisconnectClient(it->second);
        }
        m_identity_to_fd[identity] = client.fd;
        std::cout << "Client registered identity: " << identity << std::endl;
    }

    void HandleForward(Client& client, const std::string& line) {
        size_t space = line.find(' ');
        if (space == std::string::npos) return;

        std::string dest_identity = line.substr(0, space);
        std::string payload = line.substr(space + 1);

        auto it = m_identity_to_fd.find(dest_identity);
        if (it != m_identity_to_fd.end()) {
            auto dest_it = m_clients.find(it->second);
            if (dest_it != m_clients.end()) {
                Client& dest_client = dest_it->second;
                dest_client.write_buffer += client.identity + " " + payload + "\n";
                std::cout << "Forwarding: " << client.identity << " -> " << dest_identity << " (" << payload.size() << " bytes)" << std::endl;
            }
        } else {
            std::cout << "Destination not found: " << dest_identity << std::endl;
        }
    }

    void WriteClient(int fd) {
        auto it = m_clients.find(fd);
        if (it == m_clients.end()) return;

        Client& client = it->second;
        if (client.write_buffer.empty()) return;

        ssize_t n = send(fd, client.write_buffer.data(), client.write_buffer.size(), 0);
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                DisconnectClient(fd);
            }
            return;
        }
        client.write_buffer.erase(0, n);
    }
};

int main(int argc, char** argv) {
    int port = DEFAULT_PORT;
    if (argc > 1) {
        port = std::stoi(argv[1]);
    }

    SignalingServer server(port);
    if (!server.Start()) {
        return 1;
    }
    server.Run();

    return 0;
}

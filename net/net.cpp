#include "net.h"

#include <cstring>
#include <cerrno>
#include <cstdio>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

namespace net {

static bool g_eof = false;

bool peer_eof() { return g_eof; }
void reset_eof() { g_eof = false; }

int listen(const std::string& ip, uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (ip.empty() || ip == "0.0.0.0") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else if (::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        return -1;
    }
    if (::bind(fd, (sockaddr*)&addr, sizeof addr) != 0) {
        ::close(fd);
        return -1;
    }
    if (::listen(fd, 4) != 0) {
        ::close(fd);
        return -1;
    }
    // Non-blocking so a lazy accept() (e.g. in a server event loop that also
    // reads from already-accepted clients) returns -1 immediately instead of
    // blocking until the next connection arrives.
    int fl = ::fcntl(fd, F_GETFL, 0);
    if (fl < 0 || ::fcntl(fd, F_SETFL, fl | O_NONBLOCK) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

int accept(int listen_fd) {
    sockaddr_in addr{};
    socklen_t addrlen = sizeof addr;
    int fd = ::accept(listen_fd, (sockaddr*)&addr, &addrlen);
    return fd;
}

int connect(const std::string& host, uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) == 1) {
        if (::connect(fd, (sockaddr*)&addr, sizeof addr) != 0) {
            ::close(fd);
            return -1;
        }
        g_eof = false;
        return fd;
    }

    // host is a hostname; resolve
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    std::string portstr = std::to_string(port);
    if (::getaddrinfo(host.c_str(), portstr.c_str(), &hints, &res) != 0) {
        ::close(fd);
        return -1;
    }
    int rc = -1;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            rc = fd;
            break;
        }
    }
    ::freeaddrinfo(res);
    if (rc < 0) {
        ::close(fd);
        return -1;
    }
    g_eof = false;
    return fd;
}

// Read exactly n bytes. Returns true on success, false on EOF/error.
static bool read_exact(int fd, uint8_t* out, size_t n, int timeout_ms) {
    size_t got = 0;
    while (got < n) {
        if (timeout_ms > 0) {
            pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLIN;
            int pr = ::poll(&pfd, 1, timeout_ms);
            if (pr <= 0) return false;  // timeout or error
        }
        ssize_t r = ::recv(fd, out + got, n - got, 0);
        if (r == 0) { g_eof = true; return false; }  // clean peer close
        if (r < 0) return false;
        got += (size_t)r;
    }
    return true;
}

int read_packet(int fd, std::vector<uint8_t>& buf, int timeout_ms) {
    uint8_t lenb[2];
    if (!read_exact(fd, lenb, 2, timeout_ms)) return -1;  // EOF/timeout
    uint16_t packet_len;
    std::memcpy(&packet_len, lenb, sizeof packet_len);
    if (packet_len < 1) return -1;
    buf.resize(packet_len);
    if (!read_exact(fd, buf.data(), packet_len, timeout_ms)) return -1;
    // packet_len includes the proto byte, so payload = packet_len - 1
    // (may be 0 for payload-less packets like CTOS_HS_READY)
    return (int)packet_len - 1;
}

int write_packet(int fd, uint8_t proto, const uint8_t* data, size_t len) {
    if (len + 1 > 0xffff) return -1;
    uint16_t packet_len = (uint16_t)(len + 1);
    std::vector<uint8_t> out(len + 3);
    std::memcpy(out.data(), &packet_len, 2);
    out[2] = proto;
    if (len > 0) std::memcpy(out.data() + 3, data, len);
    // Single send() call: the server (srvpro) rejects a packet whose 2-byte
    // length header arrives in a different TCP chunk than its payload.
    size_t off = 0;
    while (off < out.size()) {
        ssize_t n = ::send(fd, out.data() + off, out.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return (int)len;
}

int send_packet(int fd, uint8_t proto) {
    return write_packet(fd, proto, nullptr, 0);
}

void close(int fd) {
    if (fd >= 0) ::close(fd);
}

} // namespace net

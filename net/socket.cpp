#include "net/socket.hpp"

#include "core/exceptions.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>

namespace exchange::net {
namespace {

[[nodiscard]] std::string describe(const char* what) {
    return std::string(what) + ": " + std::strerror(errno);
}

} // namespace

void Fd::reset() noexcept {
    if (fd_ >= 0) {
        // The return value is deliberately ignored. close() can report EINTR,
        // but on Linux the descriptor is released regardless, so retrying
        // would risk closing a descriptor another thread has since been given
        // the same number for -- a far worse bug than the one being handled.
        ::close(fd_);
        fd_ = -1;
    }
}

Fd listenOn(std::uint16_t port, int backlog) {
    Fd socket(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!socket.valid()) {
        throw NetworkError(describe("socket"));
    }

    // Must precede bind(). Applied afterwards it has no effect on the address
    // already claimed.
    const int enable = 1;
    if (::setsockopt(socket.get(), SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) != 0) {
        throw NetworkError(describe("setsockopt(SO_REUSEADDR)"));
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    // reinterpret_cast to sockaddr* is the documented BSD sockets calling
    // convention, predating any better option in C. -Wold-style-cast is what
    // forces it to be spelled out rather than hidden in a C cast.
    if (::bind(socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        throw NetworkError(describe("bind"));
    }
    if (::listen(socket.get(), backlog) != 0) {
        throw NetworkError(describe("listen"));
    }

    setNonBlocking(socket.get());
    return socket;
}

std::uint16_t boundPort(int fd) {
    sockaddr_in address{};
    socklen_t length = sizeof(address);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        throw NetworkError(describe("getsockname"));
    }
    return ntohs(address.sin_port);
}

Fd connectToLoopback(std::uint16_t port) {
    Fd socket(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!socket.valid()) {
        throw NetworkError(describe("socket"));
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);

    if (::connect(socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) !=
        0) {
        throw NetworkError(describe("connect"));
    }

    setNoDelay(socket.get());
    return socket;
}

void setNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        throw NetworkError(describe("fcntl(F_GETFL)"));
    }
    // Read-modify-write rather than a bare set: overwriting the flag word
    // would silently clear anything else already on the descriptor.
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        throw NetworkError(describe("fcntl(F_SETFL, O_NONBLOCK)"));
    }
}

void setNoDelay(int fd) {
    const int enable = 1;
    if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable)) != 0) {
        throw NetworkError(describe("setsockopt(TCP_NODELAY)"));
    }
}
void setSendBufferSize(int fd, int bytes) {
    if (::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes)) != 0) {
        throw NetworkError(describe("setsockopt(SO_SNDBUF)"));
    }
}

void setReceiveBufferSize(int fd, int bytes) {
    if (::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes)) != 0) {
        throw NetworkError(describe("setsockopt(SO_RCVBUF)"));
    }
}

} // namespace exchange::net

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include <gtest/gtest.h>

#include "tcpkv/fd.hpp"
#include "tcpkv/socket.hpp"

using tcpkv::FileDescriptor;
using tcpkv::Socket;

namespace {

// fcntl(F_GETFD) is the cheapest way to ask the kernel "is this descriptor still open?".
bool fd_is_open(int fd) { return ::fcntl(fd, F_GETFD) != -1; }

std::string read_exactly(Socket& socket, std::size_t count) {
    std::string out;
    char buf[4096];
    while (out.size() < count) {
        const std::size_t n = socket.read_some(buf, std::min(sizeof buf, count - out.size()));
        if (n == 0) {
            break;
        }
        out.append(buf, n);
    }
    return out;
}

}  // namespace

TEST(FileDescriptor, DefaultConstructedIsInvalid) {
    const FileDescriptor fd;
    EXPECT_FALSE(fd.valid());
    EXPECT_FALSE(static_cast<bool>(fd));
    EXPECT_EQ(fd.get(), -1);
}

TEST(FileDescriptor, ClosesOnDestruction) {
    int raw = -1;
    {
        const std::pair<FileDescriptor, FileDescriptor> ends = tcpkv::make_pipe();
        raw = ends.first.get();
        ASSERT_TRUE(fd_is_open(raw));
    }
    EXPECT_FALSE(fd_is_open(raw));
}

TEST(FileDescriptor, MoveConstructorTransfersOwnership) {
    std::pair<FileDescriptor, FileDescriptor> ends = tcpkv::make_pipe();
    const int raw = ends.first.get();

    FileDescriptor moved(std::move(ends.first));
    EXPECT_EQ(moved.get(), raw);
    EXPECT_FALSE(ends.first.valid());  // moved-from holds -1, so it will not close `raw`
    EXPECT_TRUE(fd_is_open(raw));
}

TEST(FileDescriptor, MoveAssignmentClosesTheOldDescriptor) {
    std::pair<FileDescriptor, FileDescriptor> a = tcpkv::make_pipe();
    std::pair<FileDescriptor, FileDescriptor> b = tcpkv::make_pipe();
    const int old_raw = a.first.get();
    const int new_raw = b.first.get();

    a.first = std::move(b.first);
    EXPECT_FALSE(fd_is_open(old_raw));
    EXPECT_TRUE(fd_is_open(new_raw));
    EXPECT_EQ(a.first.get(), new_raw);
    EXPECT_FALSE(b.first.valid());
}

TEST(FileDescriptor, ReleaseGivesUpOwnershipWithoutClosing) {
    std::pair<FileDescriptor, FileDescriptor> ends = tcpkv::make_pipe();
    const int raw = ends.first.release();
    EXPECT_FALSE(ends.first.valid());
    EXPECT_TRUE(fd_is_open(raw));
    ::close(raw);  // we own it now
}

TEST(FileDescriptor, ResetClosesAndAdopts) {
    std::pair<FileDescriptor, FileDescriptor> ends = tcpkv::make_pipe();
    const int first = ends.first.get();
    const int second = ends.second.release();

    ends.first.reset(second);
    EXPECT_FALSE(fd_is_open(first));
    EXPECT_TRUE(fd_is_open(second));
    EXPECT_EQ(ends.first.get(), second);

    ends.first.reset();
    EXPECT_FALSE(fd_is_open(second));
    EXPECT_FALSE(ends.first.valid());
}

TEST(FileDescriptor, SwapExchangesDescriptors) {
    std::pair<FileDescriptor, FileDescriptor> ends = tcpkv::make_pipe();
    const int r = ends.first.get();
    const int w = ends.second.get();
    swap(ends.first, ends.second);
    EXPECT_EQ(ends.first.get(), w);
    EXPECT_EQ(ends.second.get(), r);
}

TEST(FileDescriptor, PipeEndsHaveCloseOnExec) {
    const std::pair<FileDescriptor, FileDescriptor> ends = tcpkv::make_pipe();
    EXPECT_NE(::fcntl(ends.first.get(), F_GETFD) & FD_CLOEXEC, 0);
    EXPECT_NE(::fcntl(ends.second.get(), F_GETFD) & FD_CLOEXEC, 0);
}

TEST(FileDescriptor, SetNonblockingTogglesFlag) {
    const std::pair<FileDescriptor, FileDescriptor> ends = tcpkv::make_pipe();
    tcpkv::set_nonblocking(ends.first.get(), true);
    EXPECT_NE(::fcntl(ends.first.get(), F_GETFL) & O_NONBLOCK, 0);
    tcpkv::set_nonblocking(ends.first.get(), false);
    EXPECT_EQ(::fcntl(ends.first.get(), F_GETFL) & O_NONBLOCK, 0);
}

TEST(FileDescriptor, SetNonblockingOnBadDescriptorThrows) {
    EXPECT_THROW(tcpkv::set_nonblocking(-1, true), std::system_error);
}

TEST(Socket, ListenOnEphemeralPortReportsRealPort) {
    const Socket listener = Socket::listen_tcp("127.0.0.1", 0, 8);
    EXPECT_GT(listener.local_port(), 0);
}

TEST(Socket, AcceptedConnectionEchoesBytes) {
    Socket listener = Socket::listen_tcp("127.0.0.1", 0, 8);
    Socket client = Socket::connect_tcp("127.0.0.1", listener.local_port());
    Socket server_side = listener.accept();

    client.write_all("hello");
    EXPECT_EQ(read_exactly(server_side, 5), "hello");

    server_side.write_all("world");
    EXPECT_EQ(read_exactly(client, 5), "world");

    EXPECT_NE(server_side.peer_address(), "?");
}

TEST(Socket, ReadReturnsZeroAfterPeerShutdown) {
    Socket listener = Socket::listen_tcp("127.0.0.1", 0, 8);
    Socket client = Socket::connect_tcp("127.0.0.1", listener.local_port());
    Socket server_side = listener.accept();

    client.shutdown_both();
    char buf[8];
    EXPECT_EQ(server_side.read_some(buf, sizeof buf), 0u);
}

TEST(Socket, WaitReadableTimesOutWhenIdle) {
    Socket listener = Socket::listen_tcp("127.0.0.1", 0, 8);
    Socket client = Socket::connect_tcp("127.0.0.1", listener.local_port());
    Socket server_side = listener.accept();

    EXPECT_FALSE(server_side.wait_readable(std::chrono::milliseconds(30)));
    client.write_all("x");
    EXPECT_TRUE(server_side.wait_readable(std::chrono::milliseconds(1000)));
}

TEST(Socket, LargeWriteIsDeliveredCompletely) {
    Socket listener = Socket::listen_tcp("127.0.0.1", 0, 8);
    Socket client = Socket::connect_tcp("127.0.0.1", listener.local_port());
    Socket server_side = listener.accept();

    // Far larger than the socket buffers, so send() must return short counts and
    // write_all() must loop. A reader thread is needed or both sides would block.
    const std::string payload(4 * 1024 * 1024, 'z');
    std::thread writer([&] {
        client.write_all(payload);
        client.shutdown_both();
    });

    std::string received;
    char buf[65536];
    for (;;) {
        const std::size_t n = server_side.read_some(buf, sizeof buf);
        if (n == 0) {
            break;
        }
        received.append(buf, n);
    }
    writer.join();

    ASSERT_EQ(received.size(), payload.size());
    EXPECT_EQ(received, payload);
}

TEST(Socket, ConnectToClosedPortThrows) {
    std::uint16_t port = 0;
    {
        const Socket listener = Socket::listen_tcp("127.0.0.1", 0, 8);
        port = listener.local_port();
    }  // listener closed: nothing listens on `port` now (barring an unlucky reuse)
    EXPECT_THROW(Socket::connect_tcp("127.0.0.1", port), std::system_error);
}

TEST(Socket, NonBlockingAcceptWithoutClientThrowsEagain) {
    Socket listener = Socket::listen_tcp("127.0.0.1", 0, 8);
    listener.set_nonblocking(true);
    try {
        listener.accept();
        FAIL() << "accept should not succeed with no pending connection";
    } catch (const std::system_error& e) {
        const int code = e.code().value();
        EXPECT_TRUE(code == EAGAIN || code == EWOULDBLOCK) << e.what();
    }
}

// m04_framing.cpp - length-prefixed binary framing with explicit byte order.
//
// Build:  g++ -std=c++17 -Wall -Wextra -Werror -pthread m04_framing.cpp -o framing
// Run:    ./framing
//
// Demonstrates:
//   1. A fixed 8-byte header {u32 length, u16 type, u16 flags} encoded in network byte order.
//   2. Encoding with memcpy (never cast a char* to a struct*: alignment and padding).
//   3. Decoding from a byte stream that arrives ONE BYTE AT A TIME over a real socketpair,
//      proving the read loop / parser handles any chunking.
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace frame {

constexpr size_t kHeaderSize = 8;
constexpr uint32_t kMaxPayload = 16u * 1024 * 1024;

struct Header {
    uint32_t length; // payload bytes, not counting the header
    uint16_t type;   // message type / opcode
    uint16_t flags;
};

// Serialise the header into exactly 8 bytes, big-endian ("network byte order").
inline void encode_header(const Header& h, unsigned char out[kHeaderSize]) {
    uint32_t len_be = htonl(h.length);
    uint16_t type_be = htons(h.type);
    uint16_t flags_be = htons(h.flags);
    std::memcpy(out + 0, &len_be, 4);
    std::memcpy(out + 4, &type_be, 2);
    std::memcpy(out + 6, &flags_be, 2);
}

inline Header decode_header(const unsigned char in[kHeaderSize]) {
    uint32_t len_be;
    uint16_t type_be, flags_be;
    std::memcpy(&len_be, in + 0, 4);
    std::memcpy(&type_be, in + 4, 2);
    std::memcpy(&flags_be, in + 6, 2);
    return Header{ntohl(len_be), ntohs(type_be), ntohs(flags_be)};
}

inline std::string encode(uint16_t type, uint16_t flags, const std::string& payload) {
    assert(payload.size() <= kMaxPayload);
    unsigned char hdr[kHeaderSize];
    encode_header(Header{static_cast<uint32_t>(payload.size()), type, flags}, hdr);
    std::string out(reinterpret_cast<const char*>(hdr), kHeaderSize);
    out += payload;
    return out;
}

// Incremental decoder: feed any number of bytes, pull complete frames.
class Decoder {
public:
    void feed(const char* p, size_t n) { buf_.append(p, n); }

    // Returns true and fills (hdr, payload) when a whole frame is available.
    // Throws std::runtime_error when the header is invalid (caller should close the connection).
    bool next(Header& hdr, std::string& payload) {
        if (buf_.size() - pos_ < kHeaderSize) return false;
        Header h = decode_header(reinterpret_cast<const unsigned char*>(buf_.data() + pos_));
        if (h.length > kMaxPayload) throw std::runtime_error("frame too large: refusing to allocate");
        if (buf_.size() - pos_ < kHeaderSize + h.length) return false; // wait for the rest
        hdr = h;
        payload.assign(buf_.data() + pos_ + kHeaderSize, h.length);
        pos_ += kHeaderSize + h.length;
        if (pos_ > 65536 && pos_ > buf_.size() / 2) {
            buf_.erase(0, pos_);
            pos_ = 0;
        }
        return true;
    }

private:
    std::string buf_;
    size_t pos_ = 0;
};

} // namespace frame

static void write_all(int fd, const char* p, size_t n) {
    while (n > 0) {
        ssize_t w = ::send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) continue;
            std::perror("send");
            std::abort();
        }
        p += w;
        n -= static_cast<size_t>(w);
    }
}

int main() {
    // --- 1. Header round trip and byte layout ------------------------------------------------
    unsigned char raw[frame::kHeaderSize];
    frame::encode_header(frame::Header{0x01020304u, 0x0A0Bu, 0x0001u}, raw);
    std::printf("header bytes on the wire:");
    for (unsigned char b : raw) std::printf(" %02x", b);
    std::printf("\n");
    // Big-endian: most significant byte first, regardless of the CPU (x86 is little-endian).
    assert(raw[0] == 0x01 && raw[1] == 0x02 && raw[2] == 0x03 && raw[3] == 0x04);
    assert(raw[4] == 0x0A && raw[5] == 0x0B && raw[6] == 0x00 && raw[7] == 0x01);
    frame::Header back = frame::decode_header(raw);
    assert(back.length == 0x01020304u && back.type == 0x0A0Bu && back.flags == 1);

    // --- 2. Stream several frames through a socketpair, one byte per send() ------------------
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        std::perror("socketpair");
        return 1;
    }
    std::vector<std::string> payloads = {"", "hello", std::string(5000, 'x'), std::string("\r\n\0\xff", 4)};
    std::string wire;
    for (size_t i = 0; i < payloads.size(); ++i) wire += frame::encode(static_cast<uint16_t>(i), 0, payloads[i]);

    std::thread writer([&] {
        for (char c : wire) write_all(sv[0], &c, 1); // worst-case chunking: one byte at a time
        ::shutdown(sv[0], SHUT_WR);
    });

    frame::Decoder dec;
    size_t got = 0;
    char buf[4096];
    for (;;) {
        ssize_t n = ::recv(sv[1], buf, sizeof buf, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            std::perror("recv");
            return 1;
        }
        if (n == 0) break; // writer finished
        dec.feed(buf, static_cast<size_t>(n));
        frame::Header h;
        std::string payload;
        while (dec.next(h, payload)) {
            assert(static_cast<size_t>(h.type) == got);
            assert(payload == payloads[got]);
            std::printf("frame %u: %u bytes payload, ok\n", static_cast<unsigned>(h.type), h.length);
            ++got;
        }
    }
    writer.join();
    assert(got == payloads.size());
    ::close(sv[0]);
    ::close(sv[1]);
    std::puts("framing round trip passed");
    return 0;
}

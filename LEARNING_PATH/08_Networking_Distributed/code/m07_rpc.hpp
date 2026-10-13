// m07_rpc.hpp - a tiny binary RPC layer over TCP. Header-only, C++17, Linux.
//
// Wire format: every message is one frame.
//
//   offset size  field        notes
//   0      2     magic        0x5250 ('R','P') - catches a client speaking the wrong protocol
//   2      1     version      1
//   3      1     type         1 = request, 2 = response
//   4      4     request_id   client-chosen; the response carries the same id (correlation)
//   8      2     method_id    which procedure
//   10     2     status       0 in requests; Status code in responses
//   12     4     payload_len  bytes following the 16-byte header; max 16 MiB
//   16     ...   payload      method-specific, encoded with Writer/Reader below
//
// All integers big-endian. The payload is a hand-rolled TLV-free encoding: fields in a fixed
// order per method, strings as u32 length + bytes. This is what "serialization" means at the
// bottom; protobuf/flatbuffers automate exactly this plus schema evolution.
#pragma once

#include "m03_socket.hpp"

#include <cstdint>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace rpc {

constexpr uint16_t kMagic = 0x5250;
constexpr uint8_t kVersion = 1;
constexpr size_t kHeaderSize = 16;
constexpr uint32_t kMaxPayload = 16u * 1024 * 1024;

enum class Type : uint8_t { Request = 1, Response = 2 };

enum class Status : uint16_t {
    Ok = 0,
    UnknownMethod = 1, // permanent: do not retry
    BadRequest = 2,    // permanent: malformed payload
    Internal = 3,      // server-side exception: retry only if the call is idempotent
    Timeout = 4,       // client-side: no reply within deadline (outcome UNKNOWN)
    Unavailable = 5,   // transport failure (connect/reset); retry with backoff
    Overloaded = 6,    // server shed the request; retry with backoff
};

inline const char* status_name(Status s) {
    switch (s) {
    case Status::Ok: return "OK";
    case Status::UnknownMethod: return "UNKNOWN_METHOD";
    case Status::BadRequest: return "BAD_REQUEST";
    case Status::Internal: return "INTERNAL";
    case Status::Timeout: return "TIMEOUT";
    case Status::Unavailable: return "UNAVAILABLE";
    case Status::Overloaded: return "OVERLOADED";
    }
    return "?";
}

struct Frame {
    Type type = Type::Request;
    uint32_t request_id = 0;
    uint16_t method_id = 0;
    Status status = Status::Ok;
    std::string payload;
};

// ---------------------------------------------------------------------------------------------
// Payload encoding
// ---------------------------------------------------------------------------------------------
class Writer {
public:
    void u8(uint8_t v) { buf_.push_back(static_cast<char>(v)); }
    void u16(uint16_t v) { u8(static_cast<uint8_t>(v >> 8)); u8(static_cast<uint8_t>(v)); }
    void u32(uint32_t v) { u16(static_cast<uint16_t>(v >> 16)); u16(static_cast<uint16_t>(v)); }
    void u64(uint64_t v) { u32(static_cast<uint32_t>(v >> 32)); u32(static_cast<uint32_t>(v)); }
    void i64(int64_t v) { u64(static_cast<uint64_t>(v)); } // two's complement round-trips exactly
    void str(std::string_view s) {
        if (s.size() > kMaxPayload) throw std::length_error("string too long");
        u32(static_cast<uint32_t>(s.size()));
        buf_.append(s.data(), s.size());
    }
    const std::string& data() const { return buf_; }
    std::string take() { return std::move(buf_); }

private:
    std::string buf_;
};

// Every read is bounds-checked. A malformed payload throws BadPayload, which the dispatcher turns
// into Status::BadRequest. Never trust a length that came off the wire.
struct BadPayload : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Reader {
public:
    explicit Reader(std::string_view s) : p_(s.data()), end_(s.data() + s.size()) {}
    uint8_t u8() {
        need(1);
        return static_cast<uint8_t>(*p_++);
    }
    uint16_t u16() { uint16_t hi = u8(); return static_cast<uint16_t>((hi << 8) | u8()); }
    uint32_t u32() { uint32_t hi = u16(); return (hi << 16) | u16(); }
    uint64_t u64() { uint64_t hi = u32(); return (hi << 32) | u32(); }
    int64_t i64() { return static_cast<int64_t>(u64()); }
    std::string str() {
        uint32_t n = u32();
        need(n);
        std::string s(p_, n);
        p_ += n;
        return s;
    }
    bool done() const { return p_ == end_; }

private:
    void need(size_t n) const {
        if (static_cast<size_t>(end_ - p_) < n) throw BadPayload("payload truncated");
    }
    const char* p_;
    const char* end_;
};

// ---------------------------------------------------------------------------------------------
// Frame I/O
// ---------------------------------------------------------------------------------------------
inline void encode_header(const Frame& f, unsigned char out[kHeaderSize]) {
    uint16_t magic = htons(kMagic);
    uint32_t id = htonl(f.request_id);
    uint16_t method = htons(f.method_id);
    uint16_t status = htons(static_cast<uint16_t>(f.status));
    uint32_t len = htonl(static_cast<uint32_t>(f.payload.size()));
    std::memcpy(out + 0, &magic, 2);
    out[2] = kVersion;
    out[3] = static_cast<unsigned char>(f.type);
    std::memcpy(out + 4, &id, 4);
    std::memcpy(out + 8, &method, 2);
    std::memcpy(out + 10, &status, 2);
    std::memcpy(out + 12, &len, 4);
}

// Returns payload length. Throws on bad magic/version/length: the stream cannot be trusted.
inline uint32_t decode_header(const unsigned char in[kHeaderSize], Frame& f) {
    uint16_t magic, method, status;
    uint32_t id, len;
    std::memcpy(&magic, in + 0, 2);
    std::memcpy(&id, in + 4, 4);
    std::memcpy(&method, in + 8, 2);
    std::memcpy(&status, in + 10, 2);
    std::memcpy(&len, in + 12, 4);
    if (ntohs(magic) != kMagic) throw std::runtime_error("rpc: bad magic (peer is not speaking this protocol)");
    if (in[2] != kVersion) throw std::runtime_error("rpc: unsupported version " + std::to_string(in[2]));
    if (in[3] != 1 && in[3] != 2) throw std::runtime_error("rpc: bad frame type");
    len = ntohl(len);
    if (len > kMaxPayload) throw std::runtime_error("rpc: payload too large");
    f.type = static_cast<Type>(in[3]);
    f.request_id = ntohl(id);
    f.method_id = ntohs(method);
    f.status = static_cast<Status>(ntohs(status));
    return len;
}

// One send() for header + payload: no Nagle/delayed-ACK stall, no interleaving window.
inline void write_frame(int fd, const Frame& f) {
    if (f.payload.size() > kMaxPayload) throw std::length_error("rpc: payload too large");
    std::string wire;
    wire.resize(kHeaderSize);
    encode_header(f, reinterpret_cast<unsigned char*>(&wire[0]));
    wire += f.payload;
    net::write_all(fd, wire);
}

// Returns false on clean EOF between frames. Throws on error or truncated frame.
inline bool read_frame(int fd, Frame& f) {
    unsigned char hdr[kHeaderSize];
    if (!net::read_exact(fd, hdr, kHeaderSize)) return false;
    uint32_t len = decode_header(hdr, f);
    f.payload.resize(len);
    if (len > 0 && !net::read_exact(fd, &f.payload[0], len))
        throw std::runtime_error("rpc: EOF inside payload");
    return true;
}

// ---------------------------------------------------------------------------------------------
// Server side: dispatch table
// ---------------------------------------------------------------------------------------------
// A handler decodes its arguments from `req`, writes its result to `resp`, returns a Status.
// Throwing BadPayload => BadRequest. Any other exception => Internal, message in payload.
using Handler = std::function<Status(Reader& req, Writer& resp)>;

class Dispatcher {
public:
    void add(uint16_t method_id, Handler h) { table_[method_id] = std::move(h); }

    Frame handle(const Frame& req) const {
        Frame resp;
        resp.type = Type::Response;
        resp.request_id = req.request_id; // correlation: the client matches on this
        resp.method_id = req.method_id;
        auto it = table_.find(req.method_id);
        if (it == table_.end()) {
            resp.status = Status::UnknownMethod;
            resp.payload = "unknown method " + std::to_string(req.method_id);
            return resp;
        }
        Reader r(req.payload);
        Writer w;
        try {
            resp.status = it->second(r, w);
            resp.payload = w.take();
        } catch (const BadPayload& e) {
            resp.status = Status::BadRequest;
            resp.payload = e.what();
        } catch (const std::exception& e) {
            // Error propagation: the exception does not cross the wire, its text does, with a
            // status the client can classify. The server keeps running.
            resp.status = Status::Internal;
            resp.payload = e.what();
        }
        return resp;
    }

private:
    std::unordered_map<uint16_t, Handler> table_;
};

// Serve one connection until EOF or a framing error. Blocking; call from a worker thread.
inline void serve_connection(int fd, const Dispatcher& d) {
    Frame req;
    while (read_frame(fd, req)) {
        if (req.type != Type::Request) throw std::runtime_error("rpc: expected a request frame");
        write_frame(fd, d.handle(req));
    }
}

// ---------------------------------------------------------------------------------------------
// Client side: synchronous stub transport
// ---------------------------------------------------------------------------------------------
struct Result {
    Status status = Status::Unavailable;
    std::string payload; // method result on Ok, error text otherwise
};

class Client {
public:
    Client(const char* host, const char* port) : fd_(net::connect_tcp(host, port)) {
        net::set_nodelay(fd_.get());
    }

    // One call at a time per Client (pipelining with multiple outstanding ids is an exercise).
    Result call(uint16_t method_id, std::string payload, int timeout_ms) {
        Result res;
        if (!fd_) {
            res.payload = "connection is closed";
            return res; // Unavailable
        }
        Frame req;
        req.type = Type::Request;
        req.request_id = next_id_++;
        req.method_id = method_id;
        req.payload = std::move(payload);
        try {
            write_frame(fd_.get(), req);
            for (;;) {
                if (!net::wait_readable(fd_.get(), timeout_ms)) {
                    // Deadline passed. We do NOT know whether the server executed the call.
                    // A late reply for this id would be matched to a future request, so the
                    // only safe move is to abandon the connection.
                    fd_.reset();
                    res.status = Status::Timeout;
                    res.payload = "no reply within deadline; outcome unknown";
                    return res;
                }
                Frame resp;
                if (!read_frame(fd_.get(), resp)) {
                    fd_.reset();
                    res.payload = "server closed connection";
                    return res; // Unavailable
                }
                if (resp.type != Type::Response) throw std::runtime_error("rpc: expected a response frame");
                if (resp.request_id != req.request_id) continue; // stale reply (cannot happen here; defensive)
                res.status = resp.status;
                res.payload = std::move(resp.payload);
                return res;
            }
        } catch (const std::exception& e) {
            fd_.reset();
            res.status = Status::Unavailable;
            res.payload = e.what();
            return res;
        }
    }

    bool connected() const { return fd_.valid(); }

private:
    net::Fd fd_;
    uint32_t next_id_ = 1;
};

} // namespace rpc

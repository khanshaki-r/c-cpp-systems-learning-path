// tcpkv_cli: interactive shell and one-shot client, in the spirit of redis-cli.
//
//   tcpkv_cli                       interactive session against 127.0.0.1:7379
//   tcpkv_cli -p 7400 set k hello   one-shot command (the value is everything after the key)

#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "tcpkv/client.hpp"

namespace {

void print_usage(std::ostream& os) {
    os << "usage: tcpkv_cli [-h host] [-p port] [command [args...]]\n"
          "  with no command, starts an interactive session (type 'exit' to leave)\n"
          "  SET takes the rest of the line as the value, spaces included\n";
}

std::string upper(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return text;
}

// Prints bytes the way redis-cli does: quoted, with escapes for anything that is not printable.
std::string quoted(std::string_view bytes) {
    static const char kHex[] = "0123456789abcdef";
    std::string out = "\"";
    for (const char c : bytes) {
        const auto u = static_cast<unsigned char>(c);
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (u >= 0x20 && u < 0x7F) {
                    out += c;
                } else {
                    out += "\\x";
                    out += kHex[u >> 4];
                    out += kHex[u & 0x0F];
                }
        }
    }
    out += '"';
    return out;
}

void print_reply(const tcpkv::Reply& reply, std::ostream& os) {
    using Kind = tcpkv::Reply::Kind;
    switch (reply.kind) {
        case Kind::Status: os << reply.text << '\n'; break;
        case Kind::Error: os << "(error) " << reply.text << '\n'; break;
        case Kind::Integer: os << "(integer) " << reply.integer << '\n'; break;
        case Kind::Bulk: os << quoted(reply.text) << '\n'; break;
        case Kind::Null: os << "(nil)\n"; break;
        case Kind::Array:
            if (reply.array.empty()) {
                os << "(empty list)\n";
            }
            for (std::size_t i = 0; i < reply.array.size(); ++i) {
                os << (i + 1) << ") " << quoted(reply.array[i]) << '\n';
            }
            break;
    }
}

std::vector<std::string> split(const std::string& line) {
    std::istringstream in(line);
    std::vector<std::string> tokens;
    std::string token;
    while (in >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

// Executes one line. Returns false when the session should end.
bool run_line(tcpkv::Client& client, const std::string& line, std::ostream& os) {
    std::istringstream in(line);
    std::string command;
    if (!(in >> command)) {
        return true;  // blank line
    }
    const std::string name = upper(command);
    if (name == "EXIT") {
        try {
            client.quit();
        } catch (const std::exception&) {
            // Leaving anyway.
        }
        return false;
    }

    try {
        if (name == "SET") {
            std::string key;
            if (!(in >> key)) {
                os << "(error) usage: SET key value\n";
                return true;
            }
            std::string value;
            std::getline(in, value);  // the rest of the line, starting with the separating space
            if (!value.empty() && value.front() == ' ') {
                value.erase(0, 1);
            }
            client.set(key, value);
            os << "OK\n";
            return true;
        }

        std::vector<std::string> tokens = split(line);
        tokens[0] = name;
        const tcpkv::Reply reply = client.execute(tokens);
        print_reply(reply, os);
        return !(name == "QUIT" && reply.kind == tcpkv::Reply::Kind::Status);
    } catch (const tcpkv::ClientError& e) {
        os << "(error) " << e.what() << '\n';
        return true;
    } catch (const std::system_error& e) {
        os << "(connection error) " << e.what() << '\n';
        return false;
    }
}

bool parse_port(std::string_view text, std::uint16_t& out) {
    unsigned value = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc() || result.ptr != end || value > 65535) {
        return false;
    }
    out = static_cast<std::uint16_t>(value);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    std::uint16_t port = 7379;

    int i = 1;
    for (; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help") {
            print_usage(std::cout);
            return 0;
        }
        if (arg == "-h" || arg == "--host") {
            if (i + 1 >= argc) {
                print_usage(std::cerr);
                return 2;
            }
            host = argv[++i];
        } else if (arg == "-p" || arg == "--port") {
            if (i + 1 >= argc || !parse_port(argv[i + 1], port)) {
                print_usage(std::cerr);
                return 2;
            }
            ++i;
        } else {
            break;  // first non-option: the command starts here
        }
    }

    try {
        tcpkv::Client client(host, port);

        if (i < argc) {
            // One-shot mode: rebuild the command line so SET's value keeps its spaces.
            std::string line;
            for (int j = i; j < argc; ++j) {
                if (j != i) {
                    line += ' ';
                }
                line += argv[j];
            }
            run_line(client, line, std::cout);
            return 0;
        }

        std::string line;
        std::cout << "tcpkv> " << std::flush;
        while (std::getline(std::cin, line)) {
            if (!run_line(client, line, std::cout)) {
                break;
            }
            std::cout << "tcpkv> " << std::flush;
        }
        return 0;
    } catch (const std::system_error& e) {
        std::cerr << "could not connect to " << host << ":" << port << ": " << e.what() << '\n';
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}

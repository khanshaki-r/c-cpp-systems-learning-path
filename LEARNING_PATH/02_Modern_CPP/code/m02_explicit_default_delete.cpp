// m02_explicit_default_delete.cpp
// explicit constructors, =default, =delete, and the rule of zero / three / five at a glance.
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread m02_explicit_default_delete.cpp
#include <iostream>
#include <string>
#include <utility>

class Meters {
public:
    explicit Meters(double v) : value_(v) {}   // explicit: no silent double -> Meters conversion
    double value() const { return value_; }
private:
    double value_;
};

void print(Meters m) { std::cout << m.value() << " m\n"; }

// A type that must never be copied: two copies would close the same fd twice.
// This is "rule of five": we manage a resource, so we write all five special members.
class Connection {
public:
    explicit Connection(int fd) : fd_(fd) {}
    ~Connection() { reset(); }
    Connection(const Connection&) = delete;               // copying is a bug: make it a compile error
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}   // steal, leave source empty
    Connection& operator=(Connection&& o) noexcept {
        if (this != &o) {
            reset();                                      // release what we held
            fd_ = std::exchange(o.fd_, -1);
        }
        return *this;
    }
    int fd() const { return fd_; }
private:
    void reset() noexcept {
        if (fd_ != -1) { std::cout << "close(" << fd_ << ")\n"; fd_ = -1; }   // real code: ::close(fd_)
    }
    int fd_;
};

// Rule of zero: owns nothing special, so write NO special members. The compiler generates
// copy/move/destructor that are correct and trivially fast.
struct Point {
    int x = 0;
    int y = 0;
};

struct Widget {
    Widget() = default;            // adding Widget(int) would remove the default ctor; =default brings it back
    Widget(int id) : id_(id) {}    // implicit: Widget w = 42; compiles. Add 'explicit' if that is unwanted.
    int id_ = 0;
};

int main() {
    print(Meters{3.5});
    // print(3.5);                 // error: explicit constructor blocks the implicit conversion

    Connection a{7};
    // Connection b = a;           // error: use of deleted copy constructor
    Connection c = std::move(a);   // ownership of fd 7 moves to c; a is now empty (-1)
    std::cout << "a.fd=" << a.fd() << " c.fd=" << c.fd() << '\n';
    Connection d{9};
    d = std::move(c);              // closes 9, takes 7
    std::cout << "c.fd=" << c.fd() << " d.fd=" << d.fd() << '\n';

    Point p{1, 2};
    Point q = p;                   // compiler-generated memberwise copy: a plain 8-byte copy
    std::cout << q.x + q.y << '\n';

    Widget w;
    Widget w2{42};
    std::cout << w.id_ << ' ' << w2.id_ << '\n';
    return 0;
}   // d's destructor closes fd 7. Nothing is closed twice, nothing is leaked.

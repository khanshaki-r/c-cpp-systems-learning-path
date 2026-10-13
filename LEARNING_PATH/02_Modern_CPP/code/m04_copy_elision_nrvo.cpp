// m04_copy_elision_nrvo.cpp
// Watch exactly when copies and moves happen: elision, NRVO, parameter passing conventions.
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread m04_copy_elision_nrvo.cpp
// Try also: -fno-elide-constructors to see what the compiler normally removes.
#include <iostream>
#include <string>
#include <utility>
#include <vector>

struct Loud {   // every special member announces itself
    std::string tag;
    Loud(std::string t) : tag(std::move(t))            { std::cout << "ctor  " << tag << '\n'; }
    Loud(const Loud& o) : tag(o.tag)                   { std::cout << "COPY  " << tag << '\n'; }
    Loud(Loud&& o) noexcept : tag(std::move(o.tag))    { std::cout << "MOVE  " << tag << '\n'; }
    Loud& operator=(const Loud& o)          { tag = o.tag;            std::cout << "copy= " << tag << '\n'; return *this; }
    Loud& operator=(Loud&& o) noexcept      { tag = std::move(o.tag); std::cout << "move= " << tag << '\n'; return *this; }
    ~Loud() = default;
};

Loud make_prvalue() { return Loud("prvalue"); }   // C++17: GUARANTEED elision. Zero copies, zero moves.

Loud make_nrvo() {                                // Named Return Value Optimisation: allowed, not guaranteed.
    Loud l("nrvo");                               // GCC does it at every optimisation level.
    l.tag += "!";
    return l;                                     // if not elided, this is a MOVE (never a copy)
}

Loud make_no_nrvo(bool flag) {                    // two different named objects may be returned:
    Loud a("a"), b("b");                          // NRVO is impossible, the compiler falls back to MOVE
    if (flag) return a;
    return b;
}

void by_value(Loud l)       { std::cout << "  by_value got " << l.tag << '\n'; }           // copy or move, caller decides
void by_cref(const Loud& l) { std::cout << "  by_cref got " << l.tag << '\n'; }            // nothing happens
void by_rref(Loud&& l)      { Loud mine = std::move(l); std::cout << "  by_rref took " << mine.tag << '\n'; }

struct Person {
    std::string name;
    explicit Person(std::string n) : name(std::move(n)) {}   // "sink" parameter: take by value, move into place.
};                                                            // Caller passes lvalue -> 1 copy; rvalue -> 0 copies.

int main() {
    std::cout << "-- prvalue (guaranteed elision)\n";  Loud a = make_prvalue();
    std::cout << "-- nrvo\n";                            Loud b = make_nrvo();
    std::cout << "-- no nrvo possible\n";                Loud c = make_no_nrvo(true);

    std::cout << "-- by_value(lvalue): COPY\n";          by_value(a);
    std::cout << "-- by_value(rvalue): MOVE\n";          by_value(std::move(a));
    std::cout << "-- by_cref: nothing\n";                by_cref(b);
    std::cout << "-- by_rref: move happens inside\n";    by_rref(std::move(b));

    // Moved-from objects are valid but their value is unspecified. For std::string it is usually
    // empty, but do not rely on it. The only safe operations: assign, destroy, or query without assumptions.
    std::cout << "-- moved-from: a.tag='" << a.tag << "' b.tag='" << b.tag << "'\n";
    a = Loud("fresh");          // assigning to a moved-from object is always fine (move=)
    std::cout << "a.tag='" << a.tag << "'\n";

    std::cout << "-- vector push_back vs emplace_back\n";
    std::vector<Loud> v;
    v.reserve(3);               // avoid a reallocation, which would add MOVE lines to the output
    v.push_back(c);             // COPY
    v.push_back(std::move(c));  // MOVE
    v.emplace_back("in place"); // ctor only: constructed directly in the vector's storage

    std::cout << "-- sink parameter\n";
    std::string n = "shakir";
    Person p1(n);               // one copy (into the parameter), then a move into the member
    Person p2(std::move(n));    // zero copies
    std::cout << p1.name << ' ' << p2.name << '\n';
    return 0;
}

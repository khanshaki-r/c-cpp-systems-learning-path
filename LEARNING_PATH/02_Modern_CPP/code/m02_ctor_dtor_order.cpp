// m02_ctor_dtor_order.cpp
// Object lifetime: construction and destruction order for globals, locals, statics, bases and members.
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread m02_ctor_dtor_order.cpp
#include <iostream>
#include <string>
#include <utility>

struct Tracer {   // prints when it is born and when it dies, so we can watch the order
    std::string name;
    explicit Tracer(std::string n) : name(std::move(n)) { std::cout << "  construct " << name << '\n'; }
    ~Tracer() { std::cout << "  destroy   " << name << '\n'; }
    Tracer(const Tracer&) = delete;              // not needed for this demo; deleting avoids accidental copies
    Tracer& operator=(const Tracer&) = delete;
};

struct Base {
    Tracer t{"Base::t"};                         // default member initialiser
    Base()  { std::cout << "  Base() body\n"; }  // body runs AFTER all members are constructed
    ~Base() { std::cout << "  ~Base() body\n"; } // body runs BEFORE members are destroyed
};

struct Derived : Base {
    Tracer a{"Derived::a"};   // members are constructed in DECLARATION order ...
    Tracer b{"Derived::b"};   // ... regardless of the order written in a member-initialiser list (-Wreorder warns)
    Derived()  { std::cout << "  Derived() body\n"; }
    ~Derived() { std::cout << "  ~Derived() body\n"; }
};

struct Account {
    const std::string id;     // const member: can ONLY be set in the initialiser list
    long long balance;
    // Member initialiser list: constructs id directly from i. Assigning in the body instead would
    // default-construct id first and then assign (wasted work), and is impossible for const/reference members.
    Account(std::string i, long long b) : id(std::move(i)), balance(b) {}
};

Tracer g_global{"global"};    // constructed before main() starts, destroyed after main() returns

void scope_demo() {
    std::cout << "enter scope_demo\n";
    Tracer first{"first"};
    Tracer second{"second"};
    std::cout << "leave scope_demo\n";
}   // destroyed in REVERSE order of construction: second, then first

int main() {
    std::cout << "main begins\n";
    scope_demo();
    {
        std::cout << "constructing Derived\n";
        Derived d;                                 // order: Base::t, Base() body, Derived::a, Derived::b, Derived() body
        std::cout << "d alive, sizeof=" << sizeof d << '\n';
    }   // order: ~Derived() body, destroy b, destroy a, ~Base() body, destroy Base::t

    Account acc("ACC-9", 100);
    std::cout << acc.id << ' ' << acc.balance << '\n';

    static Tracer s{"static local"};   // constructed on the first pass through this line, destroyed after main returns
    std::cout << "main ends\n";
    return 0;
}   // after main: "static local" destroyed first (constructed last), then "global"

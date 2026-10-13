// m09_vtable_demo.cpp
// Virtual dispatch under the hood: vptr, vtables, object size, qualified calls, slicing.
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread m09_vtable_demo.cpp
// Extra: g++ -std=c++17 -fdump-lang-class m09_vtable_demo.cpp   -> see *.class file with the vtable layouts
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

struct Plain { int a; int b; };   // no virtual functions: no vptr, sizeof == 8

class Shape {
public:
    virtual ~Shape() = default;           // virtual dtor: 'delete shapePtr' runs the derived destructor
    virtual double area() const = 0;      // pure virtual: Shape is abstract, cannot be instantiated
    virtual const char* name() const { return "shape"; }
    int id = 0;
};

class Circle final : public Shape {       // final: nobody derives further, so calls on a Circle can be devirtualised
public:
    explicit Circle(double r) : r_(r) {}
    double area() const override { return 3.14159 * r_ * r_; }
    const char* name() const override { return "circle"; }
private:
    double r_;
};

class Square : public Shape {
public:
    explicit Square(double s) : s_(s) {}
    double area() const override { return s_ * s_; }
    // name() is NOT overridden: Square's vtable slot for name() points at Shape::name
private:
    double s_;
};

// On the Itanium C++ ABI (GCC, Clang on Linux) the vptr is the first 8 bytes of a polymorphic object.
// memcpy reads those bytes without violating strict aliasing.
std::uintptr_t read_vptr(const Shape& s) {
    std::uintptr_t v = 0;
    std::memcpy(&v, &s, sizeof v);
    return v;
}

struct Animal {
    virtual ~Animal() = default;
    virtual const char* sound() const { return "..."; }
};
struct Dog : Animal {
    const char* sound() const override { return "woof"; }
};
void slice(Animal a)          { std::cout << "  by value: " << a.sound() << '\n'; }   // copies ONLY the Animal part
void no_slice(const Animal& a) { std::cout << "  by ref:   " << a.sound() << '\n'; }

int main() {
    std::cout << "sizeof(Plain)=" << sizeof(Plain) << "  sizeof(Shape)=" << sizeof(Shape)
              << " (8-byte vptr + 4-byte id + 4 padding)  sizeof(Circle)=" << sizeof(Circle) << '\n';

    Circle c1(1.0), c2(2.0);
    Square sq(3.0);
    std::cout << std::hex;
    std::cout << "vptr c1=0x" << read_vptr(c1) << " c2=0x" << read_vptr(c2) << "  (same class -> same vtable)\n";
    std::cout << "vptr sq=0x" << read_vptr(sq) << "  (different class -> different vtable)\n";
    std::cout << std::dec;

    // Dynamic dispatch: the call site does not know the concrete type. It loads vptr, loads slot, calls.
    std::vector<std::unique_ptr<Shape>> shapes;
    shapes.push_back(std::make_unique<Circle>(1.5));
    shapes.push_back(std::make_unique<Square>(2.0));
    for (const auto& s : shapes) std::cout << s->name() << " area=" << s->area() << '\n';
    // When 'shapes' is destroyed: unique_ptr<Shape> calls delete on a Shape*; the virtual destructor
    // makes that run ~Circle / ~Square. Without 'virtual ~Shape()' this would be undefined behaviour.

    Shape& ref = c1;
    std::cout << "static type Shape&, dynamic type " << ref.name() << '\n';
    std::cout << "qualified call skips the vtable: " << ref.Shape::name() << '\n';

    std::cout << "slicing:\n";
    Dog d;
    slice(d);      // prints "..." : the Dog part was cut off, and the copy's vptr is Animal's
    no_slice(d);   // prints "woof"
    return 0;
}

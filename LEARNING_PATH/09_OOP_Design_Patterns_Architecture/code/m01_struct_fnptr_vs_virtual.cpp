// m01_struct_fnptr_vs_virtual.cpp
// The same "Store" abstraction written three ways:
//   1. C style: struct of function pointers + void* context (what Shakir writes today)
//   2. C++ class with virtual functions (the compiler builds the function-pointer table for you)
//   3. Template (compile-time polymorphism: no table, no indirect call, but one copy per type)
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -o m01a m01_struct_fnptr_vs_virtual.cpp

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>

// ---------------------------------------------------------------------------
// 1. C style. This is valid C++ too, but it is how you would write it in C.
// ---------------------------------------------------------------------------
namespace c_style {

// The "interface": a table of function pointers. Every store fills one in.
struct store_ops {
    int (*put)(void* self, const char* key, const char* value);
    const char* (*get)(void* self, const char* key);
    void (*destroy)(void* self);
};

// The "object": ops table + opaque state. Caller never looks inside state.
struct store {
    const store_ops* ops;
    void* state;
};

// --- One implementation: fixed-size array store ---
struct array_state {
    char keys[8][32];
    char values[8][64];
    int count;
};

static int array_put(void* self, const char* key, const char* value) {
    auto* s = static_cast<array_state*>(self);
    for (int i = 0; i < s->count; ++i) {
        if (std::strcmp(s->keys[i], key) == 0) {
            std::snprintf(s->values[i], sizeof s->values[i], "%s", value);
            return 0;
        }
    }
    if (s->count == 8) return -1;  // full
    std::snprintf(s->keys[s->count], sizeof s->keys[0], "%s", key);
    std::snprintf(s->values[s->count], sizeof s->values[0], "%s", value);
    ++s->count;
    return 0;
}

static const char* array_get(void* self, const char* key) {
    auto* s = static_cast<array_state*>(self);
    for (int i = 0; i < s->count; ++i)
        if (std::strcmp(s->keys[i], key) == 0) return s->values[i];
    return nullptr;
}

static void array_destroy(void* self) { std::free(self); }

static const store_ops array_ops = {array_put, array_get, array_destroy};

// "Constructor": allocate state, wire the table.
static store array_store_create() {
    auto* st = static_cast<array_state*>(std::calloc(1, sizeof(array_state)));
    return store{&array_ops, st};
}

// Generic code written against the table. Notice: every call is an indirect
// call through ops->..., and the compiler cannot inline it because it does
// not know which function the pointer holds.
static void exercise(store* s) {
    s->ops->put(s->state, "acct:1001", "balance=500");
    const char* v = s->ops->get(s->state, "acct:1001");
    std::printf("[c-style]   acct:1001 -> %s\n", v ? v : "(null)");
}

}  // namespace c_style

// ---------------------------------------------------------------------------
// 2. C++ with virtual functions. The compiler generates:
//    - a per-class table of function pointers (the vtable), filled in once
//    - a hidden pointer in each object to that table (the vptr)
//    - at each call site: load vptr, load slot, indirect call
//    That is exactly store_ops + store::ops, done for you and type-checked.
// ---------------------------------------------------------------------------
namespace cpp_virtual {

class Store {
public:
    virtual ~Store() = default;  // deleting through Store* must run the right destructor
    virtual bool put(const std::string& key, const std::string& value) = 0;
    virtual std::optional<std::string> get(const std::string& key) const = 0;
};

class MapStore final : public Store {
public:
    bool put(const std::string& key, const std::string& value) override {
        data_[key] = value;
        return true;
    }
    std::optional<std::string> get(const std::string& key) const override {
        auto it = data_.find(key);
        if (it == data_.end()) return std::nullopt;
        return it->second;
    }

private:
    std::map<std::string, std::string> data_;  // private: nobody can corrupt it from outside
};

// Generic code written against the interface. Same shape as c_style::exercise.
void exercise(Store& s) {
    s.put("acct:1001", "balance=500");
    auto v = s.get("acct:1001");
    std::printf("[virtual]   acct:1001 -> %s\n", v ? v->c_str() : "(none)");
}

}  // namespace cpp_virtual

// ---------------------------------------------------------------------------
// 3. Template. No base class. The requirement "has put() and get()" is checked
//    at compile time when exercise<T> is instantiated. The call is direct and
//    inlinable, but exercise is compiled once per store type.
// ---------------------------------------------------------------------------
namespace cpp_template {

class MapStore {  // no inheritance, no virtual, no vptr: sizeof == sizeof(std::map)
public:
    bool put(const std::string& key, const std::string& value) {
        data_[key] = value;
        return true;
    }
    std::optional<std::string> get(const std::string& key) const {
        auto it = data_.find(key);
        if (it == data_.end()) return std::nullopt;
        return it->second;
    }

private:
    std::map<std::string, std::string> data_;
};

template <class StoreT>
void exercise(StoreT& s) {
    s.put("acct:1001", "balance=500");
    auto v = s.get("acct:1001");
    std::printf("[template]  acct:1001 -> %s\n", v ? v->c_str() : "(none)");
}

}  // namespace cpp_template

int main() {
    // 1. C style: manual create / destroy, no type safety on state.
    c_style::store s = c_style::array_store_create();
    c_style::exercise(&s);
    s.ops->destroy(s.state);  // forget this and you leak; call it twice and you crash

    // 2. Virtual: unique_ptr runs the virtual destructor for us.
    std::unique_ptr<cpp_virtual::Store> vs = std::make_unique<cpp_virtual::MapStore>();
    cpp_virtual::exercise(*vs);

    // 3. Template: a plain value on the stack. Destroyed at end of scope.
    cpp_template::MapStore ts;
    cpp_template::exercise(ts);

    // Size check: the virtual version carries a hidden vptr.
    std::printf("sizeof(cpp_virtual::MapStore)  = %zu\n", sizeof(cpp_virtual::MapStore));
    std::printf("sizeof(cpp_template::MapStore) = %zu\n", sizeof(cpp_template::MapStore));
    return 0;
}

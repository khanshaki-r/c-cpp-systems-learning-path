// m08_tag_dispatch_pipeline.cpp
// Two compile-time techniques:
//
// 1. TAG DISPATCH: choose an overload by passing an empty "tag" type whose only
//    job is to select. The standard library does this with iterator_category
//    (std::advance picks O(1) for random access, O(n) otherwise). You use it to
//    specialise behaviour per type trait without if-constexpr chains or
//    partial specialisation of a whole class.
//
// 2. RANGES-STYLE PIPELINE: compose transformations with operator| into a
//    lazy chain that is fully inlined. The C++20 <ranges> library does this
//    properly; here is a small C++17 version to show the mechanism and the
//    cost model: zero runtime dispatch, one type per pipeline.
//
// C equivalent: #if/#ifdef or a switch on a type code for dispatch; nested
// loops for the pipeline. Templates do it with checking and inlining.
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -o m08d m08_tag_dispatch_pipeline.cpp

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <list>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// 1. Tag dispatch
// ---------------------------------------------------------------------------

// Our own tags. Empty structs; sizeof == 1; no runtime content.
struct trivially_copyable_tag {};
struct needs_construction_tag {};

template <class T>
using copy_category_t = std::conditional_t<std::is_trivially_copyable_v<T>, trivially_copyable_tag, needs_construction_tag>;

// Two implementations, selected by tag. The tag parameter is unnamed: it exists
// only to pick the overload.
template <class T>
void copy_records_impl(const T* src, T* dst, std::size_t n, trivially_copyable_tag) {
    std::printf("  [memcpy path for %zu x %zu bytes]\n", n, sizeof(T));
    std::memcpy(static_cast<void*>(dst), static_cast<const void*>(src), n * sizeof(T));
}

template <class T>
void copy_records_impl(const T* src, T* dst, std::size_t n, needs_construction_tag) {
    std::printf("  [element-wise copy path for %zu elements]\n", n);
    for (std::size_t i = 0; i < n; ++i) dst[i] = src[i];
}

// The public entry: constructs the right tag and dispatches. Resolved at compile time.
template <class T>
void copy_records(const T* src, T* dst, std::size_t n) {
    copy_records_impl(src, dst, n, copy_category_t<T>{});
}

// The standard-library example: advance an iterator by n, O(1) if the category allows.
template <class It>
void my_advance_impl(It& it, long n, std::random_access_iterator_tag) {
    std::printf("  [random access: it += n]\n");
    it += n;
}
template <class It>
void my_advance_impl(It& it, long n, std::input_iterator_tag) {   // base of forward/bidirectional: catches all
    std::printf("  [stepping %ld times]\n", n);
    while (n-- > 0) ++it;
}
template <class It>
void my_advance(It& it, long n) {
    my_advance_impl(it, n, typename std::iterator_traits<It>::iterator_category{});   // derived tags convert to base
}

// The C++17 alternative for simple cases: if constexpr. Prefer it when the branches
// are short and live in one function; prefer tags when implementations are large,
// live in different headers, or must be extensible by users adding overloads.
template <class T>
const char* describe_copy() {
    if constexpr (std::is_trivially_copyable_v<T>) return "memcpy-able";
    else return "needs copy constructor";
}

// ---------------------------------------------------------------------------
// 2. A minimal lazy pipeline with operator|
// ---------------------------------------------------------------------------

// A "view" over a range plus a transformation. Each adaptor wraps the previous
// one; nothing runs until you iterate (here: until to_vector or for_each).
template <class Range, class Fn>
struct Mapped {
    Range range;
    Fn fn;
    template <class Out>
    void for_each(Out&& out) const {
        range.for_each([&](const auto& x) { out(fn(x)); });
    }
};

template <class Range, class Pred>
struct Filtered {
    Range range;
    Pred pred;
    template <class Out>
    void for_each(Out&& out) const {
        range.for_each([&](const auto& x) { if (pred(x)) out(x); });
    }
};

// The source: wraps a container by reference.
template <class Container>
struct From {
    const Container& c;
    template <class Out>
    void for_each(Out&& out) const {
        for (const auto& x : c) out(x);
    }
};
template <class C>
From<C> from(const C& c) { return From<C>{c}; }

// Adaptor "closures": carry the function until a range is piped in.
template <class Fn> struct MapAdaptor { Fn fn; };
template <class Pred> struct FilterAdaptor { Pred pred; };
template <class Fn> MapAdaptor<Fn> map(Fn fn) { return {std::move(fn)}; }
template <class Pred> FilterAdaptor<Pred> filter(Pred pred) { return {std::move(pred)}; }

template <class Range, class Fn>
Mapped<Range, Fn> operator|(Range r, MapAdaptor<Fn> m) { return {std::move(r), std::move(m.fn)}; }
template <class Range, class Pred>
Filtered<Range, Pred> operator|(Range r, FilterAdaptor<Pred> f) { return {std::move(r), std::move(f.pred)}; }

// Terminal operations.
template <class Range>
std::vector<std::int64_t> to_vector(const Range& r) {   // kept simple: our pipelines produce int64
    std::vector<std::int64_t> out;
    r.for_each([&](const auto& x) { out.push_back(static_cast<std::int64_t>(x)); });
    return out;
}

template <class Range>
std::int64_t sum(const Range& r) {
    std::int64_t s = 0;
    r.for_each([&](const auto& x) { s += static_cast<std::int64_t>(x); });
    return s;
}

struct Txn {
    std::int64_t amount;
    bool reversed;
};

int main() {
    // --- tag dispatch ---
    std::printf("tag dispatch:\n");
    int a[4] = {1, 2, 3, 4}, b[4] = {};
    copy_records(a, b, 4);                       // trivially copyable -> memcpy path
    std::string s1[2] = {"x", "y"}, s2[2];
    copy_records(s1, s2, 2);                     // std::string -> element-wise
    std::printf("  b[3]=%d s2[1]=%s; int is %s, string %s\n", b[3], s2[1].c_str(), describe_copy<int>(),
                describe_copy<std::string>());

    std::vector<int> v{10, 20, 30, 40, 50};
    std::list<int> l{10, 20, 30, 40, 50};
    auto vi = v.begin();
    auto li = l.begin();
    my_advance(vi, 3);
    my_advance(li, 3);
    std::printf("  vector -> %d, list -> %d\n", *vi, *li);

    // --- pipeline ---
    std::printf("pipeline:\n");
    std::vector<Txn> txns{{500, false}, {-200, false}, {1000, true}, {300, false}, {-50, true}};
    auto pipeline = from(txns)
                  | filter([](const Txn& t) { return !t.reversed; })
                  | map([](const Txn& t) { return t.amount; })
                  | filter([](std::int64_t amt) { return amt > 0; });
    // Nothing has executed yet. The pipeline is a nested struct of 3 adaptors.
    auto credits = to_vector(pipeline);
    std::printf("  positive, non-reversed amounts:");
    for (auto x : credits) std::printf(" %lld", static_cast<long long>(x));
    std::printf("\n  sum = %lld, sizeof(pipeline) = %zu (just the reference + empty lambdas)\n",
                static_cast<long long>(sum(pipeline)), sizeof(pipeline));
    return 0;
}

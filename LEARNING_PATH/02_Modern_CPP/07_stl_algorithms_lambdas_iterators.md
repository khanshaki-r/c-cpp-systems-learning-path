# 7. STL algorithms, lambdas and iterators

**Time:** ~2.5 hours   **Prerequisites:** 06

## 1. In plain words

Most loops you write in C do one of a few things: find something, count something, copy with a
transformation, remove matching elements, sort, sum. `<algorithm>` and `<numeric>` already contain
these loops, written once, tested for decades, and specialised for every container through
iterators. Your job becomes saying *what* you want (`std::count_if(begin, end, is_overdue)`) instead
of *how* to loop.

The missing piece in C was a cheap way to pass "the condition" to such a function. Function
pointers work but cannot carry state. A **lambda** is an anonymous function object that can capture
local variables, so the condition can be written right where it is used.

Analogy: `qsort` with a comparator function is the C version. Lambdas and templates make the
comparator inline, type-safe, and able to see your local variables, and the compiler inlines the whole
thing so it runs as fast as a hand-written loop.

## 2. The real thing

### 2.1 Iterators

An iterator is a generalised pointer: `*it` gives the element, `++it` advances, `it != end` tests.
Raw pointers *are* iterators for arrays. Containers expose `begin()`/`end()`; `end()` is one past the
last element and must not be dereferenced.

Iterator categories decide which algorithms are available and how fast they are:

| Category | Can do | Examples |
|---|---|---|
| Input / Output | read (or write) once, forward only | `istream_iterator`, `back_inserter` |
| Forward | multi-pass, `++` | `forward_list`, `unordered_map` |
| Bidirectional | plus `--` | `list`, `map`, `set` |
| Random access | plus `it + n`, `it[n]`, `it1 - it2`, `<` in O(1) | `vector`, `deque`, `array`, `string`, pointers |

`std::sort` requires random access (so you cannot `std::sort` a `list`; use `list::sort`).
`std::lower_bound` works on forward iterators but is only O(log n) with random access.
`std::distance(a, b)` is O(1) for random access and O(n) otherwise.

### 2.2 The algorithms you will use every week

```cpp
// searching
std::find(b, e, value);  std::find_if(b, e, pred);  std::count_if(b, e, pred);
std::any_of / all_of / none_of(b, e, pred);
std::lower_bound(b, e, value);   // sorted range: first element >= value
std::upper_bound(b, e, value);   // sorted range: first element >  value
std::binary_search(b, e, value); // sorted range: bool
std::min_element / max_element / minmax_element(b, e [, cmp]);

// ordering
std::sort(b, e [, cmp]);          // O(n log n), not stable (introsort)
std::stable_sort(b, e [, cmp]);   // keeps relative order of equal elements, may allocate
std::partial_sort(b, mid, e);     // top-k
std::nth_element(b, nth, e);      // O(n) median / k-th
std::partition / stable_partition(b, e, pred);
std::reverse(b, e);  std::rotate(b, mid, e);  std::unique(b, e);   // unique: on sorted, then erase

// modifying / copying
std::copy / copy_if(b, e, out);  std::transform(b, e, out, f);  std::fill(b, e, v);  std::iota(b, e, start);
std::remove / remove_if(b, e, pred);   // returns new logical end; MUST be followed by erase
std::swap_ranges;  std::move(b, e, out);   // the three-argument std::move moves elements

// numeric
std::accumulate(b, e, init [, op]);    // type of init is the accumulator type
std::reduce (C++17, parallel-friendly), std::inner_product, std::partial_sum, std::adjacent_difference
```

**Erase-remove idiom.** `std::remove_if` cannot change the container's size (it only sees iterators),
so it shifts survivors to the front and returns the new logical end. You then call
`v.erase(new_end, v.end())`. C++20 adds `std::erase_if(v, pred)` to do both.

**Comparators must be strict weak orderings.** `cmp(a, a)` must be false; if `cmp(a, b)` then
`!cmp(b, a)`; transitive. Using `<=` instead of `<` is UB in `std::sort` and crashes libstdc++ with
out-of-bounds reads. The same comparator must be used for sorting and for `lower_bound`.

### 2.3 Lambdas

```cpp
[captures](parameters) specifiers -> return_type { body }
```

- `[x]` capture by copy (at creation time), `[&x]` by reference, `[=]`/`[&]` capture everything used
  by copy/reference, `[this]` the enclosing object pointer, `[n = expr]` init-capture (C++14; also
  how you move something into a lambda: `[p = std::move(ptr)]`).
- Captured-by-copy variables are `const` inside the body unless the lambda is `mutable`.
- `auto` parameters make a **generic lambda** (its `operator()` is a template).
- Return type is deduced; write `-> T` when branches disagree or you return a braced list.
- A lambda with no captures converts to a plain function pointer, so it can be passed to C APIs
  (`qsort`, `pthread_create`, `signal`).

A lambda is sugar for a struct with an `operator()`: the captures are its members, so
`sizeof(lambda)` equals the size of its captures (1 for none). The compiler sees the exact type, so
calls through a template parameter are inlined.

### 2.4 `std::function`

A type-erased wrapper that can hold *any* callable with a given signature: lambda, function pointer,
functor, `std::bind` result. Use it when you must **store** heterogeneous callables (a handler table,
a callback member). Costs: 32 bytes on libstdc++, a heap allocation if the callable does not fit the
internal 16-byte buffer, and one indirect call per invocation (no inlining). Do not use it as a
parameter type when a template parameter would do.

### 2.5 Projections and ranges (preview)

C++20 ranges (`std::ranges::sort(v, {}, &Txn::amount)`) remove the `begin()/end()` boilerplate and
add projections and lazy views. Not available in C++17; know they exist for interviews.

## 3. Code

`code/m07_algorithms_lambdas.cpp` runs through the daily toolkit on a vector of transactions:

```cpp
std::sort(txns.begin(), txns.end(), [](const Txn& a, const Txn& b) {
    if (a.priority != b.priority) return a.priority > b.priority;   // strict: never true for equal elements
    return a.account < b.account;
});

txns.erase(std::remove_if(txns.begin(), txns.end(),
                          [](const Txn& t) { return t.paise == 0; }),
           txns.end());                                              // erase-remove

long long total = std::accumulate(txns.begin(), txns.end(), 0LL,    // 0LL, not 0: accumulator type
                                  [](long long acc, const Txn& t) { return acc + t.paise; });

std::transform(txns.begin(), txns.end(), std::back_inserter(labels),
               [](const Txn& t) { return t.account + "/" + std::to_string(t.paise); });

auto lb = std::lower_bound(sorted.begin(), sorted.end(), 20);       // first >= 20
auto ub = std::upper_bound(sorted.begin(), sorted.end(), 20);       // first >  20
auto mid = std::partition(txns.begin(), txns.end(), [](const Txn& t) { return t.paise > 0; });
```

`code/m07_lambdas_std_function.cpp` shows captures and the cost model:

```cpp
auto by_value = [threshold](int x) { return x > threshold; };   // copies threshold now
auto by_ref   = [&hits](int x) { if (x > 5) ++hits; };          // hits must outlive the lambda
auto counter  = [n = 0]() mutable { return ++n; };              // init-capture + mutable: owns state
auto generic  = [](auto a, auto b) { return a + b; };           // template operator()

template <class F> void apply_template(const std::vector<int>& v, F f);                        // inlined
void apply_function(const std::vector<int>& v, const std::function<void(int)>& f);             // indirect

std::cout << sizeof(by_value) << ' ' << sizeof(by_ref) << ' ' << sizeof(empty) << ' '
          << sizeof(std::function<void(int)>);     // 4 8 1 32 on x86-64 libstdc++
```

## 4. Under the hood (senior-level)

**Templates make algorithms free.** `std::sort(v.begin(), v.end(), cmp)` instantiates a sort
specialised for `vector<Txn>::iterator` and the exact lambda type. The comparator call is a direct
call to a known function and is inlined; the result is the same machine code as a hand-written
quicksort for that struct. `qsort` in C calls through a function pointer per comparison and cannot
inline, which is why `std::sort` typically beats `qsort` by 2x or more on small element types.

**`std::sort` is introsort.** Quicksort with median-of-three pivots, switching to heapsort if
recursion depth exceeds 2*log2(n) (guaranteeing O(n log n) worst case), and finishing with insertion
sort on partitions of 16 or fewer elements. `std::stable_sort` is a merge sort that tries to allocate
a temporary buffer (falls back to an in-place O(n log^2 n) version if allocation fails).

**Lambda object layout.** `[threshold](int x) {...}` compiles to
`struct __lambda { int threshold; bool operator()(int x) const { ... } };`. `[&hits]` stores
`int* hits`. `[this]` stores a pointer. Capturing a large object by value copies it into the lambda,
so a lambda capturing a `std::vector` by value is as expensive to copy as the vector. Each lambda
expression has a unique, unnameable type, so two lambdas with identical text are different types.

**Generic lambdas** are templates: `[](auto a, auto b)` has `template <class T, class U> auto operator()(T a, U b) const`.
Each distinct argument type pair instantiates a new `operator()`.

**`std::function` internals (libstdc++).** 32 bytes: a 16-byte local storage union, a pointer to the
invoker function, and a pointer to the manager function (copy/destroy/type query). Callables up to
16 bytes that are nothrow-move-constructible are stored inline; larger ones are heap-allocated. A
call does: load invoker pointer, indirect call, which then calls the stored callable. Branch
prediction usually handles it, but there is no inlining across the boundary and no vectorisation of
the surrounding loop.

**Dangling captures.** A lambda capturing by reference stores a pointer; if it is called after the
referenced variable's scope ends, it dereferences freed stack. `[=]` can also dangle: capturing
`this` by copy copies the pointer, not the object. In asynchronous code (threads, callbacks, timers)
default to capturing by value or by `shared_ptr`/`weak_ptr`, and capture by reference only when the
lambda is consumed before the enclosing scope ends (e.g. passed to `std::sort`).

**`back_inserter` and output iterators.** `std::back_inserter(v)` returns an iterator whose
`*it = x` calls `v.push_back(x)`. Algorithms can grow the destination this way, at the usual
amortised cost. For known sizes, `v.resize(n)` first and write with a normal iterator avoids the
reallocation checks.

## 5. In production

- **Replace raw loops in review.** A `for` loop that computes a sum, finds an element, or filters is
  a candidate for an algorithm. The algorithm version states intent, has no off-by-one errors, and is
  easier to make parallel later.
- **Sorting with a bad comparator** is a classic latent crash: works for years on small inputs, then a
  particular dataset makes `std::sort` read past the end. Rules: use `<`, never `<=`; compare all
  fields in a fixed order; `std::tie(a.x, a.y) < std::tie(b.x, b.y)` is the idiom for lexicographic
  comparison.
- **`std::function` in a request handler table** is fine (one indirect call per request). In a
  per-byte parser loop it is not; use a template parameter or a plain function pointer.
- **Callbacks that outlive their captures** are the most common crash in asynchronous C++ servers:
  a timer lambda captures `[&conn]` and fires after the connection is closed. Capture a `weak_ptr`
  and `lock()` it, or a `shared_ptr` when the callback must keep the object alive.
- **Ranges of `std::string_view` and `std::sort`.** Sorting a vector of views is cheap (16 bytes per
  element, no string copies); sorting a vector of `std::string` moves 32-byte objects and may be
  memory-bound. Choose the element type with the algorithm's data movement in mind.
- **3 AM debugging.** A stack trace inside `std::__introsort_loop` or `std::__unguarded_partition`
  with a segfault means a non-strict comparator or a comparator that reads uninitialised fields.
  Check the comparator before suspecting the library.

## 6. Try it

1. **(easy)** Rewrite three raw loops from your own code (sum, find-first, remove-matching) using
   `accumulate`, `find_if`, and erase-remove. Done when the outputs are identical and the loops are gone.
2. **(easy)** Write a comparator with `<=` instead of `<` and sort 1000 equal elements under
   `-D_GLIBCXX_DEBUG` or ASan. Done when you have seen the failure and can explain strict weak ordering.
3. **(medium)** Implement `top_k(const std::vector<Txn>&, size_t k)` three ways: full `sort`,
   `partial_sort`, and `nth_element` + `sort` of the prefix. Time them for n = 1M, k = 10. Done when
   you can rank them and explain why.
4. **(medium)** Write a `std::vector<std::function<void(Event&)>>` handler registry and a template
   `dispatch(Handlers&, Event&)` alternative using a `std::tuple` of lambdas. Measure the per-call cost
   of each with 100M dispatches. Done when you have numbers and a recommendation for when each fits.
5. **(hard)** Implement a word-frequency counter: read a file into a string, split into
   `std::string_view` tokens without copying, count with `unordered_map<string_view, int>`, then
   produce the top 20 using `partial_sort` on a vector of pairs. Done when it runs on a 100 MB file in
   under a few seconds and makes no per-word `std::string` allocations (verify with `ltrace -e malloc`
   or a counting allocator).
6. **(hard)** Write a generic `template <class It, class Pred> It stable_partition_copy_free(It, It, Pred)`
   that works on bidirectional iterators, and use `static_assert` with `std::iterator_traits` to
   reject forward iterators. Done when `list` works, `forward_list` fails to compile with your message,
   and `vector` works.

## 7. Interview questions

**Q: What is the erase-remove idiom and why is it needed?**
`std::remove_if` only sees iterators, so it cannot resize the container; it moves the survivors to
the front and returns the new logical end. `container.erase(new_end, end())` then drops the tail.
Forgetting the erase leaves stale elements at the back.

**Q: What are the requirements on a comparator passed to `std::sort`?**
A strict weak ordering: irreflexive (`cmp(a,a)` is false), asymmetric, transitive, and with
transitive equivalence. Using `<=` violates irreflexivity and is undefined behaviour; libstdc++ can
read out of bounds and crash.

**Q: How does a lambda differ from a function pointer?**
A lambda is an object of a unique class with an `operator()`; it can capture state and the compiler
knows its exact type, so calls through templates inline. A function pointer carries no state and is
called indirectly. A capture-less lambda converts to a function pointer.

**Q: When should you use `std::function`?**
When you need to store or pass callables of different types through one non-template interface:
callback members, handler tables, plugin hooks. Avoid it as a parameter type in hot or generic code;
a template parameter is zero-cost.

**Q: What does `std::function` cost?**
32 bytes on libstdc++; a heap allocation if the stored callable exceeds the 16-byte inline buffer;
an indirect call per invocation that cannot be inlined. Copying it copies (or clones) the stored callable.

**Q: Why can you not `std::sort` a `std::list`?**
`std::sort` needs random-access iterators (`it + n`, `it1 - it2`). `std::list` iterators are only
bidirectional. Use `list::sort()`, a merge sort that relinks nodes.

**Q: What is the difference between `std::lower_bound` and `std::find` on a sorted vector?**
Both find an element, but `lower_bound` is a binary search, O(log n), and requires sorted input,
returning the first element not less than the value. `find` is a linear scan, O(n), works on any range,
and returns the first equal element.

**Q: What is wrong with `auto f = [&x]{...};` returned from a function?**
`x` is a local; the lambda stores a pointer to it, and after the function returns calling the lambda
dereferences a dead stack slot (undefined behaviour). Capture by value, or by a smart pointer when
the lambda will run asynchronously.

## 8. Recap

- Iterators generalise pointers; categories (input, forward, bidirectional, random access) decide
  which algorithms work and how fast.
- Know the daily set: `find_if`, `count_if`, `any_of`, `sort`, `stable_sort`, `lower_bound`,
  `partition`, `transform`, `accumulate`, `remove_if` + `erase`.
- Comparators must be strict weak orderings; use `<`, never `<=`; `std::tie` for lexicographic.
- A lambda is a struct with `operator()`; its size is its captures; capture-less lambdas convert to
  function pointers.
- `[x]` copies now, `[&x]` stores a pointer, `[n = expr]` init-captures (and moves), `mutable` lets
  by-copy captures change.
- Template parameters inline the callable; `std::function` type-erases with an indirect call and
  possible heap allocation.
- Reference captures in asynchronous code dangle; capture by value or `shared_ptr`/`weak_ptr`.
- `std::sort` is introsort; `std::stable_sort` is merge sort with a buffer.
- `accumulate`'s initial value sets the accumulator type: write `0LL` or `0.0`.

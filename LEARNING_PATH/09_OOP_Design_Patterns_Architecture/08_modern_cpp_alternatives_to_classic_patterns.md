# 08. Modern C++ Alternatives to Classic Patterns

**Time:** ~3 hours   **Prerequisites:** 01, 05, 06; templates basics from 02_Modern_CPP

## 1. In plain words

The classic design patterns were catalogued in 1994 for languages where the only way to vary
behaviour was a class hierarchy with virtual functions. C++ today has three more tools:
templates (decide at compile time), `std::function`-style type erasure (decide at runtime
without a base class), and `std::variant` (a closed set of alternatives checked by the
compiler). Most patterns from Modules 04 to 06 have a shorter, faster form using one of them.

This module is about the trade. Templates move work from run time to compile time: the
program runs faster and the binary and build get bigger. Type erasure gives runtime
flexibility without forcing your types to inherit from anything. Variants give exhaustiveness
checking. None of them is free, and a senior engineer can say, for a given seam, which one to
use and what it costs in nanoseconds, bytes and seconds of compile time.

The C analogy for the whole module: a macro versus a function pointer. The macro is resolved
at build time, inlined, fast, and produces one copy per use site. The function pointer is
resolved at run time, one copy of the code, slower per call, swappable. Templates are macros
with type checking; type erasure is function pointers with type checking.

## 2. The real thing

### 2.1 Type erasure: `std::function`, `std::any`, hand-rolled

**Problem:** you want to store "anything with `put()` and `get()`" behind one type, by value,
but the candidate types come from different libraries and share no base class. Inheritance
would require editing them.

**Mechanism (the thing to be able to draw on a whiteboard):**

```
AnyStore                        heap
+----------------+              +------------------------+
| unique_ptr -------------->    | Model<MapStore>        |  : Concept (abstract)
+----------------+              |  vptr -> Model<MapStore> vtable
                                |  MapStore obj_         |
                                +------------------------+
```

- `Concept`: an abstract class, *private to the wrapper*, declaring the operations.
- `Model<T>`: a template deriving from `Concept`, holding a `T` by value, forwarding each
  operation to `obj_`.
- The wrapper holds `unique_ptr<Concept>` and exposes the operations.

The user's `T` never inherits from anything. The vtable lives in `Model<T>`, generated per
`T` when the wrapper is constructed from one. Value semantics (copy via a virtual `clone()`)
are preserved.

**The standard examples:**

- `std::function<R(Args...)>`: erases callables. 32 bytes on libstdc++, 16-byte inline buffer,
  heap otherwise. Owning.
- `std::any`: erases anything copyable; retrieval needs `any_cast<T>` with the exact type.
  Rarely the right tool (it is a typed `void*`); useful for plug-in property bags.
- `std::shared_ptr<void>` with a typed deleter erases the deleter type. `unique_ptr<T, D>`
  does not (D is part of the type).

**`function_ref`**: the non-owning version: two pointers (object, trampoline), no allocation,
accepts any callable including capturing lambdas, must not outlive the callable. It is the
correct parameter type for "I will call this during my call and not store it". Not in the
standard until C++26; the code file hand-rolls it in 15 lines.

**Cost:** same as virtual dispatch (one indirect call, no inlining) plus, for owning
wrappers, a heap allocation unless small-buffer optimised.

**When:** you need runtime polymorphism over types you do not control, or you want value
semantics (copyable, stack-allocatable) for polymorphic objects, or your API should accept
lambdas and functors uniformly.

### 2.2 CRTP for static polymorphism

**Mechanism:** `class Derived : public Base<Derived>`. Inside `Base`, `static_cast<Derived&>
(*this).step()` calls the derived function. The compiler knows the exact type, so the call
is direct and inlinable. No vptr, no vtable.

```cpp
template <class Derived>
class RequestProcessor {
public:
    std::string process(const std::string& in) {           // template method, zero virtual calls
        auto& self = static_cast<Derived&>(*this);
        if (!self.validate(in)) return "ERR";
        return self.run(in);
    }
    bool validate(const std::string& s) const { return !s.empty(); }   // default; Derived may shadow
};
class EchoProcessor : public RequestProcessor<EchoProcessor> { std::string run(const std::string&); };
```

**What you lose:** a common base type. `RequestProcessor<Echo>` and `RequestProcessor<Upper>`
are unrelated types; you cannot put them in one `vector<Base*>`. If you need that, you need
virtual dispatch (or type erasure on top).

**Mixins** are CRTP's second use: `struct Money : Comparable<Money>` defines `operator<` and
gets `>`, `<=`, `>=`, `==`, `!=` generated as friends. Boost.Operators is this pattern.

**When:** template-method shaped code on a hot path; mixins that add operators or
bookkeeping to many unrelated types; libraries where the user supplies the derived class
(Eigen, many serialisation libraries).

### 2.3 Policy-based design

**Mechanism:** the host class takes its behavioural pieces as template parameters:
`PolicyStore<HashedStorage, MutexLock, StdoutLog>`. Each policy is a small class with an
agreed (duck-typed) interface. The host inherits from or holds them.

It is the decorator stack of Module 05 flattened into one type at compile time: `Logging
<Metrics<Retry<Memory>>>` becomes `Store<Memory, Retry, Metrics, Logging>` with no virtual
calls between layers and every layer inlinable.

**Zero-cost layers:** a `NoLock` policy whose `Guard` has an empty constructor compiles to
nothing. A `NoLog::log` that is an empty static inline function compiles to nothing. The
empty base optimisation makes stateless policies take zero bytes in the host. This is how
you write one `Store` that is lock-free in a single-threaded tool and mutex-protected in the
server, with no runtime flag.

**Cost:** one host instantiation per combination; combinations are types, so configuration
is a build-time decision; error messages when a policy does not fit are long (C++20 concepts
fix this; in C++17, `static_assert` on the policy's members helps). Readers must understand
templates.

**When:** libraries and infrastructure where the configuration is fixed per build and
performance matters: allocators, containers, logging frameworks, serialisers. Not for
business logic that product people change weekly.

### 2.4 Tag dispatch

**Mechanism:** select an overload by passing an empty struct whose only purpose is its type.
`std::advance(it, n)` calls `advance_impl(it, n, iterator_category{})`; the random-access
overload does `it += n`, the input-iterator overload loops. Derived tags convert to base tags,
so one fallback overload catches a family.

**C++17 alternative:** `if constexpr (std::is_trivially_copyable_v<T>) { ... } else { ... }`.
Prefer `if constexpr` for short branches inside one function. Prefer tags when the
implementations are large, live in different headers, or must be *extensible* by users
adding their own overloads for their own tags (open for extension, at compile time).

### 2.5 `std::variant` state machines

Module 06 showed the state-class form; the code file here builds a protocol framer with
both states and events as variants and a single overload set of transitions:

```cpp
using State = std::variant<WaitHeader, ReadBody, Complete, Failed>;
using Event = std::variant<Bytes, Timeout, Reset>;
Step step(State s, const Event& e) {
    return std::visit(overloaded{
        [](WaitHeader st, const Bytes& b) -> Step { /* accumulate 4 bytes, parse length */ },
        [](ReadBody st,   const Bytes& b) -> Step { /* accumulate body */ },
        [](Complete,      const Reset&)   -> Step { return {WaitHeader{}, 0}; },
        [](auto,          const Reset&)   -> Step { return {WaitHeader{}, 0}; },
        [](auto st, const auto&) -> Step { /* loud fallback */ },
    }, std::move(s), e);
}
```

Properties: every transition in one place; each state carries exactly its data (the partial
header, the body so far) and that data is destroyed on transition; no heap; two-dimensional
`visit` builds a (states x events) jump table. The fallback is the one danger: it can hide a
forgotten pair, so make it log loudly or omit it and let the compiler complain.

### 2.6 Ranges-style pipelines

**Mechanism:** adaptors (`filter`, `map`) are small structs that wrap a source and a
callable; `operator|` nests them; nothing executes until a terminal operation iterates.
The whole chain is one nested type, so every callable is inlined and the compiler sees a
single loop. C++20 `<ranges>` is the standard version; the code file has a 60-line C++17
model showing the mechanism.

**Cost model:** zero runtime dispatch; the pipeline object is the size of its captured state
(a reference plus empty lambdas is 8 to 16 bytes); but each distinct pipeline is a distinct
type with its own instantiated code. Compile time grows with the number of distinct chains.
Debugging steps through template machinery.

### 2.7 When templates beat virtual, with the costs

| Dimension | Virtual / `std::function` | Template / CRTP / policy |
|---|---|---|
| Decision time | Run time | Compile time |
| Call cost | 2 loads + indirect call, no inlining | Direct, inlined; enables vectorisation and constant folding |
| Per-object size | +8 bytes vptr (or 32 bytes for `std::function`) | 0 (empty base optimisation for stateless policies) |
| Heterogeneous containers | `vector<unique_ptr<Base>>` works | Not directly; need `std::variant` or type erasure |
| Open set of implementations (plug-ins, other TUs) | Yes | No (all types known where instantiated) |
| Binary size | One copy of each function | One copy per instantiation |
| Compile time | Low; implementation in `.cpp` | Higher; implementation in headers, recompiled per TU |
| Error messages | Clear | Long (improved by `static_assert`, fixed by C++20 concepts) |
| ABI / shared library boundary | Stable if interface stable | Templates cannot cross a binary boundary as templates |

**The decision rule:** if the choice depends on *runtime data* (config, message type,
client) or must be made by *code you do not compile* (plug-ins), use virtual or type erasure.
If the choice is *fixed at build time* and the code is *hot* or the layering is deep, use
templates. If the set of alternatives is *closed and small* and you want *exhaustiveness
checking*, use `std::variant`.

Order of magnitude for "hot": a virtual call costs single-digit nanoseconds when predicted.
A database round trip costs 100 microseconds or more. Anything that happens once per
request around I/O can be virtual with no measurable effect. Anything that happens per byte,
per element or per hash-table probe should be a template.

## 3. Code

- `code/m08_type_erasure.cpp`: `AnyStore` (Concept/Model/clone, value semantics) holding a
  `MapStore` and a `VectorStore` with no common base; `function_ref` hand-rolled and used as a
  callback parameter accepting a capturing lambda; `sizeof` printed for both.
- `code/m08_crtp_policy.cpp`: `RequestProcessor<Derived>` as a zero-virtual template method,
  `Comparable<Derived>` mixin, and `PolicyStore<Storage, Lock, Log>` with `NoLock`/`MutexLock`
  and `NoLog`/`StdoutLog`, including `sizeof` showing the empty base optimisation.
- `code/m08_variant_state_machine.cpp`: the length-prefixed framer with variant states and
  events, a `Framer` driver that feeds arbitrary chunk sizes, timeout and bad-length paths.
- `code/m08_tag_dispatch_pipeline.cpp`: `copy_records` dispatching on trivial copyability,
  `my_advance` dispatching on iterator category, `if constexpr` comparison, and a lazy
  `from(c) | filter(...) | map(...)` pipeline with `to_vector` and `sum`.

The type-erasure core, which is the part to memorise:

```cpp
class AnyStore {
public:
    template <class T, class = std::enable_if_t<!std::is_same_v<std::decay_t<T>, AnyStore>>>
    AnyStore(T&& obj) : self_(std::make_unique<Model<std::decay_t<T>>>(std::forward<T>(obj))) {}
    AnyStore(const AnyStore& o) : self_(o.self_->clone()) {}
    void put(const std::string& k, const std::string& v) { self_->put(k, v); }
private:
    struct Concept { virtual ~Concept() = default;
                     virtual void put(const std::string&, const std::string&) = 0;
                     virtual std::unique_ptr<Concept> clone() const = 0; };
    template <class T> struct Model final : Concept {
        explicit Model(T obj) : obj_(std::move(obj)) {}
        void put(const std::string& k, const std::string& v) override { obj_.put(k, v); }
        std::unique_ptr<Concept> clone() const override { return std::make_unique<Model>(obj_); }
        T obj_;
    };
    std::unique_ptr<Concept> self_;
};
```

## 4. Under the hood (senior-level)

**What a template instantiation costs the compiler.** Each distinct `PolicyStore<A, B, C>`
is parsed, type-checked, and code-generated separately in every translation unit that uses
it; the linker then discards the duplicates (COMDAT folding). A header-heavy template
codebase can spend most of its build time generating code it throws away. Mitigations:
explicit instantiation in one `.cpp` (`template class PolicyStore<...>;`) with `extern
template` in the header; keeping non-dependent code out of templates (hoist it into a
non-template base); precompiled headers; and simply not templating what does not need it.

**Code size.** Ten pipelines with different lambdas are ten loops in the binary. Usually
small, but instruction-cache pressure is real on hot paths: a tight loop that fits in L1i
can be slower when surrounded by many near-duplicate instantiations. Measure with `size`
and `perf stat -e L1-icache-load-misses` before assuming templates are faster.

**Devirtualisation is not nothing.** With `-O2`, GCC devirtualises calls when the dynamic type
is provable (`final` classes, local objects, after inlining a factory). With `-flto` it does
whole-program speculative devirtualisation: `if (vptr == &Derived::vtable) inline else call`.
So the virtual-vs-template gap is sometimes smaller than the table above suggests. The gap is
largest when the call site is genuinely polymorphic at run time.

**`std::visit` compiled.** For one variant, libstdc++ builds a `constexpr` array of function
pointers indexed by `index()` and calls through it; for small variants GCC often turns this
into a `switch` and inlines the lambdas. For two variants (states x events) the table is
two-dimensional. `std::get_if` is a compare of the index and a pointer offset.

**Empty base optimisation.** A base class with no non-static data members occupies zero bytes
in the derived object (the standard requires distinct addresses only for distinct subobjects
of the same type). That is why `PolicyStore<HashedStorage, NoLock, NoLog>` is exactly the
size of the `unordered_map`. Policies held as *members* instead of bases would cost at least
one byte each plus padding; C++20's `[[no_unique_address]]` fixes that for members.

**Type erasure's hidden allocation and how to remove it.** Owning wrappers allocate for
`Model<T>`. A small-buffer optimisation keeps `Model<T>` inline when `sizeof(T)` fits (say,
32 bytes) and heap-allocates otherwise; this is what `std::function` does with its 16-byte
buffer. The implementation uses `std::aligned_storage` and placement new, with the
`Concept` vtable still in play. Move semantics for the inline case must move the object, not
the pointer, which is why `std::function`'s move is not guaranteed `noexcept`-cheap for
every target.

## 5. In production

**Where this shows up in a server.** The storage interface at the service boundary: virtual
(runtime choice of backend, needs a test fake). The framing and parsing of bytes on the
socket: templates or `std::variant` (hot, fixed at build time). Per-request middleware:
`std::function` (configured at startup, called once per request, cost invisible). Metrics
and logging *inside* the hash-table implementation: policy template (called per probe, must
compile away when disabled).

**A real cost conversation.** A team moved a decoder from a virtual `Codec` interface to a
template because a profile showed 12% of CPU in `Codec::decode` dispatch and lost inlining.
Decode time dropped 35%. Build time for the TU went from 4 seconds to 40 because the
decoder was now instantiated in nine places. They added explicit instantiation for the three
codecs actually used and got back to 9 seconds. Both numbers mattered; the senior decision
was to pay the build cost once it was bounded.

**The type-erasure trap.** `std::function` members in a hot object (one per callback in a
connection class with 50,000 connections): 32 bytes each, plus a heap block each when the
captured lambda exceeds 16 bytes. Memory doubled. Fix: `function_ref` where the callback is
only used during a call; a single `std::function` to a per-connection handler object
otherwise; or a plain function pointer plus `void*` context (the C way is sometimes the
right C++ way).

**Error messages.** A policy that lacks a required member produces a 200-line error from
inside the host template. Put `static_assert(has_guard_v<Lock>, "Lock policy must define
Guard")` at the top of the host; the error becomes one line. In C++20 a `concept` does this
natively.

**Code review question to ask every time:** "Does this decision need to be made at run
time?" If the answer is no, the virtual function is a cost without benefit. If the answer
is yes, the template is a flexibility you cannot use.

## 6. Try it

1. **(easy)** Add a third store type to `m08_type_erasure.cpp` that is a `struct` with
   `std::function` members for `put` and `get`. Store it in the same `vector<AnyStore>`.
   Done when it works without modifying `AnyStore`.
2. **(easy)** In `m08_crtp_policy.cpp`, write a `CountingLog` policy that counts calls in a
   static counter. Compose `PolicyStore<OrderedStorage, NoLock, CountingLog>`. Done when
   `sizeof` is unchanged from `NoLog` and the count is correct.
3. **(medium)** Give `AnyStore` a small-buffer optimisation: 32 bytes of aligned storage;
   `Model<T>` constructed in place if it fits, on the heap otherwise. Done when
   `sizeof(AnyStore)` is 40 or 48, `MapStore` (a `std::map`, 48 bytes) goes to the heap, and a
   tiny store stays inline; verify with a counting `operator new`.
4. **(medium)** Add an `Escape` event and a `Paused` state to the variant framer. Observe
   which overloads the compiler asks you to add, and which fell into the fallback. Then
   remove the fallback and list every (state, event) pair you had to write. Done when the
   machine compiles without a fallback.
5. **(hard)** Benchmark `m05_decorator_store.cpp`'s virtual decorator stack (Logging over
   Metrics over Memory, with logging to a null sink) against a policy version
   `PolicyStore<HashedStorage, NoLock, NullLog>` with a metrics policy, 10 million `get`
   calls at `-O2`. Report ns/call for both and the binary size of each. Done when you can
   explain the ratio and state when it would and would not matter.
6. **(hard)** Compile `m08_tag_dispatch_pipeline.cpp` with `-O2 -S` and find the loop that
   `sum(pipeline)` became. Confirm there is no call instruction inside it. Then replace the
   lambdas with `std::function` objects and find the calls. Done when you can point to the
   indirect call instruction and the vptr-like load that precedes it.

## 7. Interview questions

**Q: What is type erasure and how does `std::function` implement it?**
A: Runtime polymorphism without a common base class. The wrapper holds an internal abstract
`Concept`; a template `Model<T>` implements it by forwarding to a `T` it owns; the vtable
lives in the wrapper, not in the user's type. `std::function` adds a 16-byte inline buffer
for small callables and heap-allocates larger ones; invocation is an indirect call.

**Q: When would you choose CRTP over virtual functions?**
A: When the derived type is known at compile time and the call is hot: template-method
skeletons, mixins that generate operators, library bases where the user supplies the derived
type. CRTP gives direct, inlinable calls and no vptr but loses the common base type, so you
cannot store mixed derived types in one container.

**Q: What is policy-based design and what does it cost?**
A: The host class takes behavioural pieces as template parameters (storage, locking,
logging), composing them at compile time with no virtual calls; stateless policies cost zero
bytes via the empty base optimisation. Costs: one instantiation per combination,
configuration fixed at build time, longer error messages, and header-only implementation.

**Q: Explain the full cost comparison between a virtual call and a template call.**
A: Virtual: two dependent loads plus an indirect call, cheap when predicted, but no inlining,
so the optimiser cannot fold constants, hoist or vectorise across it, and each object carries
a vptr. Template: direct call, inlined, optimisable; but one code copy per instantiation
(binary size, instruction cache, compile time) and the type must be known at compile time.
The right choice depends on whether the decision is a runtime one and how hot the call is.

**Q: Why is `std::variant` a good fit for state machines?**
A: Each state is a struct carrying exactly its data, destroyed on transition; all transitions
can live in one overload set; there is no heap or vtable; and `std::visit` requires a handler
for every combination, so forgotten (state, event) pairs are compile errors unless you add a
fallback.

**Q: What is tag dispatch and when is `if constexpr` better?**
A: Tag dispatch selects an overload by passing an empty tag type (as `std::advance` does with
iterator categories), with derived tags falling back to base overloads. `if constexpr` is
simpler when the branches are short and in one function; tags are better when
implementations are large, in different headers, or must be extensible by user-provided
overloads.

**Q: What is `function_ref` and when do you use it instead of `std::function`?**
A: A non-owning, two-pointer view of a callable: object pointer plus a trampoline that knows
the real type. No allocation, accepts capturing lambdas. Use it for callback parameters that
are only invoked during the call; never store it, because it does not own the callable.

## 8. Recap

- Type erasure: Concept/Model/wrapper; polymorphism without inheritance; value semantics;
  one allocation unless small-buffer optimised.
- `function_ref` for callback parameters; `std::function` for stored callbacks; `std::any`
  almost never.
- CRTP: static template method and mixins; loses the common base type.
- Policy-based design: decorator stacks flattened at compile time; zero-cost disabled
  policies via empty base optimisation.
- Tag dispatch for extensible compile-time selection; `if constexpr` for short local
  branches.
- `std::variant` state machines: one overload set, per-state data, exhaustiveness.
- Pipelines: lazy, inlined, one type per chain.
- Templates trade run time for compile time and binary size; virtual trades the reverse.
  Ask: is the decision made at run time? Is the call hot?
- Explicit instantiation and `static_assert`s tame template build cost and error messages.

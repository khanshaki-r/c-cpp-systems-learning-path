# 02. How a C++ program is built

**Time:** ~3 hours   **Prerequisites:** 01

## 1. In plain words

When you press Build in Visual Studio, four separate programs run behind the button: a preprocessor
that pastes headers and expands macros, a compiler that turns one `.cpp` file into assembly, an
assembler that turns assembly into an object file, and a linker that glues all object files and
libraries into one executable. On Linux you can see and run each of those four steps yourself.
Once you can, "undefined reference", "multiple definition", "cannot open shared object file" and
"symbol lookup error" stop being mysteries and become specific failures of a specific step.

Analogy: building a book from chapters written by different authors. Preprocessing is each author
pasting in their boilerplate. Compiling is each chapter being typeset independently; references to
other chapters are left as "see page ???". Linking is the editor binding all chapters together and
filling in the real page numbers. A static library is a set of pre-typeset chapters you can bind in.
A shared library is a separate volume on the shelf that the book says "go read volume X page 42";
the reader (the dynamic loader) fetches it when you open the book.

## 2. The real thing

### 2.1 The four stages

```
 main.cpp ---> [cpp: preprocess] ---> main.ii  (pure C++ text, no #, headers pasted in)
 main.ii  ---> [cc1plus: compile] ---> main.s   (x86-64 assembly for THIS file only)
 main.s   ---> [as: assemble]     ---> main.o   (ELF relocatable object: machine code + symbols + relocations)
 main.o + util.o + libfoo.a + libc.so ---> [ld: link] ---> a.out (ELF executable)
```

`g++` is a **driver**: it decides which of these to run based on file extensions and flags, and
calls `cc1plus`, `as`, `collect2`/`ld` for you. Watch it: `g++ -v main.cpp`. Stop after each stage:

```bash
g++ -E main.cpp -o main.ii     # preprocess only
g++ -S main.cpp -o main.s      # compile to assembly
g++ -c main.cpp -o main.o      # compile + assemble, do not link
g++ main.o util.o -o prog      # link only
g++ -save-temps main.cpp       # keep .ii .s .o
```

**Preprocess:** `#include` is textual paste; `#define` is textual replace; `#if` is textual removal.
After this step there is no notion of "header". A 30-line `main.cpp` that includes `<iostream>` becomes
roughly 30,000 lines (`g++ -E main.cpp | wc -l`). This is why compile times are dominated by headers
and why forward declarations and precompiled headers matter.

**Compile:** parse, semantic analysis, optimisation, code generation, for one translation unit (TU)
at a time. The compiler sees one `.ii` and nothing else. Any function it calls that is not defined in
this TU becomes an **undefined symbol** with a relocation: "call to `foo`, address to be filled in".
Any function it defines becomes a **defined symbol**.

**Assemble:** text assembly to machine code in an ELF relocatable object. Still per TU.

**Link:** take all `.o` files and libraries, merge same-named sections (`.text` with `.text`,
`.data` with `.data`), build a symbol table, resolve every undefined symbol to exactly one
definition, apply relocations (patch the addresses), write an executable.

### 2.2 Object files and ELF

ELF (Executable and Linkable Format) is the container for `.o`, `.so`, executables and core dumps
on Linux. Windows' PE/COFF is the equivalent. Key sections:

| Section | Contents | Windows name |
|---|---|---|
| `.text` | machine code | `.text` |
| `.rodata` | string literals, const tables | `.rdata` |
| `.data` | initialised globals/statics | `.data` |
| `.bss` | zero-initialised globals (no bytes in file, only a size) | `.bss` |
| `.symtab` | symbol table (name, address, size, type, binding) | COFF symbol table |
| `.rela.text` | relocations: patch-me-here entries | relocations |
| `.debug_*` | DWARF debug info (with `-g`) | `.pdb` (separate file) |
| `.dynamic`, `.dynsym`, `.dynstr` | dynamic linking info (in .so and executables) | import/export tables |
| `.init_array` | constructors to run before `main` (global C++ objects) | `.CRT$XCU` |

Tools to look inside:

```bash
file prog                         # "ELF 64-bit LSB pie executable, x86-64, dynamically linked ..."
nm main.o                         # symbols: T defined text, U undefined, D data, B bss, W weak
nm -C main.o                      # -C demangles C++ names
nm -D libfoo.so                   # dynamic (exported) symbols only
objdump -d main.o                 # disassemble
objdump -t main.o                 # symbol table
objdump -r main.o                 # relocations
readelf -h prog                   # ELF header: type, entry point
readelf -S prog                   # section headers
readelf -s prog | grep FUNC       # symbol table
readelf -d prog                   # dynamic section: NEEDED libs, RPATH/RUNPATH, SONAME
readelf -l prog                   # program headers (segments the loader maps)
size prog                         # text/data/bss totals
strings -n 8 prog | less          # printable strings
c++filt _ZN5tcpkv5Store3getERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE   # demangle one
```

`nm` letters you will read daily:

```
T  defined function in .text (global)      t  same, local (static) to this TU
U  undefined: needed from elsewhere
D  defined initialised data                 B  defined zero-init data (.bss)
R  read-only data                           W  weak symbol (inline functions, templates)
V  weak object                              i  indirect function (ifunc)
```

### 2.3 Name mangling

C++ allows overloading, namespaces and templates, so the linker-visible symbol must encode the full
signature. `int tcpkv::Store::get(const std::string&)` becomes
`_ZN5tcpkv5Store3getERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE` under the Itanium
ABI used by GCC and Clang. MSVC uses a different scheme (`?get@Store@tcpkv@@QEAAH...`), which is one
reason you cannot link MSVC objects with GCC objects.

Practical consequence: "undefined reference to `foo(int)`" vs a definition of `foo(long)` is a
mangling mismatch, not a missing file. `extern "C"` turns mangling off, which is how C libraries
export stable names and how a C++ library exposes a C API.

### 2.4 Symbols: strong, weak, local

- **Strong (global) defined**: a normal function or variable definition. Two strong definitions
  of the same name in two objects is a link error: "multiple definition of `foo`".
- **Weak**: inline functions, template instantiations, `inline` variables, functions with
  `__attribute__((weak))`. The linker keeps one copy and silently discards the rest. This is how
  the same `std::vector<int>::push_back` instantiated in ten TUs becomes one function.
- **Local (static)**: `static` functions and anonymous-namespace entities. Invisible to other objects.
  Cannot collide.
- **Undefined**: referenced, not defined here. Must be resolved by exactly one strong (or weak)
  definition somewhere in the link, or the link fails.

### 2.5 Static libraries: `.a`

A static library is an archive of `.o` files with an index. Nothing more. `ar` creates it.

```bash
g++ -c mathlib.cpp -o mathlib.o
ar rcs libmathlib.a mathlib.o          # r insert, c create, s index
ar t libmathlib.a                      # list members
nm libmathlib.a                        # symbols per member
g++ main.o -L. -lmathlib -o prog       # -L search dir, -lmathlib means libmathlib.a (or .so)
```

How the linker uses it: it walks the command line left to right, keeping a list of currently
undefined symbols. When it meets an archive, it pulls in **only the members that define a currently
undefined symbol**, then re-scans those members for new undefineds. Members that nothing needs are
dropped. This has two consequences:

1. **Order matters.** `g++ -lmathlib main.o` fails: when the linker reaches `libmathlib.a`, nothing
   is undefined yet, so it takes nothing; then `main.o` adds undefineds that are never satisfied.
   Libraries go **after** the objects that use them, and a library that depends on another library
   goes **before** it (`-lA -lB` if A uses B).
2. The code is copied into your executable. No run-time dependency. Bigger binary. Update the
   library, relink every consumer.

Windows: `.lib` produced by `lib.exe` is the same idea (COFF archive). The MSVC linker is more
forgiving about order because it does iterative resolution over all inputs by default.

### 2.6 Shared libraries: `.so`

A shared object is a complete ELF file with code and a dynamic symbol table, loaded at run time
by the dynamic loader and mapped into the process. One copy of `libstdc++.so.6` on disk serves every
C++ program on the machine, and its pages are shared in RAM.

```bash
g++ -fPIC -c mathlib.cpp -o mathlib.o
g++ -shared -o libmathlib.so.1.0.0 mathlib.o -Wl,-soname,libmathlib.so.1
ln -s libmathlib.so.1.0.0 libmathlib.so.1       # soname: what consumers record
ln -s libmathlib.so.1 libmathlib.so              # dev name: what -lmathlib finds
g++ main.o -L. -lmathlib -o prog
./prog
# ./prog: error while loading shared libraries: libmathlib.so.1: cannot open shared object file
```

That error is the most common first contact with the dynamic loader. The link succeeded; the run
failed because at run time `ld.so` could not find `libmathlib.so.1`.

**`-fPIC`: Position-Independent Code.** A shared library is mapped at a different address in every
process (ASLR), so its code cannot contain absolute addresses. `-fPIC` makes the compiler emit
RIP-relative addressing and go through the GOT (Global Offset Table) for globals and the PLT
(Procedure Linkage Table) for function calls. Without `-fPIC`, `ld` refuses: "relocation
R_X86_64_32 against `.rodata` can not be used when making a shared object; recompile with -fPIC".
On x86-64, cost is small (one indirection for cross-library calls). Executables on Ubuntu are
PIE (position-independent executables) by default too.

**Soname and versioning.** `libmathlib.so.1` is the soname; the "1" is the ABI version. Consumers
record the soname in their `NEEDED` entry (`readelf -d prog`). You can ship `libmathlib.so.1.0.1`
with bug fixes and all consumers pick it up after updating the symlink; a breaking ABI change becomes
`libmathlib.so.2` and old binaries keep working with the old file.

**Windows comparison:** a `.dll` plus an import `.lib`. On Linux you link directly against the
`.so`; there is no import library. Windows exports nothing by default (`__declspec(dllexport)`);
GCC exports everything by default. Hide symbols with `-fvisibility=hidden` and
`__attribute__((visibility("default")))` on the public API; this shrinks the dynamic symbol table
and speeds up loading.

### 2.7 How the dynamic loader finds libraries

The program's `PT_INTERP` names the loader: `/lib64/ld-linux-x86-64.so.2`. The kernel maps the
executable and the loader; the loader then resolves every `NEEDED` entry in this order:

1. `DT_RPATH` baked into the executable (deprecated, but honoured if there is no RUNPATH).
2. `LD_LIBRARY_PATH` environment variable (colon-separated).
3. `DT_RUNPATH` baked into the executable (`-Wl,-rpath,...` with modern `ld` sets RUNPATH).
4. `/etc/ld.so.cache`, built by `ldconfig` from `/etc/ld.so.conf.d/*`.
5. `/lib`, `/usr/lib` (and the multiarch dirs).

Special token: `$ORIGIN` means "the directory of the executable itself", so
`-Wl,-rpath,'$ORIGIN/../lib'` lets you ship a relocatable tree `bin/prog` + `lib/libfoo.so`.

```bash
ldd ./prog                         # what will be loaded and from where ("not found" is the bug)
LD_LIBRARY_PATH=. ./prog           # quick fix for testing; never in production configs
g++ main.o -L. -lmathlib -Wl,-rpath,'$ORIGIN' -o prog    # permanent fix: RUNPATH
readelf -d prog | grep -E 'NEEDED|RUNPATH|RPATH'
LD_DEBUG=libs ./prog               # trace the search
sudo ldconfig                      # rebuild cache after installing a lib to /usr/local/lib
```

Windows search order for comparison: application directory, System32, the `PATH` variable.
`LD_LIBRARY_PATH` is the `PATH` of DLL loading; RUNPATH is the application-directory rule made
explicit and relocatable.

**Lazy binding.** Function calls into a shared library go through the PLT. The first call jumps to
the resolver, which looks up the symbol and patches the GOT entry; later calls go straight through.
`LD_BIND_NOW=1` resolves everything at startup (what `-Wl,-z,now` does permanently; used by hardened
builds). A symbol that is missing in a lazily bound program only fails **when first called**:
"symbol lookup error: undefined symbol: foo". That is a run-time undefined reference.

### 2.8 Reading "undefined reference to"

```
/usr/bin/ld: main.o: in function `main':
main.cpp:(.text+0x1a): undefined reference to `mathlib::add(int, int)'
collect2: error: ld returned 1 exit status
```

Translate: while linking, object `main.o`, inside function `main`, there is a call to a symbol
`mathlib::add(int,int)` and no input provided a definition. Three causes, in order of frequency:

1. **You did not link the object or library that defines it.** Forgot `mathlib.o`, forgot
   `-lmathlib`, or forgot `target_link_libraries` in CMake. Fix: add it.
2. **Order.** The library appeared before the object that uses it (static libs). Fix: move the
   library after, or use `-Wl,--start-group ... --end-group` for circular static deps.
3. **Signature mismatch.** Declared `int add(int, int)` in the header but defined
   `long add(long, long)` in the `.cpp`; or the header was C++ but the definition compiled as C
   without `extern "C"`; or the definition is `static`/anonymous-namespace and so invisible.
   Fix: `nm -C mathlib.o | grep add` and compare exactly.

A fourth, for shared libs: the library is linked but the symbol is hidden (`-fvisibility=hidden`)
or the `.so` was built from a different version of the header.

Also common: `undefined reference to 'pthread_create'` means add `-pthread`;
`undefined reference to 'std::__cxx11::basic_string...'` with a prebuilt library means a
`_GLIBCXX_USE_CXX11_ABI` mismatch between your build and the library's build;
`undefined reference to 'vtable for Foo'` means you declared a virtual function and never defined it
(usually the first non-inline virtual, often the destructor).

### 2.9 Multiple definitions and ODR

The One Definition Rule: every non-inline function and variable must have exactly one definition in
the whole program; every class, inline function and template may be defined in many TUs **as long as
every definition is token-for-token identical**.

Link-time symptom of a broken ODR: `multiple definition of 'counter'; util.o:(.bss+0x0): first
defined here`. Cause: a variable or non-inline function defined in a header that is included by two
TUs. Fix: declare `extern int counter;` in the header, define once in a `.cpp`; or `inline int
counter = 0;` (C++17 inline variable); or mark the function `inline` or `static`.

The dangerous ODR violation is the **silent** one: two TUs define `struct Config` with different
members (because one was compiled with `-DDEBUG_FIELDS` and the other not), or two TUs define the
same `inline` function differently. The linker keeps one weak copy at random and the program
misbehaves. No error. Sanitizers do not catch it. `-Wodr` with LTO catches some cases. This is a
senior-level "why does it crash only in release" root cause (see 05_Memory_Debugging_RCA).

### 2.10 Link-time optimisation (LTO)

Normally the compiler optimises one TU at a time, so a tiny function in `util.cpp` cannot be inlined
into `main.cpp`. With `-flto` the compiler emits its intermediate representation (GIMPLE for GCC)
into the object files instead of final machine code; the linker calls back into the compiler to
optimise the whole program at once, then generates code. Gains: cross-TU inlining, dead code
removal, devirtualisation, and `-Wodr` warnings. Costs: link time and memory; harder debugging.
Enable in CMake with `set_property(TARGET tcpkv PROPERTY INTERPROCEDURAL_OPTIMIZATION ON)`.
MSVC equivalent: `/GL` + `/LTCG`.

## 3. Code

All examples are in `code/m02_*`. Each directory has a `build.sh` that runs the exact commands.

### 3.1 Static library (`code/m02_static_lib/`)

```cpp
// mathlib.h
#pragma once
namespace mathlib {
int add(int a, int b);
int mul(int a, int b);
}
```

```cpp
// mathlib.cpp
#include "mathlib.h"
namespace mathlib {
int add(int a, int b) { return a + b; }
int mul(int a, int b) { return a * b; }
}
```

```cpp
// main.cpp
#include <cstdio>
#include "mathlib.h"
int main() {
    std::printf("%d %d\n", mathlib::add(2, 3), mathlib::mul(4, 5));
    return 0;
}
```

```bash
# build.sh (key lines)
g++ -std=c++17 -Wall -Wextra -Werror -c mathlib.cpp -o mathlib.o
ar rcs libmathlib.a mathlib.o
g++ -std=c++17 -Wall -Wextra -Werror -c main.cpp -o main.o
nm -C main.o                 # U mathlib::add(int,int)  -- undefined here
nm -C libmathlib.a           # T mathlib::add(int,int)  -- defined here
g++ main.o -L. -lmathlib -o prog_static      # correct order
./prog_static
g++ -L. -lmathlib main.o -o prog_wrong 2>&1 | head -3   # wrong order: undefined reference
```

### 3.2 Shared library (`code/m02_shared_lib/`)

Same sources, different build:

```bash
g++ -std=c++17 -Wall -Wextra -Werror -fPIC -c mathlib.cpp -o mathlib.o
g++ -shared -Wl,-soname,libmathlib.so.1 -o libmathlib.so.1.0.0 mathlib.o
ln -sf libmathlib.so.1.0.0 libmathlib.so.1
ln -sf libmathlib.so.1 libmathlib.so
g++ -std=c++17 -c main.cpp -o main.o
g++ main.o -L. -lmathlib -o prog_norpath
./prog_norpath || true                        # fails: cannot open shared object file
LD_LIBRARY_PATH=. ./prog_norpath              # works: env var search
g++ main.o -L. -lmathlib -Wl,-rpath,'$ORIGIN' -o prog_rpath
./prog_rpath                                  # works: RUNPATH baked in
ldd ./prog_rpath                              # shows libmathlib.so.1 => ./libmathlib.so.1
readelf -d prog_rpath | grep -E 'NEEDED|RUNPATH'
nm -D libmathlib.so | grep -i add             # exported symbol
```

### 3.3 Undefined reference, three ways (`code/m02_undefined_ref/`)

```cpp
// util.h
#pragma once
int helper(int x);
// util.cpp
#include "util.h"
int helper(int x) { return x * 2; }
// main.cpp
#include <cstdio>
#include "util.h"
int main() { std::printf("%d\n", helper(21)); return 0; }
```

```bash
g++ -c main.cpp && g++ -c util.cpp
g++ main.o -o prog             # 1: forgot util.o -> undefined reference to `helper(int)'
g++ main.o util.o -o prog && ./prog          # fixed
# 2: signature mismatch. util_long.cpp defines long helper(long)
g++ -c util_long.cpp && g++ main.o util_long.o -o prog   # undefined reference: mangled names differ
nm -C main.o | grep helper; nm -C util_long.o | grep helper   # compare: helper(int) vs helper(long)
# 3: C vs C++ mangling. util_c.c defines helper without extern "C"
gcc -c util_c.c && g++ main.o util_c.o -o prog           # undefined: main wants _Z6helperi, util_c has helper
```

### 3.4 ODR violation (`code/m02_odr_violation/`)

```cpp
// bad_header.h: a definition in a header (the bug)
#pragma once
int g_counter = 0;             // every TU that includes this DEFINES g_counter
// a.cpp and b.cpp both include bad_header.h
```

```bash
g++ -c a.cpp && g++ -c b.cpp
g++ a.o b.o -o prog   # multiple definition of `g_counter'; b.o:(.bss+0x0): first defined here
# fix 1: header says extern int g_counter; a.cpp defines it
# fix 2 (C++17): inline int g_counter = 0;
```

The directory also has `silent_odr/` where two TUs define `struct Config` with different layouts
behind a macro. It links without error and prints garbage. Run it, then run it with `-flto -Wodr`
and read the warning.

## 4. Under the hood (senior-level)

**What `main.o` really contains for `mathlib::add(2,3)`.** The compiler emits
`call <placeholder>` with a relocation entry `R_X86_64_PLT32 _ZN7mathlib3addEii - 4` at that
offset. `objdump -dr main.o` shows it. The linker computes the final address of `add`, subtracts the
address of the instruction, and writes the 32-bit displacement. For a static link that is the
whole story. For a shared library the call goes to a PLT stub instead, and the GOT slot the stub
reads is filled at load time (or first call).

**Why the linker is single pass over archives.** `ld` is designed around streaming inputs; it
keeps an undefined-symbol set and resolves as it goes. This is cheap and predictable but imposes
the ordering rule. `--start-group/--end-group` makes it loop over a set until no new symbols are
resolved. MSVC's `link.exe` and `lld` effectively do that for all inputs by default, which is why
Windows developers rarely meet the ordering problem.

**The dynamic loader is user-space code.** `execve` maps the executable's `PT_LOAD` segments and
the interpreter named in `PT_INTERP`, then jumps to the interpreter's entry. `ld.so` reads
`.dynamic`, walks `NEEDED`, `mmap`s each library (`strace ./prog` shows the `openat` + `mmap`
pairs), applies relocations, runs each library's `.init_array` (C++ global constructors), then
jumps to `_start` in the executable, which calls `__libc_start_main`, which calls `main`.
Startup time is dominated by this: a program linked against 50 shared libraries pays 50 file opens
and thousands of relocations before `main`. `-Wl,-z,now` moves lazy PLT resolution to startup;
prelinking and static linking remove it.

**Copy relocations and why `-fPIC` is slower on 32-bit but not on x86-64.** x86-64 has RIP-relative
addressing, so position-independent code to access local data costs nothing extra. Only accesses to
symbols that might be interposed (globals from other libraries) go through the GOT. On 32-bit x86
there was no PC-relative addressing, so `-fPIC` had to dedicate a register (`ebx`) to the GOT base,
costing one of only six general-purpose registers. That history is why older texts call PIC "slow".

**Symbol interposition.** Because the loader resolves a symbol to the first definition in load
order, `LD_PRELOAD=./mymalloc.so ./prog` replaces `malloc` for the whole process. This is how
`valgrind`-free leak checkers, `libasan`, `jemalloc` and `tcmalloc` hook in. It is also a security
reason that `LD_PRELOAD` and `LD_LIBRARY_PATH` are ignored for setuid binaries.

**Where `.bss` goes.** Zero-initialised data has no bytes in the file; the ELF program header
records `p_memsz > p_filesz`, and the kernel maps anonymous zero pages for the difference. A
100 MB static array costs nothing on disk and nothing in RAM until touched (demand paging).

**Startup of C++ globals across TUs** runs in link order within one object file and in an
unspecified order across TUs: the "static initialisation order fiasco". The fix is a function-local
static (`Meyers singleton`), which the compiler guards with a thread-safe once-flag.

## 5. In production

- **"Works on my machine, fails on the server: cannot open shared object file."** The dev box has
  `libfoo-dev` installed; the server does not. `ldd ./tcpkv` on the server shows `not found`.
  Fixes, from quick to right: `LD_LIBRARY_PATH` (quick), `ldconfig` after installing the runtime
  package, `$ORIGIN` RUNPATH for relocatable deploys, or static linking of that one library.
- **Static vs shared for a server binary.** Static (`-static-libstdc++ -static-libgcc` at minimum,
  or fully `-static` with musl) gives a single file that runs on any kernel-compatible Linux and is
  what a slim Docker image wants (module 08). Shared gets security updates to `libssl` without a
  rebuild and shares pages across processes. Most servers: dynamic against glibc, static for
  everything else they own.
- **ABI breaks.** Upgrading a vendor `.so` from `.1.0.0` to `.1.1.0` with a changed struct layout
  but the same soname corrupts memory silently. The soname contract is your defence; demand it in
  vendor conversations.
- **Link order in Makefiles.** `LDLIBS` must be after objects; CMake orders this correctly from the
  target dependency graph, which is one argument for CMake over hand-written Makefiles.
- **Your Berkeley DB to YugabyteDB migration engine** links `libdb` and `libpq`. On Linux you would
  ship it with `readelf -d` showing `NEEDED libdb-5.3.so, libpq.so.5` and a RUNPATH; or statically
  link libdb (BSD licence allows it) and keep libpq dynamic to pick up TLS fixes. Being able to say
  that is senior-level.
- **Symbol stripping.** Release binaries are `strip`ped (or built with `-s`) to remove `.symtab`
  and DWARF; keep an unstripped copy or a separate `.debug` file (`objcopy --only-keep-debug`)
  so you can still symbolise cores. This is the Linux version of archiving `.pdb` files
  (05_Memory_Debugging_RCA).

## 6. Try it

1. **(easy)** Run `g++ -E hello.cpp | wc -l` on a hello-world with `<iostream>`, then with
   `<cstdio>`. Done when you have both counts and can explain why the preprocessor output is so
   much larger than the source.
2. **(easy)** Build `code/m02_static_lib` with `build.sh`. Run `nm -C` on `main.o`, `libmathlib.a`
   and `prog_static`. Done when you can point at the `U` becoming a `T`.
3. **(medium)** Build `code/m02_shared_lib`. Make `prog_norpath` work three different ways
   (`LD_LIBRARY_PATH`, copy the `.so` to `/usr/local/lib` + `ldconfig`, relink with `$ORIGIN`).
   Done when `ldd` shows a resolved path in all three cases and you can rank them for production use.
4. **(medium)** Reproduce all three undefined-reference cases in `code/m02_undefined_ref` and, for
   each, write one sentence stating how `nm -C` output told you which case it was.
5. **(hard)** Write `libplugin.so` exporting `extern "C" const char* plugin_name()`, and a host
   program that `dlopen`s it and calls `dlsym`. Build with `-fvisibility=hidden` and show with
   `nm -D` that only `plugin_name` is exported. Done when `./host ./libplugin.so` prints the name
   and `nm -D` lists one symbol of yours.
6. **(hard)** Compile `code/m02_odr_violation/silent_odr` normally (links fine, prints garbage),
   then with `-flto -Wodr` and read the warning. Then fix it properly. Done when you can explain
   why the linker could not catch the first version.

## 7. Interview questions

**Q: Walk me through what happens when you run `g++ main.cpp util.cpp -o prog`.**
The driver preprocesses each file (headers pasted, macros expanded), compiles each to assembly,
assembles each to an ELF relocatable object with symbols and relocations, then invokes `ld` with
both objects plus `crt1.o`, `libstdc++`, `libc` to resolve every undefined symbol and patch
addresses, producing a PIE executable with an interpreter entry pointing at `ld-linux`.

**Q: Static library vs shared library: what is the difference and when do you pick each?**
A `.a` is an archive of object files copied into the executable at link time; no run-time
dependency, bigger binary, relink to update. A `.so` is loaded by `ld.so` at run time, shared across
processes, updatable independently, needs `-fPIC` and a resolvable path. Static for self-contained
deploys and tiny containers; shared for system libraries that need security patches and for plugins.

**Q: What does `-fPIC` do and why do shared libraries need it?**
It makes the compiler emit position-independent code: RIP-relative data access and indirect
calls/loads through the GOT and PLT, so the library can be mapped at any address in any process
without relocating its code pages. Without it, the code would contain absolute addresses and
the pages could not be shared.

**Q: The program links fine but fails at start with "cannot open shared object file". What do you do?**
`ldd ./prog` to see which `NEEDED` library is not found. Then decide: install the runtime package
and run `ldconfig`, set RUNPATH with `$ORIGIN` for a relocatable tree, or as a test
`LD_LIBRARY_PATH`. The search order is RPATH, `LD_LIBRARY_PATH`, RUNPATH, `ld.so.cache`,
`/lib` and `/usr/lib`.

**Q: Why does `g++ -lfoo main.o` fail but `g++ main.o -lfoo` works?**
`ld` scans inputs left to right, pulling from an archive only members that satisfy currently
undefined symbols. Before `main.o` is read nothing is undefined, so `libfoo.a` contributes nothing.
Objects first, then libraries, dependents before dependencies.

**Q: What is an ODR violation and how does it manifest?**
Two definitions of one entity that must be unique, or non-identical definitions of an inline
entity. The first fails at link time with "multiple definition". The second links silently; the
linker keeps one weak copy, and you get crashes or wrong results that depend on link order.
`-flto -Wodr` finds some cases; discipline with headers finds the rest.

**Q: What is name mangling and why does `extern "C"` exist?**
Mangling encodes namespace, class and parameter types into the symbol name so overloads can
coexist at the linker level. `extern "C"` disables it so a C compiler, or another C++ compiler
with a different mangling scheme, can call the function by its plain name.

**Q: What is LTO?**
Link-time optimisation: the compiler emits IR instead of machine code into objects, and the linker
runs whole-program optimisation (cross-TU inlining, dead-code elimination, devirtualisation) before
code generation. `-flto` in GCC/Clang, `/GL /LTCG` in MSVC. Costs link time and memory.

## 8. Recap

- Four stages: preprocess (text), compile (one TU to assembly), assemble (ELF `.o`), link (merge, resolve, relocate).
- The compiler sees one TU; everything it calls but does not define is an undefined symbol `U` with a relocation.
- `nm -C`, `objdump -dr`, `readelf -d`, `ldd` are the tools; learn to read `T`, `U`, `W`.
- Static `.a` is an archive copied in; link order matters; objects before libraries.
- Shared `.so` needs `-fPIC`, has a soname, and is found at run time by RPATH, `LD_LIBRARY_PATH`, RUNPATH, cache, defaults.
- `$ORIGIN` RUNPATH is the relocatable-deploy answer; `LD_LIBRARY_PATH` is for testing only.
- "undefined reference": missing input, wrong order, or signature/mangling mismatch. `nm -C` tells you which.
- "multiple definition": a non-inline definition in a header; use `extern`, `inline`, or `static`.
- Silent ODR violations link fine and crash later; `-flto -Wodr` is the best automated catch.
- LTO lets the linker optimise across TUs; `-flto` / `INTERPROCEDURAL_OPTIMIZATION`.

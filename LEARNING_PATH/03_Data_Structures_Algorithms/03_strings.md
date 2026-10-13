# 03. Strings

**Time:** ~3 hours   **Prerequisites:** 01, 02

## 1. In plain words

In C a string is a `char*` that ends at the first zero byte. You know the consequences:
`strlen` is O(n), you carry a separate length when you care about speed, and every buffer is
a potential overflow. `std::string` fixes this by storing the length, owning its memory and
growing like a vector. Think of it as `std::vector<char>` that also guarantees a trailing
`'\0'` so you can hand `c_str()` to any C API.

Most string interview problems are array problems in disguise: two pointers for palindromes
and reversals, sliding window for "longest substring with property", a 26-entry or 256-entry
count array instead of a hash map. What is specific to strings is parsing (turning text into
numbers and tokens safely), hashing (comparing many substrings cheaply) and pattern matching
(finding a needle in a haystack faster than O(n*m)).

The one new tool this module adds is `std::string_view`: a pointer plus a length that does not
own anything. It lets you slice and pass strings around without copying, which is what you
would do with `const char*, size_t` in C — but with a real type that knows its bounds.

## 2. The real thing

### 2.1 `std::string` essentials

```cpp
std::string s = "hello";          // copies the literal into s
s.size(); s.length();             // same thing, O(1), std::size_t
s[1]; s.at(1);                    // unchecked / checked
s += " world";                    // amortised O(1) per char (vector-like growth)
s + "!"                           // creates a NEW string: O(n). Avoid in loops.
s.substr(pos, len)                // copies: O(len)
s.find("wor")                     // std::string::npos if absent; O(n*m) naive, fine in practice
s.compare(t); s == t; s < t       // lexicographic, byte-wise
s.c_str(); s.data()               // const char* (data() non-const since C++17), '\0'-terminated
s.push_back('x'); s.pop_back(); s.back(); s.front()
s.insert(pos, "x"); s.erase(pos, len)   // O(n), shifts like vector
std::to_string(42); std::stoi("42"); std::stoll("9000000000")
std::getline(std::cin, s);        // read a line
```

Strings are mutable, contiguous, and `'\0'`-terminated (guaranteed since C++11). `s.size()`
does not count the terminator. `s[s.size()]` is `'\0'` and reading it is defined; writing it is
not.

### 2.2 `std::string_view` (C++17)

```cpp
std::string_view sv = s;               // no copy, points into s
sv.substr(2, 3);                       // O(1): a new view, not a new string
sv.remove_prefix(1); sv.remove_suffix(1);
sv.find('x'); sv.starts_with(...)      // starts_with is C++20; in C++17 use sv.substr(0,n) == prefix
std::string owned(sv);                 // explicit copy when you need ownership
```

Rules:
- A `string_view` is **not** `'\0'`-terminated. Never pass `sv.data()` to a C function that
  expects a C string.
- It dangles the moment the underlying string is destroyed or reallocated. Do not store a
  `string_view` as a member unless you control the lifetime of the source. Do not return a
  view of a local `std::string`.
- Take `std::string_view` as a function parameter when you only read: it accepts
  `std::string`, `const char*` and literals without a copy. Take `const std::string&` when
  you need `c_str()`; take `std::string` by value when you will store it.

### 2.3 Characters, bytes, and the 256-table trick

`char` is a byte, signed on x86 GCC. `'a'` is 97. `c - 'a'` maps lowercase letters to 0..25.
Before indexing any table with a `char`, cast: `static_cast<unsigned char>(c)` — otherwise a
byte above 127 indexes with a negative number. For "letters only" problems a `std::array<int,
26>` replaces a hash map and is 10x faster. For arbitrary bytes use `std::array<int, 256>`.

Unicode: `std::string` is a sequence of bytes. In UTF-8, "é" is two bytes and `size()` says 2.
Reversing a UTF-8 string byte by byte breaks it. Interview problems are ASCII unless stated;
say "assuming ASCII" out loud when you reverse or count characters.

`<cctype>`: `std::isdigit`, `std::isalpha`, `std::isspace`, `std::tolower`. They take an `int`
that must be in `unsigned char` range or EOF — cast first.

### 2.4 Parsing safely

`std::stoi` throws `std::invalid_argument` on garbage and `std::out_of_range` on overflow.
`atoi` returns 0 on garbage and has undefined behaviour on overflow — the same behaviour you
know from C. For hand-written parsing (the "string to integer" interview problem), the rules
are: skip whitespace, read an optional sign, accumulate digits, **check overflow before
multiplying**:

```cpp
if (result > (INT_MAX - digit) / 10) return sign == 1 ? INT_MAX : INT_MIN;  // would overflow
result = result * 10 + digit;
```

Tokenising: a loop with `find_first_not_of(delims, pos)` and `find_first_of(delims, start)`
over a `string_view` splits without allocating per token. `std::istringstream` with `>>` is
simpler and fine when performance does not matter.

### 2.5 Hashing strings

A hash function maps a string to an integer so that equal strings give equal hashes and
different strings usually give different hashes. `std::hash<std::string>` (libstdc++ uses
MurmurHash2 on 64-bit) is what `std::unordered_map<std::string, ...>` uses. It reads every
byte: O(length). It is not cryptographic and not stable across platforms or versions — never
persist it or send it over the wire.

**Polynomial rolling hash.** For algorithm problems you want to hash every substring of length
m in O(1) each. Treat the string as a number in base B modulo a prime M:

```
H(s[0..m)) = s[0]*B^(m-1) + s[1]*B^(m-2) + ... + s[m-1]   (mod M)
H(s[i+1..i+m+1)) = (H(s[i..i+m)) - s[i]*B^(m-1)) * B + s[i+m]   (mod M)
```

Slide the window: remove the leading character's contribution, shift, add the new one. All
O(1). Typical constants: B = 131 or 31, M = 1e9+7 or 2^61-1. Collisions are possible
(probability about 1/M per comparison), so either verify a hash match by comparing the actual
substrings, or use two independent moduli. This is Rabin-Karp string search: O(n + m) expected.

### 2.6 KMP: the idea

Naive search for pattern P (length m) in text T (length n) restarts at i+1 after a mismatch:
O(n*m) worst case (e.g. T = "aaaa...a", P = "aaab"). KMP observes that when a mismatch happens
after matching k characters, you already know what those k characters are — they are
P[0..k). The longest proper prefix of P[0..k) that is also a suffix of it tells you how far to
slide without missing a match. Precompute that for every k into the **failure table** `f[k]`
in O(m). Then the text pointer never moves backwards: O(n + m) total.

```
P = "ababc"
f = [0,0,1,2,0]   f[k] = length of longest proper prefix of P[0..k] that is also its suffix
```

Mismatch at P[4]='c' after matching "abab" -> f[3] = 2 -> continue comparing at P[2] without
re-reading text. Interviews ask you to explain the idea; writing KMP bug-free from memory is
rare. The failure-table build is itself a KMP of P against P, which is the elegant part.

## 3. Code

Files: `code/m03_string_basics.cpp` (SSO demonstration, `string_view` usage and dangers,
tokenising), `code/m03_string_problems.cpp` (classic problems), `code/m03_kmp_rabin_karp.cpp`
(rolling hash search and KMP with failure table).

### SSO demonstration

```cpp
std::string s;
const char* before = s.data();
s = "short";                         // fits in the internal buffer: data() unchanged
std::printf("%s\n", s.data() == before ? "inline" : "heap");
s = "this string is longer than fifteen chars";
std::printf("%s\n", s.data() == before ? "inline" : "heap");   // heap
std::printf("sizeof(std::string) = %zu\n", sizeof(std::string));  // 32 on libstdc++
```

### Classic problems in `m03_string_problems.cpp`

1. **Valid palindrome (alphanumerics only, case-insensitive)** — two pointers from both ends,
   skip non-alphanumerics. O(n), O(1).
2. **Reverse words in place** — reverse the whole string, then reverse each word. O(n), O(1).
3. **Valid anagram** — 26-entry count array: +1 for s, -1 for t, all must be zero. O(n), O(1).
4. **Longest substring without repeating characters** — sliding window with a 256-entry
   last-seen index. O(n), O(1).
5. **String to integer (atoi)** — whitespace, sign, digits, overflow clamp. O(n).
6. **Longest palindromic substring** — expand around each centre (2n-1 centres). O(n^2), O(1).
   Mention Manacher's O(n) exists; nobody expects you to write it.
7. **Group anagrams** — key = sorted word (or 26-count string), hash map of vectors. O(n * k log k).
8. **Split by delimiter without allocating tokens** — `string_view` tokeniser.

Each is in the file with a `check()` call; key parts:

```cpp
// 4. longest substring without repeating characters
int longest_unique_substring(std::string_view s) {
    std::array<int, 256> last{};      // last index + 1 where each byte was seen; 0 = never
    int l = 0, best = 0;
    for (int r = 0; r < static_cast<int>(s.size()); ++r) {
        unsigned char c = static_cast<unsigned char>(s[static_cast<std::size_t>(r)]);
        l = std::max(l, last[c]);     // jump l past the previous occurrence, never backwards
        last[c] = r + 1;
        best = std::max(best, r - l + 1);
    }
    return best;
}
```

The `std::max(l, last[c])` line is the whole trick: a window is invalid only if the previous
occurrence of `c` is inside it, and `l` must never move left.

## 4. Under the hood (senior-level)

### 4.1 Small String Optimisation (SSO)

A heap allocation per string would make short strings — most strings — slow. libstdc++'s
`std::string` is 32 bytes: a pointer, a size, and a 16-byte union that is either the capacity
(heap mode) or an inline buffer of 15 chars plus `'\0'` (inline mode). Strings up to 15 bytes
live entirely inside the object: no `new`, no cache miss to reach the characters, and copying
the string is copying 32 bytes. MSVC's layout also gives 15 chars inline; libc++ (Clang on
macOS) packs it differently and gets 22.

Consequences you should be able to state:
- `std::move` of a short string copies 32 bytes; of a long string it steals the pointer. Both
  are O(1).
- `data()` changes when a string crosses the 15-char boundary — any pointer you took is dead.
- A `std::vector<std::string>` of short strings has good locality; of long strings it is a
  vector of pointers in disguise, one cache miss per string to read the text.
- `sizeof(std::string) == 32` versus `sizeof(std::string_view) == 16`. Passing a view is two
  registers.

### 4.2 Growth and `reserve`

Like vector: `+=` doubles capacity on overflow (libstdc++ grows to max(2*cap, needed)). Building
a 1 MB string with `+=` does about 17 reallocations. Call `reserve` when you know the size, e.g.
when serialising a message of known length in your RPC server.

### 4.3 Why `s = s + c` in a loop is O(n^2)

`s + c` constructs a brand-new string of size n+1 and copies n chars. Then assignment moves it
into `s`. Across n iterations that is 1 + 2 + ... + n = O(n^2) copies. `s += c` appends in
place: amortised O(1). Same for `s = s.substr(1)` in a loop (O(n) per call). Use indices or a
`string_view` to "consume" a string instead of making smaller copies.

### 4.4 `std::string::find` and `strstr`

libstdc++'s `find` for a multi-character needle is a straightforward scan using `memchr` for
the first character then `memcmp` — worst case O(n*m) but with hardware-accelerated inner
loops that make it faster than KMP for realistic text. glibc's `strstr` uses the Two-Way
algorithm (O(n+m) worst case). Takeaway: in production, use the library. In an interview, know
why naive is O(n*m) and what KMP/Rabin-Karp do about it.

### 4.5 Hash quality and DoS

If an attacker can choose keys, they can pick thousands that collide under a known hash and
turn every `unordered_map` lookup into O(n). This happened to several web frameworks around
2011 ("hash flooding"). Defences: randomised per-process seed (SipHash in Python, Rust),
switching to a tree when a bucket grows too long (Java 8 HashMap). libstdc++ does neither.
If your server hashes untrusted strings (account numbers from the network are untrusted),
know this. Module 04 covers the table side.

## 5. In production

- **Message parsing in the RPC server.** A fixed-format message header (`"LEN=00120;TYPE=TXN;"`)
  should be parsed with `string_view` slices and a hand-written digit loop with overflow
  checks, not `atoi` on `strtok` output. `strtok` is not thread-safe (static state), which
  matters in your multithreaded server; `strtok_r` or `string_view` tokenising are.
- **Logging.** `std::string line = a + ", " + b + ", " + c;` creates temporaries. Fine at 1 kHz.
  At 100 kHz, `reserve` and `+=` or `std::snprintf` into a stack buffer. Measure first.
- **Case-insensitive comparison.** Do not lowercase both strings (two allocations). Compare
  with `std::tolower` on each char pair, or use a locale-free custom comparator.
- **Account numbers as keys.** A 16-digit account number stored as `std::string` takes 32
  bytes in the object (fits SSO) and hashes in ~16 byte reads. Storing it as `std::uint64_t`
  takes 8 bytes and hashes in one multiply. When a map has 10 million entries this is 240 MB.
  Choose the key type on purpose.
- **War story.** A log-parsing tool read 2 GB of text with `std::getline` and did
  `tokens.push_back(line.substr(a, b - a))` for every field. Profile: 70% of time in
  `malloc`/`free`. Switching tokens to `std::string_view` into the line (which outlived the
  processing of its tokens) made it 6x faster with no algorithmic change.

## 6. Try it

1. **(easy)** Write `bool is_palindrome(std::string_view)` ignoring non-alphanumerics and case.
   Done when `"A man, a plan, a canal: Panama"` is true and `"race a car"` is false.
2. **(easy)** Write `std::vector<std::string_view> split(std::string_view s, char delim)` that
   skips empty tokens. Done when `",a,,b,"` gives `["a","b"]` and no `std::string` is created.
3. **(medium)** Implement `my_atoi` with overflow clamping. Done when `"  -91283472332"` gives
   INT_MIN, `"4193 with words"` gives 4193, `"words 987"` gives 0.
4. **(medium)** Longest repeating character replacement: longest substring that can be made all
   one letter by changing at most k letters. Hint: window is valid when
   `length - maxFrequencyInWindow <= k`; you never need to decrease the max. Done when
   `("AABABBA", 1) -> 4`.
5. **(hard)** Implement Rabin-Karp with two moduli and verify a match with `memcmp`. Count
   occurrences of a pattern in a 10 MB random string of 'a'..'d' and compare the time with
   `std::string::find` in a loop. Done when both counts agree and you can explain which is
   faster and why.
6. **(hard)** Write the KMP failure table and search, test on `"aaaaab"` in `"aaaaaaaaab"` and
   on `"abcabd"` in `"abcabcabd"`. Done when both find the correct index and you can explain the
   value of every failure-table entry for `"abcabd"`.

## 7. Interview questions

**Q: What is SSO and why does it matter?**
Small String Optimisation stores short strings (15 chars on libstdc++) inside the 32-byte
object, avoiding heap allocation. Most strings are short, so this removes most allocations and
gives cache locality. It also means `data()` moves when a string grows past the threshold.

**Q: `std::string` vs `std::string_view` — when do you use which?**
View for read-only parameters and slicing without copies; it is 16 bytes and non-owning.
String when you need ownership, mutation or a `'\0'`-terminated buffer. Never store a view
beyond the lifetime of its source.

**Q: Why is `s = s + ch` in a loop quadratic and what is the fix?**
Each `+` builds a new string of the current length, so total copies are 1+2+...+n. Use `+=`,
which appends in place with amortised O(1) per character, or `reserve` first.

**Q: How would you check if two strings are anagrams?**
Count characters: +1 for one string, -1 for the other, in a 26-entry (or 256-entry) array;
all counts must end at zero. O(n) time, O(1) space. Sorting both and comparing is O(n log n).

**Q: Explain rolling hash and what it is used for.**
Treat the string as a base-B number mod a prime. Sliding one character right updates the hash
in O(1): subtract the outgoing char times B^(m-1), multiply by B, add the incoming char. Used
in Rabin-Karp search, duplicate-substring detection, and plagiarism checks. Verify matches or
use two moduli because collisions are possible.

**Q: What is the idea of KMP?**
On a mismatch after matching k pattern characters, use the precomputed longest proper
prefix-suffix length of P[0..k) to shift the pattern without rewinding the text pointer.
Preprocessing O(m), search O(n). The text pointer never goes back.

**Q: Why cast `char` to `unsigned char` before indexing a table or calling `isdigit`?**
`char` is signed on x86 GCC; bytes above 127 become negative, which indexes before the table
start (undefined behaviour) and is outside the range `<cctype>` functions accept.

**Q: How does hash flooding work and how do you defend against it?**
An attacker sends many keys that collide under the known hash, degrading `unordered_map` to
O(n) per operation. Defences: a per-process random seed in the hash, limiting input size, or
a structure that degrades gracefully (tree-backed buckets).

## 8. Recap

- `std::string` is a growable, contiguous, `'\0'`-terminated byte array; `size()` is O(1).
- SSO keeps up to 15 chars inline on libstdc++; `sizeof(std::string)` is 32; crossing the
  threshold reallocates and moves `data()`.
- `string_view` is pointer + length, 16 bytes, non-owning, not `'\0'`-terminated; use for
  read-only parameters and O(1) slicing; never let it outlive its source.
- `+=` is amortised O(1); `s + c` in a loop is O(n^2); `substr` copies.
- Cast `char` to `unsigned char` before table indexing or `<cctype>`.
- Letters-only problems: 26-entry array beats a hash map.
- Rolling hash: O(1) per slide; verify matches or double the modulus.
- KMP: failure table from the pattern, text pointer never rewinds, O(n + m).
- Hashes from `std::hash` are not stable across builds; never persist them.
- Parse untrusted text with explicit overflow checks; `strtok` is not thread-safe.

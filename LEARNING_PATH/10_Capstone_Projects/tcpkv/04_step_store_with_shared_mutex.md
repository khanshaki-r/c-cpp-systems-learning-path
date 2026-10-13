# Step 04. Store with `std::shared_mutex`: single-thread tests, then multi-thread stress

**Time:** ~6 hours   **Week:** 2 (Oct 13 – Oct 19)
**Read first:** 04_Concurrency: data races and the C++ memory model (what "undefined
behaviour" means for a torn read), `std::mutex`, `std::lock_guard`/`std::unique_lock`,
`std::shared_mutex` and `std::shared_lock`, `std::atomic`, `std::thread` and `join`,
ThreadSanitizer. 02_Modern_CPP: `std::unordered_map`, `std::optional`, pass-by-value and
`std::move` for sink parameters, `const` member functions and `mutable`.

## 1. Goal

The in-memory table, safe to use from any number of threads at once. This step builds the
version without TTL (expiry arrives in step 09); the API is designed so step 09 only adds
methods.

In plain words: an `unordered_map<string, string>` with a lock around it. The interesting
part is *which* lock. A plain mutex lets one thread in at a time; a `shared_mutex` lets any
number of readers in together but gives a writer the room to itself. A key-value server is
read-heavy, so this is the standard choice, and you should be able to say when it is the
wrong one (see section 7).

## 2. Design

```
          GET / EXISTS / KEYS / size            SET / DEL
                 |                                  |
          shared_lock (many at once)       unique_lock (one, excludes readers)
                 |                                  |
                 +------------> std::unordered_map<std::string, std::string> <------+
```

* One `std::shared_mutex`, declared `mutable` so `const` readers can lock it.
* Readers copy the value out *under the lock* and return the copy. Returning a reference or
  a `string_view` into the map would be a use-after-free the moment a writer runs.
* `set` takes `std::string key, std::string value` **by value** and moves them in. Callers
  that have a temporary pay nothing; callers with an lvalue pay one copy, which they would
  have paid anyway.
* `keys()` collects under the shared lock and sorts *after* releasing it. Never do more work
  under a lock than you must.
* Nothing is `static`, nothing is global: tests can create ten `Store`s.

## 3. Files to create

### `include/tcpkv/store.hpp` (step-04 version)

```cpp
#pragma once

#include <cstddef>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tcpkv {

// In-memory key/value store.
//
// Concurrency: one std::shared_mutex. Readers (GET, EXISTS, KEYS) take a shared lock and run
// in parallel; writers (SET, DEL) take an exclusive lock. The map is never touched without
// the lock, so every method is safe to call from any thread.
class Store {
public:
    Store() = default;
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    std::optional<std::string> get(const std::string& key) const;
    void set(std::string key, std::string value);
    bool del(const std::string& key);  // true if the key existed
    bool exists(const std::string& key) const;

    // Keys starting with `prefix` (empty prefix = all keys), sorted ascending.
    std::vector<std::string> keys(std::string_view prefix = {}) const;

    std::size_t size() const;

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::string> map_;
};

}  // namespace tcpkv
```

### `src/store.cpp` (step-04 version)

```cpp
#include "tcpkv/store.hpp"

#include <algorithm>
#include <mutex>
#include <utility>

namespace tcpkv {

std::optional<std::string> Store::get(const std::string& key) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto it = map_.find(key);
    if (it == map_.end()) {
        return std::nullopt;
    }
    return it->second;  // copy made under the lock; the caller owns it afterwards
}

void Store::set(std::string key, std::string value) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    map_[std::move(key)] = std::move(value);
}

bool Store::del(const std::string& key) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    return map_.erase(key) > 0;
}

bool Store::exists(const std::string& key) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return map_.find(key) != map_.end();
}

std::vector<std::string> Store::keys(std::string_view prefix) const {
    std::vector<std::string> out;
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        for (const auto& [key, value] : map_) {
            (void)value;
            if (key.size() >= prefix.size() && key.compare(0, prefix.size(), prefix) == 0) {
                out.push_back(key);
            }
        }
    }
    std::sort(out.begin(), out.end());  // sort after releasing the lock
    return out;
}

std::size_t Store::size() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return map_.size();
}

}  // namespace tcpkv
```

`map_[std::move(key)] = std::move(value)` does an insert-or-assign in one hash lookup. The
`(void)value;` silences the unused-binding warning for the structured binding; step 09 uses
`value` and removes it.

### `tests/store_test.cpp` (step-04 version)

```cpp
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "tcpkv/store.hpp"

using tcpkv::Store;

// ------------------------------------------------------------------ single-threaded semantics

TEST(Store, GetMissingKeyReturnsNullopt) {
    Store store;
    EXPECT_FALSE(store.get("nope").has_value());
    EXPECT_EQ(store.size(), 0u);
}

TEST(Store, SetThenGet) {
    Store store;
    store.set("k", "v");
    const auto value = store.get("k");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, "v");
    EXPECT_EQ(store.size(), 1u);
}

TEST(Store, SetOverwrites) {
    Store store;
    store.set("k", "one");
    store.set("k", "two");
    EXPECT_EQ(store.get("k").value_or(""), "two");
    EXPECT_EQ(store.size(), 1u);
}

TEST(Store, EmptyAndBinaryValuesRoundTrip) {
    Store store;
    store.set("empty", "");
    const std::string binary("\x00\x01\xff\n", 4);
    store.set("bin", binary);
    ASSERT_TRUE(store.get("empty").has_value());
    EXPECT_TRUE(store.get("empty")->empty());
    EXPECT_EQ(store.get("bin").value_or(""), binary);
}

TEST(Store, DelReportsWhetherKeyExisted) {
    Store store;
    store.set("k", "v");
    EXPECT_TRUE(store.del("k"));
    EXPECT_FALSE(store.del("k"));
    EXPECT_FALSE(store.get("k").has_value());
}

TEST(Store, Exists) {
    Store store;
    EXPECT_FALSE(store.exists("k"));
    store.set("k", "v");
    EXPECT_TRUE(store.exists("k"));
}

TEST(Store, KeysAreSortedAndFilteredByPrefix) {
    Store store;
    store.set("user:2", "");
    store.set("user:1", "");
    store.set("order:9", "");

    const std::vector<std::string> all = {"order:9", "user:1", "user:2"};
    EXPECT_EQ(store.keys(), all);
    const std::vector<std::string> users = {"user:1", "user:2"};
    EXPECT_EQ(store.keys("user:"), users);
    EXPECT_TRUE(store.keys("zzz").empty());
}

// ------------------------------------------------------------------ multi-threaded stress
//
// These tests cannot prove the absence of races, but under ThreadSanitizer (cmake --preset tsan)
// any access to the map outside the lock is reported. Without TSan they still catch torn
// values and crashes.

namespace {

constexpr int kKeys = 32;
constexpr int kIterations = 20000;

std::string key_for(int i) { return "k" + std::to_string(i % kKeys); }

// Writer `id` always stores a run of one repeated letter, so a reader can detect a torn value.
std::string value_for(int writer, int i) {
    return std::string(static_cast<std::size_t>(1 + i % 64), static_cast<char>('a' + writer));
}

bool looks_valid(const std::string& value, int writers) {
    if (value.empty() || value.size() > 64) {
        return false;
    }
    const char c = value.front();
    if (c < 'a' || c >= 'a' + writers) {
        return false;
    }
    for (const char x : value) {
        if (x != c) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST(StoreStress, ConcurrentReadersAndWriters) {
    constexpr int kWriters = 4;
    constexpr int kReaders = 4;

    Store store;
    std::atomic<int> torn{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;

    for (int w = 0; w < kWriters; ++w) {
        threads.emplace_back([&, w] {
            while (!go.load()) {
                std::this_thread::yield();
            }
            for (int i = 0; i < kIterations; ++i) {
                store.set(key_for(i), value_for(w, i));
            }
        });
    }
    for (int r = 0; r < kReaders; ++r) {
        threads.emplace_back([&, r] {
            while (!go.load()) {
                std::this_thread::yield();
            }
            for (int i = 0; i < kIterations; ++i) {
                const auto value = store.get(key_for(i + r));
                if (value && !looks_valid(*value, kWriters)) {
                    ++torn;
                }
                if (i % 1000 == 0) {
                    EXPECT_LE(store.keys("k").size(), static_cast<std::size_t>(kKeys));
                    (void)store.exists(key_for(i));
                }
            }
        });
    }
    go.store(true);
    for (std::thread& t : threads) {
        t.join();
    }

    EXPECT_EQ(torn.load(), 0);
    EXPECT_EQ(store.size(), static_cast<std::size_t>(kKeys));
    for (int i = 0; i < kKeys; ++i) {
        const auto value = store.get(key_for(i));
        ASSERT_TRUE(value.has_value());
        EXPECT_TRUE(looks_valid(*value, kWriters));
    }
}

TEST(StoreStress, SetAndDeleteRace) {
    constexpr int kWriters = 3;

    Store store;
    std::atomic<int> torn{0};
    std::vector<std::thread> threads;

    for (int w = 0; w < kWriters; ++w) {
        threads.emplace_back([&, w] {
            for (int i = 0; i < kIterations; ++i) {
                store.set(key_for(i), value_for(w, i));
            }
        });
    }
    threads.emplace_back([&] {
        for (int i = 0; i < kIterations; ++i) {
            store.del(key_for(i * 7));
        }
    });
    threads.emplace_back([&] {
        for (int i = 0; i < kIterations; ++i) {
            const auto value = store.get(key_for(i));
            if (value && !looks_valid(*value, kWriters)) {
                ++torn;
            }
        }
    });
    for (std::thread& t : threads) {
        t.join();
    }

    EXPECT_EQ(torn.load(), 0);
    // Whatever interleaving happened, the store must still be fully usable.
    for (int i = 0; i < kKeys; ++i) {
        store.set(key_for(i), "final");
    }
    EXPECT_EQ(store.size(), static_cast<std::size_t>(kKeys));
    for (int i = 0; i < kKeys; ++i) {
        EXPECT_EQ(store.get(key_for(i)).value_or(""), "final");
    }
}
```

The stress tests are designed to maximise interleavings: many threads, a tiny key space (32
keys), no sleeps, and a `go` flag so all threads start together. The "repeated letter" trick
means a torn read (half of one writer's value, half of another's) is detectable by content.

### `CMakeLists.txt`

Add `src/store.cpp` to the library and `store` to the test list.

## 4. Build and run

```bash
cmake --build --preset debug && ctest --preset debug
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan
```

Expected: all pass. The TSan run of the stress tests takes a few seconds; that is normal.

## 5. Prove the test can fail

This is the most valuable twenty minutes of the step. Comment out the lock in `Store::set`:

```cpp
void Store::set(std::string key, std::string value) {
    // std::unique_lock<std::shared_mutex> lock(mutex_);
    map_[std::move(key)] = std::move(value);
}
```

Rebuild and run the stress tests three ways:

1. `ctest --preset debug`: likely a crash (`unordered_map` rehash while another thread reads)
   or `torn > 0`, but possibly a pass. Races are not deterministic.
2. `ctest --preset asan`: probably a heap-use-after-free report from a reader holding a
   pointer into a bucket that a writer freed.
3. `ctest --preset tsan`: a *deterministic* report naming both threads, both stacks and the
   two accesses: "WARNING: ThreadSanitizer: data race ... Write of size 8 by thread T1 ...
   Previous read of size 8 by thread T5".

Put the lock back. You now know what each tool does and does not promise, which is exactly
the question an interviewer asks after "you say it's thread-safe; how do you know?".

## 6. Commit

```bash
git add -A
git commit -m "Add Store: unordered_map behind shared_mutex, with stress tests

- readers (get/exists/keys/size) take shared_lock, writers (set/del) unique_lock
- values copied out under the lock; keys() sorts after unlocking
- set() takes key/value by value and moves them in
- single-thread semantics tests plus two multi-thread stress tests designed for TSan"
git push
```

## 7. What you learned, and what to say in an interview

* A **data race** (two threads, one writes, no synchronisation) is undefined behaviour in
  C++, not "sometimes wrong". The compiler may assume it does not happen.
* **`shared_mutex`** fits read-heavy workloads. It is *not* free: it has more internal state
  than a mutex, writers can starve or be starved depending on the implementation (glibc's
  `pthread_rwlock` prefers readers by default), and for very short critical sections a plain
  mutex is often faster because of less bookkeeping. Measure before choosing.
* **Alternatives you can name**: sharded locks (N maps, N mutexes, pick by hash) to cut
  writer contention; a single `std::mutex` for simplicity; lock-free hash maps (much harder,
  rarely worth it); RCU-style copy-on-write for read-mostly data.
* **Never return a reference into a locked structure.** Copy under the lock.
* **Testing concurrency**: stress tests find crashes; TSan finds races. Both, always.

Interview sentence: "The store is one `unordered_map` behind one `shared_mutex`; readers
share, writers exclude. I verified it by removing the lock and watching TSan name the race,
then putting it back. If profiling showed writer contention I would shard by key hash into
sixteen maps with sixteen locks before considering anything lock-free."

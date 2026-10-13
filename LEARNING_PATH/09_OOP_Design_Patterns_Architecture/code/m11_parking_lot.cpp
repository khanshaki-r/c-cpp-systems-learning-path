// m11_parking_lot.cpp
// LLD WALKTHROUGH: a multi-level parking lot.
//
// Requirements (as clarified):
//   - Several levels. Each level has spots of three sizes: Small, Medium, Large.
//   - Vehicles: Motorcycle (fits any spot), Car (Medium or Large), Truck (Large).
//   - park(vehicle, entry time) -> Ticket, or "full" for that vehicle type.
//     Allocation: the SMALLEST spot that fits, on the LOWEST level (strategy,
//     swappable).
//   - unpark(ticket id, exit time) -> fee. Fee depends on vehicle type and
//     duration, rounded up to whole hours (strategy, swappable).
//   - Availability query per size. Thread-safe (several gates at once).
//
// Entities (nouns -> classes):   Vehicle, Spot, Level, Ticket, ParkingLot
// Behaviours that vary (-> strategies): SpotAllocator, FeePolicy
//
//   +------------+ 1     * +-------+ 1    * +------+
//   | ParkingLot |<>------->| Level |<>----->| Spot |
//   +------------+          +-------+        +------+
//   | park()     |   uses   +----------------+     uses  +-----------+
//   | unpark()   |--------->| SpotAllocator  |  -------->| FeePolicy |
//   +------------+          +----------------+           +-----------+
//         | 1..* active
//         v
//   +--------+
//   | Ticket |  (value type: id, plate, type, spot, entry time)
//   +--------+
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -o m11a m11_parking_lot.cpp

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using Minutes   = std::chrono::minutes;
// Wall-clock time is right here: fees are billed in civil time. (C++20 names this sys_seconds.)
using TimePoint = std::chrono::time_point<std::chrono::system_clock, std::chrono::seconds>;

enum class VehicleType { Motorcycle, Car, Truck };
enum class SpotSize    { Small = 0, Medium = 1, Large = 2 };
constexpr std::size_t kSizes = 3;

// The fit rule lives in ONE function. Adding a Bus later changes this
// function and the fee table, nothing else.
constexpr SpotSize smallest_fit(VehicleType t) {
    switch (t) {
        case VehicleType::Motorcycle: return SpotSize::Small;
        case VehicleType::Car:        return SpotSize::Medium;
        case VehicleType::Truck:      return SpotSize::Large;
    }
    return SpotSize::Large;   // unreachable; keeps -Wreturn-type quiet
}

struct Vehicle {
    std::string plate;
    VehicleType type;
};

// A spot is identified by (level, index). Small value type: copy freely.
struct SpotId {
    int level;
    int index;
    bool operator==(const SpotId& o) const { return level == o.level && index == o.index; }
};

struct Ticket {
    std::uint64_t id;
    std::string plate;
    VehicleType type;
    SpotId spot;
    SpotSize size;
    TimePoint entry;
};

// A Level only knows its spots and which are free. It does not know about
// tickets, fees or vehicles: single responsibility, and trivially testable.
class Level {
public:
    Level(int number, int small, int medium, int large) : number_(number) {
        int idx = 0;
        const std::array<int, kSizes> counts{small, medium, large};
        for (std::size_t s = 0; s < kSizes; ++s)
            for (int i = 0; i < counts[s]; ++i) free_[s].insert(idx++);
    }
    int number() const noexcept { return number_; }

    // std::set gives "lowest-numbered free spot" in O(log n): nearest the gate.
    std::optional<SpotId> take(SpotSize s) {
        auto& f = free_[static_cast<std::size_t>(s)];
        if (f.empty()) return std::nullopt;
        int idx = *f.begin();
        f.erase(f.begin());
        return SpotId{number_, idx};
    }
    void give_back(SpotSize s, int index) {
        bool inserted = free_[static_cast<std::size_t>(s)].insert(index).second;
        assert(inserted && "double free of a spot");
        (void)inserted;
    }
    std::size_t free_count(SpotSize s) const { return free_[static_cast<std::size_t>(s)].size(); }

private:
    int number_;
    std::array<std::set<int>, kSizes> free_;
};

// ---------------------------------------------------------------------------
// Strategies. One operation each, so std::function (Module 06, 2.1).
// ---------------------------------------------------------------------------
using SpotAllocator = std::function<std::optional<std::pair<SpotId, SpotSize>>(
    std::vector<Level>&, VehicleType)>;
using FeePolicy = std::function<std::int64_t /*paise*/(VehicleType, Minutes)>;

// Smallest fitting size first, lowest level first within that size.
inline std::optional<std::pair<SpotId, SpotSize>>
smallest_fit_lowest_level(std::vector<Level>& levels, VehicleType t) {
    for (auto s = static_cast<std::size_t>(smallest_fit(t)); s < kSizes; ++s)
        for (auto& lvl : levels)
            if (auto spot = lvl.take(static_cast<SpotSize>(s)))
                return std::make_pair(*spot, static_cast<SpotSize>(s));
    return std::nullopt;
}

// Hourly rate per vehicle type, partial hours rounded up, first 15 min free.
inline FeePolicy hourly_fee(std::int64_t moto, std::int64_t car, std::int64_t truck) {
    return [=](VehicleType t, Minutes d) -> std::int64_t {
        if (d <= Minutes(15)) return 0;
        const std::int64_t hours = (d.count() + 59) / 60;
        const std::int64_t rate = t == VehicleType::Motorcycle ? moto
                                : t == VehicleType::Car        ? car : truck;
        return hours * rate;
    };
}

enum class ParkError { Full, AlreadyParked };
enum class UnparkError { UnknownTicket, ExitBeforeEntry };

// A tiny result type (C++23 has std::expected; Module 10 discusses it).
template <class T, class E>
struct Result {
    std::optional<T> value;
    E error{};
    explicit operator bool() const { return value.has_value(); }
};

// ---------------------------------------------------------------------------
// The facade the gates talk to. Owns the levels and the active tickets.
// One mutex: park/unpark are microseconds of work, gates are a handful of
// threads; a finer lock would be complexity with no measurable gain.
// ---------------------------------------------------------------------------
class ParkingLot {
public:
    ParkingLot(std::vector<Level> levels, SpotAllocator alloc, FeePolicy fee)
        : levels_(std::move(levels)), alloc_(std::move(alloc)), fee_(std::move(fee)) {}

    Result<Ticket, ParkError> park(const Vehicle& v, TimePoint now) {
        std::lock_guard<std::mutex> g(m_);
        if (plates_.count(v.plate)) return {std::nullopt, ParkError::AlreadyParked};
        auto found = alloc_(levels_, v.type);
        if (!found) return {std::nullopt, ParkError::Full};
        Ticket t{next_id_++, v.plate, v.type, found->first, found->second, now};
        active_.emplace(t.id, t);
        plates_.emplace(v.plate, t.id);
        return {t, ParkError{}};
    }

    Result<std::int64_t, UnparkError> unpark(std::uint64_t ticket_id, TimePoint now) {
        std::lock_guard<std::mutex> g(m_);
        auto it = active_.find(ticket_id);
        if (it == active_.end()) return {std::nullopt, UnparkError::UnknownTicket};
        const Ticket& t = it->second;
        if (now < t.entry) return {std::nullopt, UnparkError::ExitBeforeEntry};
        const auto fee = fee_(t.type, std::chrono::duration_cast<Minutes>(now - t.entry));
        level(t.spot.level).give_back(t.size, t.spot.index);
        plates_.erase(t.plate);
        active_.erase(it);
        return {fee, UnparkError{}};
    }

    std::size_t free_spots(SpotSize s) const {
        std::lock_guard<std::mutex> g(m_);
        std::size_t n = 0;
        for (const auto& l : levels_) n += l.free_count(s);
        return n;
    }

private:
    Level& level(int number) {
        for (auto& l : levels_) if (l.number() == number) return l;
        assert(false && "ticket refers to unknown level");
        return levels_.front();
    }

    mutable std::mutex m_;
    std::vector<Level> levels_;
    SpotAllocator alloc_;
    FeePolicy fee_;
    std::unordered_map<std::uint64_t, Ticket> active_;
    std::unordered_map<std::string, std::uint64_t> plates_;   // one vehicle, one ticket
    std::uint64_t next_id_ = 1;
};

int main() {
    using namespace std::chrono;
    const TimePoint t0{seconds{1'700'000'000}};

    std::vector<Level> levels;
    levels.emplace_back(0, 1, 1, 1);   // ground: 1 small, 1 medium, 1 large
    levels.emplace_back(1, 0, 1, 0);   // level 1: 1 medium
    ParkingLot lot(std::move(levels), smallest_fit_lowest_level, hourly_fee(1000, 3000, 8000));

    // Smallest fitting spot, lowest level.
    auto bike = lot.park({"KA01M1", VehicleType::Motorcycle}, t0);
    assert(bike && bike.value->size == SpotSize::Small && bike.value->spot.level == 0);

    auto car1 = lot.park({"KA01C1", VehicleType::Car}, t0);
    assert(car1 && car1.value->size == SpotSize::Medium && car1.value->spot.level == 0);
    auto car2 = lot.park({"KA01C2", VehicleType::Car}, t0);
    assert(car2 && car2.value->size == SpotSize::Medium && car2.value->spot.level == 1);
    auto car3 = lot.park({"KA01C3", VehicleType::Car}, t0);   // spills into Large
    assert(car3 && car3.value->size == SpotSize::Large);

    auto truck = lot.park({"KA01T1", VehicleType::Truck}, t0);
    assert(!truck && truck.error == ParkError::Full);

    auto dup = lot.park({"KA01C1", VehicleType::Car}, t0);
    assert(!dup && dup.error == ParkError::AlreadyParked);

    // A second motorcycle no longer finds Small; Medium and Large are full too.
    assert(!lot.park({"KA01M2", VehicleType::Motorcycle}, t0));

    // Fees: 10 min free; 61 min = 2 hours.
    auto f1 = lot.unpark(bike.value->id, t0 + minutes(10));
    assert(f1 && *f1.value == 0);
    auto f2 = lot.unpark(car3.value->id, t0 + minutes(61));
    assert(f2 && *f2.value == 6000);
    assert(lot.free_spots(SpotSize::Large) == 1);

    auto again = lot.unpark(car3.value->id, t0 + minutes(70));
    assert(!again && again.error == UnparkError::UnknownTicket);

    // The large spot is free again: the truck fits now.
    assert(lot.park({"KA01T1", VehicleType::Truck}, t0 + minutes(70)));

    // Concurrency smoke: 4 gates racing for 2 free spots (Small + one Medium).
    assert(lot.unpark(car1.value->id, t0 + minutes(80)));
    std::atomic<int> ok{0};
    std::vector<std::thread> gates;
    for (int g = 0; g < 4; ++g)
        gates.emplace_back([&, g] {
            if (lot.park({"GATE" + std::to_string(g), VehicleType::Motorcycle}, t0)) ++ok;
        });
    for (auto& th : gates) th.join();
    assert(ok == 2);

    std::puts("m11_parking_lot: all checks passed");
    return 0;
}

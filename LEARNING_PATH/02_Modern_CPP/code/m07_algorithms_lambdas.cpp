// m07_algorithms_lambdas.cpp
// <algorithm> idioms: sort with comparator, erase-remove, accumulate, transform, lower_bound, partition.
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread m07_algorithms_lambdas.cpp
#include <algorithm>
#include <iostream>
#include <iterator>
#include <numeric>
#include <string>
#include <vector>

struct Txn {
    std::string account;
    long long paise;
    int priority;
};

int main() {
    std::vector<Txn> txns{{"A1", 500, 2}, {"B2", -200, 1}, {"A1", 1500, 3}, {"C3", 0, 2}, {"B2", 800, 1}};

    // sort with a comparator. The comparator must be a STRICT WEAK ORDERING: return true only when
    // a must come strictly before b. Returning true for equal elements is undefined behaviour.
    std::sort(txns.begin(), txns.end(), [](const Txn& a, const Txn& b) {
        if (a.priority != b.priority) return a.priority > b.priority;   // high priority first
        return a.account < b.account;                                   // tie-break by account
    });
    for (const auto& t : txns) std::cout << t.account << ':' << t.paise << "(p" << t.priority << ") ";
    std::cout << '\n';

    // erase-remove idiom: remove_if only MOVES the survivors to the front and returns the new end;
    // erase() actually shrinks the container. Forgetting erase() is a classic bug.
    txns.erase(std::remove_if(txns.begin(), txns.end(), [](const Txn& t) { return t.paise == 0; }), txns.end());
    std::cout << "after removing zero-amount txns: " << txns.size() << '\n';

    // accumulate: the type of the INITIAL VALUE is the accumulator type. 0 would sum in int and overflow.
    long long total = std::accumulate(txns.begin(), txns.end(), 0LL,
                                      [](long long acc, const Txn& t) { return acc + t.paise; });
    std::cout << "total=" << total << '\n';

    // transform into a different container through back_inserter (grows the destination as needed)
    std::vector<std::string> labels;
    std::transform(txns.begin(), txns.end(), std::back_inserter(labels),
                   [](const Txn& t) { return t.account + "/" + std::to_string(t.paise); });
    for (const auto& l : labels) std::cout << l << ' ';
    std::cout << '\n';

    // lower_bound / upper_bound on a SORTED range: O(log n). On an unsorted range the result is garbage.
    std::vector<int> sorted{10, 20, 20, 30, 40};
    auto lb = std::lower_bound(sorted.begin(), sorted.end(), 20);   // first element >= 20
    auto ub = std::upper_bound(sorted.begin(), sorted.end(), 20);   // first element >  20
    std::cout << "20 starts at index " << (lb - sorted.begin()) << ", occurs " << (ub - lb) << " times\n";
    bool has25 = std::binary_search(sorted.begin(), sorted.end(), 25);
    std::cout << "has 25? " << has25 << '\n';

    // count_if, any_of, min_element: say WHAT you want, not how to loop
    auto negatives = std::count_if(txns.begin(), txns.end(), [](const Txn& t) { return t.paise < 0; });
    bool any_big   = std::any_of(txns.begin(), txns.end(), [](const Txn& t) { return t.paise > 1000; });
    auto smallest  = std::min_element(txns.begin(), txns.end(),
                                      [](const Txn& a, const Txn& b) { return a.paise < b.paise; });
    std::cout << "negatives=" << negatives << " any_big=" << any_big << " smallest=" << smallest->paise << '\n';

    // stable_sort keeps the relative order of equal keys; partition splits in place and returns the boundary
    std::stable_sort(txns.begin(), txns.end(), [](const Txn& a, const Txn& b) { return a.account < b.account; });
    auto mid = std::partition(txns.begin(), txns.end(), [](const Txn& t) { return t.paise > 0; });
    std::cout << "credits=" << (mid - txns.begin()) << " debits=" << (txns.end() - mid) << '\n';

    // iota + shuffle-free deterministic example of generate / for_each
    std::vector<int> ids(5);
    std::iota(ids.begin(), ids.end(), 100);                 // 100, 101, ...
    std::for_each(ids.begin(), ids.end(), [](int& x) { x *= 2; });
    std::reverse(ids.begin(), ids.end());
    for (int x : ids) std::cout << x << ' ';
    std::cout << '\n';
    return 0;
}

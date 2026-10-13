// buggy.cpp - two deliberate bugs for Valgrind and AddressSanitizer to find.
// Build normal : g++ -std=c++17 -Wall -Wextra -Werror -pthread -g -O0 buggy.cpp -o buggy
// Valgrind     : valgrind --leak-check=full ./buggy
// ASan         : g++ -std=c++17 -Wall -Wextra -Werror -pthread -g -O1 -fsanitize=address,undefined
//                    -fno-omit-frame-pointer buggy.cpp -o buggy_asan   (one line) then ./buggy_asan
#include <iostream>

int main() {
    const int n = 10;
    int* data = new int[n];
    for (int i = 0; i < n; ++i) data[i] = i * i;

    // Bug 1: reads data[10], one past the end of the allocation.
    // ASan: heap-buffer-overflow. Valgrind: Invalid read of size 4.
    long sum = 0;
    for (int i = 0; i <= n; ++i) sum += data[i];
    std::cout << "sum = " << sum << '\n';

    // Bug 2: the array is never deleted.
    // ASan/LeakSanitizer: detected memory leaks. Valgrind: definitely lost: 40 bytes.
    return 0;
}

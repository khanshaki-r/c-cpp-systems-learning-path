// m03_string_basics.cpp
// SSO in action, string_view usage and its dangling trap, non-allocating tokeniser,
// safe integer parsing, and the quadratic string-building anti-pattern measured.
//
// Build: g++ -std=c++17 -O2 -Wall -Wextra -Werror -pthread m03_string_basics.cpp -o m03_basics

#include <cassert>
#include <chrono>
#include <climits>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Split on a single delimiter, skipping empty tokens, without allocating any std::string.
// Each returned view points into `s`; the caller must keep `s` alive while using them.
std::vector<std::string_view> split(std::string_view s, char delim) {
    std::vector<std::string_view> out;
    std::size_t pos = 0;
    while (pos < s.size()) {
        std::size_t start = s.find_first_not_of(delim, pos);
        if (start == std::string_view::npos) break;
        std::size_t end = s.find(delim, start);
        if (end == std::string_view::npos) end = s.size();
        out.push_back(s.substr(start, end - start));   // O(1): a view, not a copy
        pos = end;
    }
    return out;
}

// The "atoi" interview problem: whitespace, sign, digits, clamp on overflow.
int my_atoi(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size() && s[i] == ' ') ++i;
    int sign = 1;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) sign = (s[i++] == '-') ? -1 : 1;
    int result = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        int digit = s[i++] - '0';
        // Check BEFORE multiplying: result*10 + digit > INT_MAX  <=>  result > (INT_MAX-digit)/10
        if (result > (INT_MAX - digit) / 10) return sign == 1 ? INT_MAX : INT_MIN;
        result = result * 10 + digit;
    }
    return sign * result;
}

double seconds_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

int main() {
    // ---- SSO: watch data() stay inside the object until the string outgrows the buffer ----
    {
        std::string s;
        const char* obj_begin = reinterpret_cast<const char*>(&s);
        const char* obj_end = obj_begin + sizeof(s);
        auto inside = [&](const std::string& str) {
            const char* d = str.data();
            return d >= obj_begin && d < obj_end;
        };
        s = "short";
        std::printf("\"%s\" (%zu chars): %s\n", s.c_str(), s.size(), inside(s) ? "inline (SSO)" : "heap");
        s = "exactly15chars!";
        std::printf("\"%s\" (%zu chars): %s\n", s.c_str(), s.size(), inside(s) ? "inline (SSO)" : "heap");
        s = "sixteen chars...";
        std::printf("\"%s\" (%zu chars): %s\n", s.c_str(), s.size(), inside(s) ? "inline (SSO)" : "heap");
        std::printf("sizeof(std::string)=%zu  sizeof(std::string_view)=%zu\n\n",
                    sizeof(std::string), sizeof(std::string_view));
    }

    // ---- string_view: zero-copy slicing ---------------------------------------------------
    {
        std::string line = "LEN=00120;TYPE=TXN;ACCT=1234567890123456;";
        auto fields = split(line, ';');
        for (std::string_view f : fields) {
            std::size_t eq = f.find('=');
            std::string_view key = f.substr(0, eq), value = f.substr(eq + 1);
            // %.*s prints exactly `len` bytes: string_view is NOT '\0'-terminated.
            std::printf("  key=%.*s value=%.*s\n", static_cast<int>(key.size()), key.data(),
                        static_cast<int>(value.size()), value.data());
        }
        assert(fields.size() == 3);
        assert(my_atoi(fields[0].substr(4)) == 120);
    }

    // ---- The dangling trap, shown and avoided ---------------------------------------------
    {
        std::string_view dangling;
        {
            std::string temp = "this string is long enough to live on the heap";
            dangling = temp;          // view into temp's heap buffer
        }                             // temp destroyed: `dangling` now points to freed memory
        // Using `dangling` here is undefined behaviour. We do not. The fix is to copy:
        std::string owned;
        {
            std::string temp = "this string is long enough to live on the heap";
            owned = std::string(std::string_view(temp).substr(0, 4));   // explicit copy
        }
        assert(owned == "this");
        (void)dangling;
    }

    // ---- my_atoi edge cases ---------------------------------------------------------------
    assert(my_atoi("42") == 42);
    assert(my_atoi("   -42") == -42);
    assert(my_atoi("4193 with words") == 4193);
    assert(my_atoi("words and 987") == 0);
    assert(my_atoi("-91283472332") == INT_MIN);
    assert(my_atoi("91283472332") == INT_MAX);

    // ---- quadratic vs linear string building ----------------------------------------------
    {
        const int n = 20000;
        auto t0 = std::chrono::steady_clock::now();
        std::string a;
        for (int i = 0; i < n; ++i) a = a + 'x';    // new string every iteration: O(n^2)
        double t_plus = seconds_since(t0);

        t0 = std::chrono::steady_clock::now();
        std::string b;
        for (int i = 0; i < n; ++i) b += 'x';       // in-place append: amortised O(1)
        double t_append = seconds_since(t0);

        assert(a == b);
        std::printf("\nbuild %d chars: s = s + c -> %.5f s, s += c -> %.5f s\n", n, t_plus, t_append);
    }
    std::printf("all string-basics checks passed\n");
    return 0;
}

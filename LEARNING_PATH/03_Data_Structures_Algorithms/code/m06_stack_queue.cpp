// m06_stack_queue.cpp
// Vector-backed stack, queue via two stacks (amortised O(1)), MinStack, valid parentheses,
// evaluate RPN. Each with a small check.
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread m06_stack_queue.cpp -o m06_sq

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

// A stack is a vector with a narrower interface. The narrowing is the point: a reader knows
// nobody indexes into the middle.
template <typename T>
class Stack {
public:
    void push(const T& x) { data_.push_back(x); }
    void pop() { data_.pop_back(); }          // precondition: !empty()
    T& top() { return data_.back(); }
    const T& top() const { return data_.back(); }
    bool empty() const { return data_.empty(); }
    std::size_t size() const { return data_.size(); }
private:
    std::vector<T> data_;
};

// Queue from two stacks. Elements move from `in` to `out` exactly once, so n operations do
// O(n) work in total even though a single pop may move many elements.
template <typename T>
class TwoStackQueue {
public:
    void push(const T& x) { in_.push_back(x); }
    T& front() { shift(); return out_.back(); }
    void pop() { shift(); out_.pop_back(); }
    bool empty() const { return in_.empty() && out_.empty(); }
    std::size_t size() const { return in_.size() + out_.size(); }
private:
    void shift() {
        if (!out_.empty()) return;
        while (!in_.empty()) { out_.push_back(in_.back()); in_.pop_back(); }   // reverses order
    }
    std::vector<T> in_, out_;
};

// MinStack: a second stack records the running minimum at the time of each push.
class MinStack {
public:
    void push(int x) {
        data_.push_back(x);
        mins_.push_back(mins_.empty() ? x : std::min(x, mins_.back()));
    }
    void pop() { data_.pop_back(); mins_.pop_back(); }
    int top() const { return data_.back(); }
    int get_min() const { return mins_.back(); }
private:
    std::vector<int> data_, mins_;
};

namespace {

bool valid_parens(std::string_view s) {
    std::vector<char> st;
    for (char c : s) {
        if (c == '(' || c == '[' || c == '{') { st.push_back(c); continue; }
        if (st.empty()) return false;              // closer with nothing open
        char open = st.back();
        st.pop_back();
        if ((c == ')' && open != '(') || (c == ']' && open != '[') || (c == '}' && open != '{'))
            return false;
    }
    return st.empty();                              // anything left open is an error
}

long long eval_rpn(const std::vector<std::string>& tokens) {
    std::vector<long long> st;
    for (const std::string& t : tokens) {
        if (t == "+" || t == "-" || t == "*" || t == "/") {
            long long b = st.back(); st.pop_back();   // right operand is on top
            long long a = st.back(); st.pop_back();
            long long r = 0;
            if (t == "+") r = a + b;
            else if (t == "-") r = a - b;
            else if (t == "*") r = a * b;
            else r = a / b;                            // C++ truncates toward zero, as the problem wants
            st.push_back(r);
        } else {
            st.push_back(std::stoll(t));
        }
    }
    return st.back();
}

}  // namespace

int main() {
    Stack<int> s;
    for (int i = 1; i <= 3; ++i) s.push(i);
    assert(s.top() == 3 && s.size() == 3);
    s.pop();
    assert(s.top() == 2);

    TwoStackQueue<int> q;
    for (int i = 1; i <= 3; ++i) q.push(i);
    assert(q.front() == 1);
    q.pop();
    q.push(4);                       // interleave pushes and pops: order must survive
    assert(q.front() == 2); q.pop();
    assert(q.front() == 3); q.pop();
    assert(q.front() == 4); q.pop();
    assert(q.empty());
    // 1M mixed operations must be fast: amortised O(1).
    for (int i = 0; i < 1'000'000; ++i) { q.push(i); if (i % 3 == 0) q.pop(); }
    std::printf("two-stack queue after 1M ops: size %zu\n", q.size());

    MinStack ms;
    ms.push(3); ms.push(5);
    assert(ms.get_min() == 3);
    ms.push(2); ms.push(1);
    assert(ms.get_min() == 1);
    ms.pop();
    assert(ms.get_min() == 2);
    ms.pop();
    assert(ms.get_min() == 3);

    assert(valid_parens("()[]{}"));
    assert(valid_parens("{[()]}"));
    assert(!valid_parens("(]"));
    assert(!valid_parens("(("));
    assert(!valid_parens("))"));
    assert(valid_parens(""));

    assert(eval_rpn({"2", "1", "+", "3", "*"}) == 9);
    assert(eval_rpn({"4", "13", "5", "/", "+"}) == 6);
    assert(eval_rpn({"10", "6", "9", "3", "+", "-11", "*", "/", "*", "17", "+", "5", "+"}) == 22);

    std::printf("all stack/queue checks passed\n");
    return 0;
}

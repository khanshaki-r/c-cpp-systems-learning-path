// m05_list_problems.cpp
// Interview-style linked list problems on a raw-pointer ListNode (the convention problem
// sites use): reverse (iterative + recursive), middle, Floyd cycle detection + cycle start,
// merge two sorted, remove nth from end, palindrome, reorder, remove duplicates from sorted.
// A helper builds and frees test lists so the program itself does not leak.
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread m05_list_problems.cpp -o m05_probs

#include <cassert>
#include <cstdio>
#include <initializer_list>
#include <vector>

struct ListNode {
    int val;
    ListNode* next;
    explicit ListNode(int v, ListNode* n = nullptr) : val(v), next(n) {}
};

namespace {

// ---- helpers ------------------------------------------------------------------------------
ListNode* build(std::initializer_list<int> vals) {
    ListNode dummy(0);
    ListNode* tail = &dummy;
    for (int v : vals) { tail->next = new ListNode(v); tail = tail->next; }
    return dummy.next;
}
void free_list(ListNode* head) {
    while (head) { ListNode* next = head->next; delete head; head = next; }
}
std::vector<int> to_vector(ListNode* head) {
    std::vector<int> out;
    for (; head; head = head->next) out.push_back(head->val);
    return out;
}

// ---- 1. reverse -------------------------------------------------------------------------
// Invariant: `prev` heads the already-reversed prefix, `cur` heads the untouched suffix.
ListNode* reverse_iterative(ListNode* head) {
    ListNode* prev = nullptr;
    while (head) {
        ListNode* next = head->next;   // save before overwriting
        head->next = prev;
        prev = head;
        head = next;
    }
    return prev;
}
// Recursive: reverse the tail, then hang `head` after what used to be its successor.
// O(n) stack: fine for interview lists, wrong for a million nodes.
ListNode* reverse_recursive(ListNode* head) {
    if (!head || !head->next) return head;
    ListNode* new_head = reverse_recursive(head->next);
    head->next->next = head;
    head->next = nullptr;
    return new_head;
}

// ---- 2. middle (second middle for even length) -------------------------------------------
ListNode* middle(ListNode* head) {
    ListNode* slow = head;
    ListNode* fast = head;
    while (fast && fast->next) { slow = slow->next; fast = fast->next->next; }
    return slow;
}

// ---- 3. Floyd: detect cycle, find its start ----------------------------------------------
ListNode* cycle_start(ListNode* head) {
    ListNode* slow = head;
    ListNode* fast = head;
    while (fast && fast->next) {
        slow = slow->next;
        fast = fast->next->next;
        if (slow == fast) {
            // Distance head->start equals distance meeting->start (mod cycle length), so two
            // pointers advancing by one from head and from the meeting point meet at the start.
            ListNode* p = head;
            while (p != slow) { p = p->next; slow = slow->next; }
            return p;
        }
    }
    return nullptr;
}

// ---- 4. merge two sorted lists --------------------------------------------------------------
ListNode* merge_sorted(ListNode* a, ListNode* b) {
    ListNode dummy(0);
    ListNode* tail = &dummy;
    while (a && b) {
        if (a->val <= b->val) { tail->next = a; a = a->next; }   // <= keeps it stable
        else                  { tail->next = b; b = b->next; }
        tail = tail->next;
    }
    tail->next = a ? a : b;   // attach whatever is left; no copying
    return dummy.next;
}

// ---- 5. remove nth from end -------------------------------------------------------------
ListNode* remove_nth_from_end(ListNode* head, int n) {
    ListNode dummy(0, head);
    ListNode* lead = &dummy;
    ListNode* trail = &dummy;
    for (int i = 0; i <= n; ++i) lead = lead->next;      // lead is n+1 ahead of trail
    while (lead) { lead = lead->next; trail = trail->next; }
    ListNode* dead = trail->next;                        // trail is just before the target
    trail->next = dead->next;
    delete dead;
    return dummy.next;                                   // works even when the head was removed
}

// ---- 6. palindrome in O(1) space ------------------------------------------------------------
bool is_palindrome(ListNode* head) {
    if (!head) return true;
    // find the end of the first half
    ListNode* slow = head;
    ListNode* fast = head;
    while (fast->next && fast->next->next) { slow = slow->next; fast = fast->next->next; }
    ListNode* second = reverse_iterative(slow->next);    // reverse second half in place
    bool ok = true;
    for (ListNode* a = head, *b = second; b; a = a->next, b = b->next)
        if (a->val != b->val) { ok = false; break; }
    slow->next = reverse_iterative(second);              // restore the list
    return ok;
}

// ---- 7. reorder L0 Ln L1 Ln-1 ... ----------------------------------------------------------
void reorder(ListNode* head) {
    if (!head || !head->next) return;
    ListNode* slow = head;
    ListNode* fast = head;
    while (fast->next && fast->next->next) { slow = slow->next; fast = fast->next->next; }
    ListNode* second = reverse_iterative(slow->next);
    slow->next = nullptr;                                // cut the first half
    ListNode* first = head;
    while (second) {                                     // interleave
        ListNode* n1 = first->next;
        ListNode* n2 = second->next;
        first->next = second;
        second->next = n1;
        first = n1;
        second = n2;
    }
}

// ---- 8. remove duplicates from a sorted list -----------------------------------------------
ListNode* dedupe_sorted(ListNode* head) {
    for (ListNode* p = head; p && p->next;) {
        if (p->val == p->next->val) { ListNode* dead = p->next; p->next = dead->next; delete dead; }
        else p = p->next;
    }
    return head;
}

}  // namespace

int main() {
    {
        ListNode* l = build({1, 2, 3, 4, 5});
        l = reverse_iterative(l);
        assert((to_vector(l) == std::vector<int>{5, 4, 3, 2, 1}));
        l = reverse_recursive(l);
        assert((to_vector(l) == std::vector<int>{1, 2, 3, 4, 5}));
        assert(middle(l)->val == 3);
        free_list(l);
        l = build({1, 2, 3, 4});
        assert(middle(l)->val == 3);   // second middle
        free_list(l);
    }
    {
        // 1 -> 2 -> 3 -> 4 -> 5 -> back to 3
        ListNode* l = build({1, 2, 3, 4, 5});
        ListNode* n3 = l->next->next;
        ListNode* n5 = n3->next->next;
        n5->next = n3;
        ListNode* start = cycle_start(l);
        assert(start == n3 && start->val == 3);
        n5->next = nullptr;   // break the cycle before freeing
        free_list(l);
        l = build({1, 2});
        assert(cycle_start(l) == nullptr);
        free_list(l);
    }
    {
        ListNode* a = build({1, 2, 4});
        ListNode* b = build({1, 3, 4});
        ListNode* m = merge_sorted(a, b);
        assert((to_vector(m) == std::vector<int>{1, 1, 2, 3, 4, 4}));
        free_list(m);   // all nodes of a and b now live in m
    }
    {
        ListNode* l = build({1, 2, 3, 4, 5});
        l = remove_nth_from_end(l, 2);
        assert((to_vector(l) == std::vector<int>{1, 2, 3, 5}));
        l = remove_nth_from_end(l, 4);   // removes the head
        assert((to_vector(l) == std::vector<int>{2, 3, 5}));
        free_list(l);
    }
    {
        ListNode* l = build({1, 2, 2, 1});
        assert(is_palindrome(l));
        assert((to_vector(l) == std::vector<int>{1, 2, 2, 1}));   // restored
        free_list(l);
        l = build({1, 2});
        assert(!is_palindrome(l));
        free_list(l);
    }
    {
        ListNode* l = build({1, 2, 3, 4, 5});
        reorder(l);
        assert((to_vector(l) == std::vector<int>{1, 5, 2, 4, 3}));
        free_list(l);
    }
    {
        ListNode* l = build({1, 1, 2, 3, 3, 3});
        l = dedupe_sorted(l);
        assert((to_vector(l) == std::vector<int>{1, 2, 3}));
        free_list(l);
    }
    std::printf("all linked-list problem checks passed\n");
    return 0;
}

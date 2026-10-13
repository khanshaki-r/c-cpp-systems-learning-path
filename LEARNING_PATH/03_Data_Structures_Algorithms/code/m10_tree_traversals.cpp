// m10_tree_traversals.cpp
// Binary tree problems on the raw-pointer TreeNode used by problem sites: recursive and
// iterative pre/in/post-order, level order by levels, Morris in-order (O(1) space), max depth,
// diameter, invert, same tree, symmetric, path sum, LCA in a general binary tree, build from
// level-order array with nulls. The test trees are freed at the end.
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread m10_tree_traversals.cpp -o m10_trav

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <deque>
#include <optional>
#include <vector>

struct TreeNode {
    int val;
    TreeNode* left = nullptr;
    TreeNode* right = nullptr;
    explicit TreeNode(int v) : val(v) {}
};

namespace {

// Build from a level-order list where nullopt is a missing child (LeetCode's format).
TreeNode* build(const std::vector<std::optional<int>>& a) {
    if (a.empty() || !a[0]) return nullptr;
    TreeNode* root = new TreeNode(*a[0]);
    std::deque<TreeNode*> q{root};
    std::size_t i = 1;
    while (!q.empty() && i < a.size()) {
        TreeNode* n = q.front();
        q.pop_front();
        if (i < a.size() && a[i]) { n->left = new TreeNode(*a[i]); q.push_back(n->left); }
        ++i;
        if (i < a.size() && a[i]) { n->right = new TreeNode(*a[i]); q.push_back(n->right); }
        ++i;
    }
    return root;
}
void free_tree(TreeNode* n) {               // post-order: children before parent
    if (!n) return;
    free_tree(n->left);
    free_tree(n->right);
    delete n;
}

// ---- recursive traversals ------------------------------------------------------------------
void preorder(TreeNode* n, std::vector<int>& out) {
    if (!n) return;
    out.push_back(n->val); preorder(n->left, out); preorder(n->right, out);
}
void inorder(TreeNode* n, std::vector<int>& out) {
    if (!n) return;
    inorder(n->left, out); out.push_back(n->val); inorder(n->right, out);
}
void postorder(TreeNode* n, std::vector<int>& out) {
    if (!n) return;
    postorder(n->left, out); postorder(n->right, out); out.push_back(n->val);
}

// ---- iterative traversals ------------------------------------------------------------------
std::vector<int> preorder_iter(TreeNode* root) {
    std::vector<int> out;
    std::vector<TreeNode*> st;
    if (root) st.push_back(root);
    while (!st.empty()) {
        TreeNode* n = st.back();
        st.pop_back();
        out.push_back(n->val);
        if (n->right) st.push_back(n->right);   // right first so left is popped first
        if (n->left) st.push_back(n->left);
    }
    return out;
}
std::vector<int> inorder_iter(TreeNode* root) {
    std::vector<int> out;
    std::vector<TreeNode*> st;
    TreeNode* cur = root;
    while (cur || !st.empty()) {
        while (cur) { st.push_back(cur); cur = cur->left; }   // descend the left spine
        cur = st.back();
        st.pop_back();
        out.push_back(cur->val);
        cur = cur->right;
    }
    return out;
}
// Post-order = reverse of (node, right, left), which is pre-order with the children swapped.
std::vector<int> postorder_iter(TreeNode* root) {
    std::vector<int> out;
    std::vector<TreeNode*> st;
    if (root) st.push_back(root);
    while (!st.empty()) {
        TreeNode* n = st.back();
        st.pop_back();
        out.push_back(n->val);
        if (n->left) st.push_back(n->left);
        if (n->right) st.push_back(n->right);
    }
    std::reverse(out.begin(), out.end());
    return out;
}
std::vector<std::vector<int>> level_order(TreeNode* root) {
    std::vector<std::vector<int>> out;
    std::deque<TreeNode*> q;
    if (root) q.push_back(root);
    while (!q.empty()) {
        std::size_t width = q.size();             // everything currently queued is one level
        out.emplace_back();
        for (std::size_t i = 0; i < width; ++i) {
            TreeNode* n = q.front();
            q.pop_front();
            out.back().push_back(n->val);
            if (n->left) q.push_back(n->left);
            if (n->right) q.push_back(n->right);
        }
    }
    return out;
}

// Morris in-order: O(1) extra space by threading each left subtree's rightmost node back to
// the current node, then removing the thread when we return. The tree is restored.
std::vector<int> inorder_morris(TreeNode* root) {
    std::vector<int> out;
    TreeNode* cur = root;
    while (cur) {
        if (!cur->left) {
            out.push_back(cur->val);
            cur = cur->right;
            continue;
        }
        TreeNode* pred = cur->left;                          // in-order predecessor of cur
        while (pred->right && pred->right != cur) pred = pred->right;
        if (!pred->right) {
            pred->right = cur;                               // create thread, go left
            cur = cur->left;
        } else {
            pred->right = nullptr;                           // thread exists: we came back
            out.push_back(cur->val);
            cur = cur->right;
        }
    }
    return out;
}

// ---- one-pass recursions ------------------------------------------------------------------
int max_depth(TreeNode* n) { return n ? 1 + std::max(max_depth(n->left), max_depth(n->right)) : 0; }

// Diameter in edges: max over nodes of left_height + right_height. Height returned, best updated.
int height_for_diameter(TreeNode* n, int& best) {
    if (!n) return 0;
    int l = height_for_diameter(n->left, best), r = height_for_diameter(n->right, best);
    best = std::max(best, l + r);
    return 1 + std::max(l, r);
}
int diameter(TreeNode* root) { int best = 0; height_for_diameter(root, best); return best; }

TreeNode* invert(TreeNode* n) {
    if (n) { std::swap(n->left, n->right); invert(n->left); invert(n->right); }
    return n;
}
bool same_tree(TreeNode* a, TreeNode* b) {
    if (!a || !b) return a == b;
    return a->val == b->val && same_tree(a->left, b->left) && same_tree(a->right, b->right);
}
bool mirror(TreeNode* a, TreeNode* b) {
    if (!a || !b) return a == b;
    return a->val == b->val && mirror(a->left, b->right) && mirror(a->right, b->left);
}
bool symmetric(TreeNode* root) { return !root || mirror(root->left, root->right); }

// Root-to-leaf path with the given sum exists?
bool has_path_sum(TreeNode* n, int target) {
    if (!n) return false;
    if (!n->left && !n->right) return n->val == target;
    return has_path_sum(n->left, target - n->val) || has_path_sum(n->right, target - n->val);
}

// LCA in a general binary tree (p and q present): the first node that has one target in each
// side, or is itself a target with the other below.
TreeNode* lca(TreeNode* n, TreeNode* p, TreeNode* q) {
    if (!n || n == p || n == q) return n;
    TreeNode* l = lca(n->left, p, q);
    TreeNode* r = lca(n->right, p, q);
    if (l && r) return n;
    return l ? l : r;
}

void print(const char* label, const std::vector<int>& v) {
    std::printf("%-16s", label);
    for (int x : v) std::printf("%d ", x);
    std::printf("\n");
}

}  // namespace

int main() {
    using V = std::vector<int>;
    //        1
    //      /   \  .
    //     2     3
    //    / \     \  .
    //   4   5     6
    TreeNode* t = build({1, 2, 3, 4, 5, std::nullopt, 6});

    V pre, in, post;
    preorder(t, pre); inorder(t, in); postorder(t, post);
    print("preorder", pre); print("inorder", in); print("postorder", post);
    assert((pre == V{1, 2, 4, 5, 3, 6}) && (in == V{4, 2, 5, 1, 3, 6}) && (post == V{4, 5, 2, 6, 3, 1}));
    assert(preorder_iter(t) == pre && inorder_iter(t) == in && postorder_iter(t) == post);
    assert(inorder_morris(t) == in && inorder_iter(t) == in);   // Morris restored the tree

    auto levels = level_order(t);
    assert(levels.size() == 3 && (levels[2] == V{4, 5, 6}));

    assert(max_depth(t) == 3 && diameter(t) == 4);            // 4-2-1-3-6
    assert(has_path_sum(t, 8) && has_path_sum(t, 10) && !has_path_sum(t, 9));
    assert(symmetric(build({})) && !symmetric(t));

    TreeNode* n4 = t->left->left;
    TreeNode* n5 = t->left->right;
    TreeNode* n6 = t->right->right;
    assert(lca(t, n4, n5) == t->left && lca(t, n4, n6) == t && lca(t, n4, t->left) == t->left);

    TreeNode* copy = build({1, 2, 3, 4, 5, std::nullopt, 6});
    assert(same_tree(t, copy));
    invert(copy);
    assert(!same_tree(t, copy));
    V inv;
    inorder(copy, inv);
    assert((inv == V{6, 3, 1, 5, 2, 4}));
    invert(copy);
    assert(same_tree(t, copy));

    TreeNode* sym = build({1, 2, 2, 3, 4, 4, 3});
    assert(symmetric(sym));

    free_tree(t); free_tree(copy); free_tree(sym);
    std::printf("all traversal checks passed\n");
    return 0;
}

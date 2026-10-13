# 10. Trees and binary search trees

**Time:** ~4 hours   **Prerequisites:** 05, 07, 09

## 1. In plain words

A tree is a linked structure where each node points to children instead of to one "next". A
file system is a tree: directories contain directories and files. An organisation chart is a
tree. The expression `(a + b) * c` is a tree with `*` at the top. Trees are how you represent
anything hierarchical, and how you search ordered data while still inserting quickly.

A **binary search tree** (BST) is a binary tree (at most two children) with one rule: every key
in a node's left subtree is smaller than the node's key, every key in its right subtree is
larger. That rule makes search a walk: compare, go left or right, repeat. If the tree is
"bushy" (balanced), the walk is log2(n) steps — the same as binary search — but inserting a
new key is also just a walk plus one pointer write, with no shifting. Your sorted array could
search in O(log n) but had to shift on insert; a linked list could insert in O(1) but had to
scan to search. The BST does both in O(log n), *if* it stays balanced.

The "if" is the whole story of modules 10 and 11. A BST built from keys arriving in sorted
order is a linked list in disguise: every node has only a right child, height n, O(n) search.
Module 11 fixes that with self-balancing trees and then with the B-tree, which is what disks
and databases need.

## 2. The real thing

### 2.1 Terminology

- **Root**: the top node. **Leaf**: a node with no children. **Internal node**: has children.
- **Parent / child / sibling / ancestor / descendant**: as in a family tree.
- **Depth** of a node: edges from the root to it. **Height** of a tree: max depth (a single
  node has height 0; the empty tree is -1 by convention, or 0 if you count nodes).
- **Subtree** rooted at `x`: `x` and all its descendants.
- **Binary tree**: each node has at most two children, called left and right.
- **Full**: every node has 0 or 2 children. **Complete**: all levels full except possibly the
  last, which is filled left to right (this is the heap shape, module 12). **Perfect**: all
  levels full; n = 2^(h+1) - 1. **Balanced**: height is O(log n) — different structures use
  different exact definitions (AVL: child heights differ by at most 1).
- **Degenerate**: every node has one child; effectively a linked list.
- **BST property**: for every node, all keys in the left subtree < key < all keys in the right
  subtree. Note "all keys in the subtree", not just the immediate child — that distinction is
  the bug in most "validate BST" attempts.

### 2.2 Traversals

Visiting every node exactly once, in some order. Four orders matter:

| Traversal | Order | Use |
|---|---|---|
| **Pre-order** | node, left, right | copy / serialise a tree; prefix expression |
| **In-order** | left, node, right | BST: yields keys in sorted order |
| **Post-order** | left, right, node | delete a tree (children before parent); evaluate expression; compute heights |
| **Level-order** (BFS) | by depth, left to right | shortest path in unweighted tree; printing by level; serialising with structure |

The first three are depth-first (DFS) and are naturally recursive. Each is O(n) time, O(h)
space for the recursion stack (h = height; O(n) worst case on a degenerate tree).

Iterative in-order uses an explicit stack: push all left descendants, pop and visit, move to
the right child, repeat. Iterative pre-order: push root; pop, visit, push right then left (so
left is processed first). Iterative post-order is awkward; the trick is "pre-order with
children pushed left-then-right, then reverse the output". Level-order uses a queue; to get
levels as separate lists, process the queue in batches of its current size.

Morris traversal does in-order in O(1) extra space by temporarily rewiring right pointers
(threading). Know it exists; it comes up as a "can you do it without a stack" follow-up.

### 2.3 BST operations

**Search**: `while (node && node->key != k) node = k < node->key ? left : right`. O(h).

**Insert**: search for the key; when you fall off the tree, attach the new node there. Keys
always become leaves. O(h). Duplicates: reject, count, or put them consistently to one side.

**Minimum / maximum**: leftmost / rightmost node. O(h).

**Successor** (next key in sorted order): if the node has a right subtree, the minimum of it;
else the nearest ancestor of which the node is in the left subtree. This is how a cursor moves
to "next key" in an ordered tree (and why `std::map::iterator::operator++` is amortised O(1)
but O(h) worst case).

**Delete** — three cases:
1. Leaf: unlink it.
2. One child: replace the node with its child.
3. Two children: find the in-order successor (minimum of the right subtree, which has no left
   child), copy its key into this node, then delete the successor from the right subtree
   (which is case 1 or 2). Using the predecessor (max of left subtree) is symmetric. O(h).

With `std::unique_ptr<Node>` the "replace node with child" step is
`*link = std::move((*link)->right)` where `link` is the owning pointer that leads to the node —
the same pointer-to-owner walk as the linked list in module 05.

**Height**: `1 + max(height(left), height(right))`, post-order. **Size**: `1 + size(left) +
size(right)`. **Balanced check**: compute heights bottom-up and return -1 as soon as a node's
children differ by more than 1 — O(n) in one pass instead of O(n^2) with a naive
height-at-every-node.

### 2.4 Classic problems

**Validate BST.** Wrong: check `left->key < key < right->key` at each node. That accepts
`[5, [1, null, 6], 8]` where 6 is in 5's left subtree. Right: pass down a `(lo, hi)` range each
node's key must lie in; the left child gets `(lo, key)`, the right gets `(key, hi)`. Or do an
in-order traversal and check it is strictly increasing. O(n).

**Lowest common ancestor (LCA).** In a BST: walk from the root; if both keys are smaller go
left, if both larger go right, else the current node is the LCA. O(h). In a general binary
tree: recurse; if a subtree returns both targets found (or one found below each child), the
current node is the LCA. O(n).

**Serialise / deserialise.** Pre-order with explicit null markers (`"5 3 # # 8 # #"`)
reconstructs uniquely. For a BST you can skip the null markers: pre-order plus the BST
property suffices (rebuild with the `(lo, hi)` range technique). Level-order with nulls is
what LeetCode uses for its examples.

**k-th smallest.** In-order traversal, stop at the k-th visit. O(h + k). With a `size` field in
each node it is O(h) — this is an **order-statistic tree**.

**Build from sorted array.** Middle element is the root; recurse on halves. Produces a
perfectly balanced BST in O(n). This is also how a B-tree is bulk-loaded from sorted data
(module 11).

**Diameter.** Longest path between any two nodes = max over nodes of (height(left) +
height(right) + 2). One post-order pass. **Path sum**, **invert**, **symmetric**, **same tree**,
**max depth**, **count nodes** — all one recursive pass each; they are the warm-up problems
interviewers use to check you can write a recursion cleanly.

### 2.5 Why an unbalanced BST is bad

Insert 1, 2, 3, ..., n in order: each key is larger than everything so far, goes to the far
right. Height n - 1. Search is O(n). Insert is O(n). You have built a linked list with extra
pointers. Random insertion order gives expected height ~ 2.99 log2(n) — fine on average, but
"random" is not something you can rely on in production: timestamps, sequence numbers and
sorted batch loads are the common case and they are the worst case. That is why no production
system uses a plain BST; `std::map` is a red-black tree, databases use B-trees, and both
guarantee O(log n) by rebalancing.

## 3. Code

Files: `code/m10_bst.cpp` (BST with `unique_ptr`: insert, search, min/max, delete with all
three cases, height, size, in-order to vector, validate, LCA, k-th smallest, build from sorted
array, serialise/deserialise pre-order with null markers; an experiment that inserts sorted vs
shuffled keys and prints the height), `code/m10_tree_traversals.cpp` (recursive and iterative
pre/in/post-order, level-order with levels, Morris in-order, max depth, diameter, invert,
same-tree, path sum, LCA in a general binary tree, on a raw-pointer `TreeNode` as problem
sites define it).

Key parts of `m10_bst.cpp`:

```cpp
struct Node {
    int key;
    std::unique_ptr<Node> left, right;
};

// Owning-pointer walk: `link` is the unique_ptr that leads to the current node. Insert
// attaches at the first null link; delete rewires through the same link.
void insert(int key) {
    std::unique_ptr<Node>* link = &root_;
    while (*link) {
        if (key == (*link)->key) return;                 // no duplicates
        link = key < (*link)->key ? &(*link)->left : &(*link)->right;
    }
    *link = std::make_unique<Node>(Node{key, nullptr, nullptr});
    ++size_;
}

bool erase(int key) {
    std::unique_ptr<Node>* link = &root_;
    while (*link && (*link)->key != key)
        link = key < (*link)->key ? &(*link)->left : &(*link)->right;
    if (!*link) return false;
    Node* n = link->get();
    if (n->left && n->right) {
        // Two children: find the successor (leftmost of right subtree), steal its key,
        // then delete the successor node, which has at most one (right) child.
        std::unique_ptr<Node>* succ = &n->right;
        while ((*succ)->left) succ = &(*succ)->left;
        n->key = (*succ)->key;
        *succ = std::move((*succ)->right);
    } else {
        // Zero or one child: replace the node by its only child (or null).
        *link = std::move(n->left ? n->left : n->right);
    }
    --size_;
    return true;
}
```

```cpp
// Validate with ranges. Using long long bounds avoids the INT_MIN/INT_MAX edge cases.
bool valid(const Node* n, long long lo, long long hi) {
    if (!n) return true;
    if (n->key <= lo || n->key >= hi) return false;
    return valid(n->left.get(), lo, n->key) && valid(n->right.get(), n->key, hi);
}
```

```cpp
// Height and balance in one post-order pass: -1 means "unbalanced somewhere below".
int balanced_height(const Node* n) {
    if (!n) return 0;
    int l = balanced_height(n->left.get());  if (l < 0) return -1;
    int r = balanced_height(n->right.get()); if (r < 0) return -1;
    if (std::abs(l - r) > 1) return -1;
    return 1 + std::max(l, r);
}
```

Key parts of `m10_tree_traversals.cpp`:

```cpp
// Iterative in-order: push the left spine, pop-visit, go right.
std::vector<int> inorder_iter(TreeNode* root) {
    std::vector<int> out; std::vector<TreeNode*> st;
    TreeNode* cur = root;
    while (cur || !st.empty()) {
        while (cur) { st.push_back(cur); cur = cur->left; }
        cur = st.back(); st.pop_back();
        out.push_back(cur->val);
        cur = cur->right;
    }
    return out;
}
// Level order, one vector per level: process the queue in batches of its current size.
std::vector<std::vector<int>> level_order(TreeNode* root) {
    std::vector<std::vector<int>> out;
    std::deque<TreeNode*> q;
    if (root) q.push_back(root);
    while (!q.empty()) {
        std::size_t width = q.size();
        out.emplace_back();
        for (std::size_t i = 0; i < width; ++i) {
            TreeNode* n = q.front(); q.pop_front();
            out.back().push_back(n->val);
            if (n->left) q.push_back(n->left);
            if (n->right) q.push_back(n->right);
        }
    }
    return out;
}
```

## 4. Under the hood (senior-level)

### 4.1 Node layout and cache behaviour

A BST node with an `int` key and two `unique_ptr` children is 24 bytes (4 + 4 padding + 8 + 8);
`std::map<int,int>`'s red-black node is 48 bytes (colour + 3 pointers + key + value + padding).
Each node is a separate allocation, so a search of depth 20 touches 20 cache lines at
unpredictable addresses: ~20 cache misses, ~2 us on a cold cache for a 1M-entry map. Compare
binary search on a sorted vector: the first 10 probes are shared by all queries and stay
cached; ~10 misses. Compare a B-tree node of 4 KB holding 500 keys: 3 misses to reach the
leaf (plus the binary search within each node, which is in-cache). This is the memory-hierarchy
argument for B-trees even in RAM, and it is why `std::map` is rarely the fastest choice.

### 4.2 Recursion depth and the degenerate tree

Recursive traversal of a degenerate tree with 10^6 nodes recurses 10^6 deep: stack overflow.
Any production tree code must either guarantee balance (then depth is ~2 log2 n ~ 40) or use
iterative traversal. Deleting a `unique_ptr`-based tree is recursive by default (the
destructor chain) — same hazard as the linked list in module 05. The code file deletes
iteratively with a post-order stack.

### 4.3 Expected height of a random BST

Inserting n distinct keys in uniformly random order gives expected height about 4.311 ln n ~
2.99 log2 n, and expected average node depth about 1.39 log2 n (the same 1.39 as quicksort's
comparison count — the BST built by insertion order and the recursion tree of quicksort on
that order are the same tree). "Random order" is a model; real key streams are sorted far
more often than random, so you rebalance.

### 4.4 Threaded trees and parent pointers

`std::map` nodes store a parent pointer so `++iterator` can find the successor without a
stack: go to the right subtree's minimum, or climb until you come from a left child. Across a
full in-order traversal every edge is crossed exactly twice, so `n` increments cost O(n)
total: amortised O(1) per step, O(h) worst case for a single step. A cursor in a B-tree leaf
chain (module 11) does better: next key is `pos + 1` in the same page, or follow one sibling
pointer.

### 4.5 Trees in the systems you know

PostgreSQL's planner builds an expression tree for every `WHERE` clause and evaluates it by
post-order walk (children first). The query plan itself is a tree of plan nodes executed by
pulling tuples from the root (Volcano model). Your RPC server's message schema is a tree.
JSON, XML and ASN.1 parsers produce trees. Directory walkers are pre-order DFS. Compilers'
ASTs are trees walked in post-order to generate code. When you say "tree traversal" in an
interview, having two of these examples ready makes the answer concrete.

## 5. In production

- **Never ship a plain BST.** Use `std::map`/`std::set` (red-black), a sorted vector, or a
  B-tree library. The plain BST is a teaching tool and an interview topic.
- **Iterative deletes and traversals** for anything that might be deep. A recursive destructor
  on a degenerate tree is a crash at shutdown — the worst time, because it hides real errors
  and corrupts the exit code.
- **Validate on load.** If you persist a tree (or an index) and read it back, validate the
  ordering invariant with an in-order walk before trusting it. PostgreSQL's `amcheck`
  extension does exactly this for B-tree indexes (`bt_index_check`) and finds corrupted
  indexes that would otherwise return wrong query results silently.
- **Expression trees in rules engines.** Fee and limit rules in banking systems are often
  expression trees evaluated per transaction. Post-order evaluation with short-circuit `AND`/`OR`
  is the standard; a stack machine (module 06) is the optimised version.
- **Debug story (typical).** A cache keyed by timestamp used a hand-written BST "because it was
  simple". Keys arrived in increasing order. After a week the tree had height ~500 000 and
  each lookup took milliseconds; a thread pool of 32 was saturated doing lookups. Replacing it
  with `std::map` (same interface, O(log n) guaranteed) fixed it in one line. The lesson is
  not "use std::map"; it is "sorted input is the normal case, and a plain BST is O(n) on it".

## 6. Try it

1. **(easy)** Write recursive pre-, in- and post-order on the `TreeNode` type and run them on
   the tree `[1, 2, 3, 4, 5, null, 6]`. Done when the outputs are `1 2 4 5 3 6`, `4 2 5 1 3 6`,
   `4 5 2 6 3 1`.
2. **(easy)** Maximum depth, count of nodes, count of leaves, sum of all values — each as a
   one-function recursion. Done when all four work on an empty tree too.
3. **(medium)** Delete from a BST handling all three cases; after each delete, verify the
   in-order sequence is still sorted and has one fewer element. Done when 10 000 random
   inserts followed by 5000 random deletes leave a valid BST (use the validator).
4. **(medium)** Serialise a binary tree in pre-order with `#` for null, deserialise it, and
   check the round trip reproduces the same structure (compare with `same_tree`). Done when a
   tree with 1000 random nodes round-trips.
5. **(hard)** k-th smallest in a BST in O(h) per query by storing subtree sizes in each node
   and maintaining them on insert and delete. Done when 1000 random queries agree with the
   in-order list.
6. **(hard)** Measure it: insert 1, 2, ..., 100 000 into your BST in sorted order and time 1000
   searches; then insert the same keys shuffled and time again; then do both with `std::map`.
   Done when you have four numbers and can explain each (expect the sorted BST to be ~1000x
   slower than the others, and `std::map` to be the same for both orders).

## 7. Interview questions

**Q: What makes a binary tree a BST?**
For every node, all keys in its left subtree are less than its key and all keys in its right
subtree are greater. "All keys in the subtree", not only the direct children.

**Q: In-order traversal of a BST gives what? Why?**
The keys in sorted order: everything smaller (left subtree) is visited before the node,
everything larger (right subtree) after, recursively.

**Q: How do you delete a node with two children?**
Replace its key with its in-order successor's key (the minimum of the right subtree), then
delete that successor node, which has at most one child. The predecessor works symmetrically.

**Q: How do you validate a BST correctly?**
Pass an allowed range `(lo, hi)` down: the left child must be in `(lo, key)`, the right in
`(key, hi)`. Checking only immediate children misses violations deeper in the subtree.
Alternatively check that an in-order traversal is strictly increasing.

**Q: What is the time complexity of BST search, and what is the catch?**
O(h) where h is the height. Balanced: O(log n). Degenerate (sorted insertion): O(n). Plain
BSTs do not control h, which is why real systems use self-balancing trees or B-trees.

**Q: How do you find the LCA of two nodes?**
BST: walk from the root; go left if both keys are smaller, right if both larger, otherwise the
current node is the LCA. General tree: post-order recursion returning which targets were found
below; the first node that sees both is the LCA.

**Q: How would you traverse a tree without recursion?**
With an explicit stack for DFS (push left spine for in-order; push right then left for
pre-order) or a queue for BFS. Needed when the tree may be deep enough to overflow the call
stack.

**Q: Why does `std::map` iterate in O(n) total if each `++` can be O(log n)?**
Each tree edge is traversed exactly twice over a full in-order walk (down and back up), so n
increments do O(n) work in total — amortised O(1) per step even though one step can climb
O(h).

## 8. Recap

- Tree terms: root, leaf, depth, height, balanced, complete, degenerate.
- Pre-order (serialise), in-order (sorted for BST), post-order (delete, heights), level-order (BFS).
- DFS is O(n) time, O(h) stack; iterative versions use an explicit stack; level-order uses a queue.
- BST search/insert/min/max/successor are O(h); delete has three cases, two-children uses the successor.
- Validate with `(lo, hi)` ranges or an in-order strictness check — never with child-only comparisons.
- LCA in a BST is a root-to-split walk; in a general tree a post-order recursion.
- Height and balance in one post-order pass with a -1 sentinel.
- Sorted insertion makes a plain BST a linked list: O(n). Random order gives ~3 log2 n height.
- Each tree level is a cache miss; B-trees and sorted vectors touch fewer lines than `std::map`.
- Never ship a plain BST; use `std::map`, a sorted vector, or a B-tree; traverse and destroy
  iteratively when depth is not guaranteed.

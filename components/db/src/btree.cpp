// btree.cpp — B+ tree over fixed 512 B nodes from mem::Pool.
//
// Insert: descend (remembering the path), insert into the leaf, and if it
// overflows split it and push a separator into the parent, repeating upwards;
// a split root grows the tree by one level.
// Erase: remove from the leaf; if a node drops below 25% full, fix it with a
// sibling under the same parent (merge if the result is <= 75% full,
// otherwise even out the two), which may leave the parent short, and so on
// upwards; a root with a single child is replaced by that child.

#include <cstring>
#include <new>
#include "btree.hpp"

namespace db {
namespace bt {

struct Node {
    uint16_t count;  // leaf: entries; inner: keys (children = count + 1)
    uint8_t is_leaf;
    uint8_t in_sram;  // inner nodes: which pool to return it to
    uint32_t reserved;  // pads the header to 8 B so keys[] stays 8-aligned
};

// Leaf: 8 B header + next (padded to 16 B) + 41 * (8 B key + 4 B ref) = 508 B.
constexpr uint16_t LEAF_CAP = (NODE_SIZE - 16) / (sizeof(Key) + sizeof(ValueRef));
// Inner: 8 B header + 41 * 8 B keys + 42 * 4 B pointers = 504 B (on a 32-bit MCU).
constexpr uint16_t INNER_CAP = (NODE_SIZE - sizeof(Node) - sizeof(void *)) / (sizeof(Key) + sizeof(void *));

constexpr uint16_t LEAF_MIN = LEAF_CAP / 4;             // below this: rebalance
constexpr uint16_t INNER_MIN = INNER_CAP / 4;
constexpr uint16_t LEAF_MERGE_MAX = LEAF_CAP * 3 / 4;   // merge only if result fits this
constexpr uint16_t INNER_MERGE_MAX = INNER_CAP * 3 / 4;
constexpr int MAX_HEIGHT = 16;  // ~11^15 keys even with quarter-full nodes

struct Leaf : Node {
    Leaf *next;
    Key keys[LEAF_CAP];
    ValueRef refs[LEAF_CAP];
};

struct Inner : Node {
    Key keys[INNER_CAP];
    Node *child[INNER_CAP + 1];
};

static_assert(sizeof(Leaf) <= NODE_SIZE && sizeof(Inner) <= NODE_SIZE, "node must fit NODE_SIZE");

}  // namespace bt

using bt::Inner;
using bt::Leaf;
using bt::Node;
using bt::INNER_CAP;
using bt::LEAF_CAP;

static Leaf *as_leaf(Node *n) { return static_cast<Leaf *>(n); }
static Inner *as_inner(Node *n) { return static_cast<Inner *>(n); }
static const Leaf *as_leaf(const Node *n) { return static_cast<const Leaf *>(n); }
static const Inner *as_inner(const Node *n) { return static_cast<const Inner *>(n); }

// First index with keys[i] >= key.
static uint16_t lower_bound(const Key *keys, uint16_t n, Key key)
{
    uint16_t lo = 0, hi = n;
    while (lo < hi) {
        uint16_t mid = (lo + hi) / 2;
        if (keys[mid] < key) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

// First index with keys[i] > key == the child to follow in an inner node.
static uint16_t upper_bound(const Key *keys, uint16_t n, Key key)
{
    uint16_t lo = 0, hi = n;
    while (lo < hi) {
        uint16_t mid = (lo + hi) / 2;
        if (keys[mid] <= key) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

// ---- leaf / inner array helpers -------------------------------------------

static void leaf_insert_at(Leaf *l, uint16_t i, Key key, ValueRef ref)
{
    memmove(&l->keys[i + 1], &l->keys[i], (l->count - i) * sizeof(Key));
    memmove(&l->refs[i + 1], &l->refs[i], (l->count - i) * sizeof(ValueRef));
    l->keys[i] = key;
    l->refs[i] = ref;
    ++l->count;
}

static void leaf_remove_at(Leaf *l, uint16_t i)
{
    memmove(&l->keys[i], &l->keys[i + 1], (l->count - i - 1) * sizeof(Key));
    memmove(&l->refs[i], &l->refs[i + 1], (l->count - i - 1) * sizeof(ValueRef));
    --l->count;
}

// Moves entries [from, src->count) to the end of dst.
static void leaf_move_tail(Leaf *src, uint16_t from, Leaf *dst)
{
    uint16_t n = src->count - from;
    memcpy(&dst->keys[dst->count], &src->keys[from], n * sizeof(Key));
    memcpy(&dst->refs[dst->count], &src->refs[from], n * sizeof(ValueRef));
    dst->count += n;
    src->count = from;
}

// Inserts separator keys[i] with its right-hand child at child[i + 1].
static void inner_insert_at(Inner *n, uint16_t i, Key key, Node *right)
{
    memmove(&n->keys[i + 1], &n->keys[i], (n->count - i) * sizeof(Key));
    memmove(&n->child[i + 2], &n->child[i + 1], (n->count - i) * sizeof(Node *));
    n->keys[i] = key;
    n->child[i + 1] = right;
    ++n->count;
}

// Removes separator keys[i] and child[i + 1].
static void inner_remove_at(Inner *n, uint16_t i)
{
    memmove(&n->keys[i], &n->keys[i + 1], (n->count - i - 1) * sizeof(Key));
    memmove(&n->child[i + 1], &n->child[i + 2], (n->count - i - 1) * sizeof(Node *));
    --n->count;
}

// ---- descent ---------------------------------------------------------------

namespace {
struct Path {
    Inner *node[bt::MAX_HEIGHT];
    uint16_t slot[bt::MAX_HEIGHT];  // child index taken in node[i]
    int depth = 0;
    bool right_edge = true;  // took the last child at every level
};
}  // namespace

static Leaf *descend(Node *n, Key key, Path &path)
{
    while (!n->is_leaf) {
        Inner *in = as_inner(n);
        uint16_t c = upper_bound(in->keys, in->count, key);
        path.node[path.depth] = in;
        path.slot[path.depth] = c;
        ++path.depth;
        path.right_edge = path.right_edge && c == in->count;
        n = in->child[c];
    }
    return as_leaf(n);
}

// ---- node allocation -------------------------------------------------------

esp_err_t BTree::init()
{
    leaves_.emplace(mem::Region::Psram, NODE_SIZE, LEAF_NODES);
    inner_sram_.emplace(mem::Region::Internal, NODE_SIZE, INNER_SRAM_NODES);
    inner_psram_.emplace(mem::Region::Psram, NODE_SIZE, INNER_PSRAM_NODES);
    // The SRAM pool may legitimately be empty (inner nodes then all go to PSRAM).
    return (leaves_->capacity() && inner_psram_->capacity()) ? ESP_OK : ESP_ERR_NO_MEM;
}

Leaf *BTree::new_leaf()
{
    Leaf *l = new (leaves_->alloc()) Leaf();  // never null: insert() checked has_room()
    l->is_leaf = 1;
    return l;
}

Inner *BTree::new_inner()
{
    void *p = inner_sram_->alloc();
    const bool sram = p != nullptr;
    if (!sram) {
        p = inner_psram_->alloc();
    }
    Inner *in = new (p) Inner();
    in->in_sram = sram;
    return in;
}

void BTree::free_node(Node *n)
{
    if (n->is_leaf) {
        leaves_->free(n);
    } else if (n->in_sram) {
        inner_sram_->free(n);
    } else {
        inner_psram_->free(n);
    }
}

size_t BTree::inner_free() const
{
    return (inner_sram_->capacity() - inner_sram_->in_use()) +
           (inner_psram_->capacity() - inner_psram_->in_use());
}

bool BTree::has_room(size_t leaves, size_t inner) const
{
    return leaves_->capacity() - leaves_->in_use() >= leaves && inner_free() >= inner;
}

namespace {
struct Need {
    size_t leaves = 0;
    size_t inner = 0;
};
}  // namespace

// Nodes inserting a new key into `leaf` allocates: none if the leaf has room;
// else one leaf, plus one inner node per full ancestor going up, plus a new
// root if every ancestor is full (or the leaf is the root).
static Need nodes_needed(const Path &path, const Leaf *leaf)
{
    Need need;
    if (leaf->count < LEAF_CAP) {
        return need;
    }
    need.leaves = 1;
    int d = path.depth - 1;
    while (d >= 0 && path.node[d]->count == INNER_CAP) {
        ++need.inner;
        --d;
    }
    if (d < 0) {
        ++need.inner;
    }
    return need;
}

bool BTree::can_insert(Key key) const
{
    if (!root_) {
        return has_room(1, 0);
    }
    Path path;
    const Leaf *leaf = descend(root_, key, path);
    const uint16_t i = lower_bound(leaf->keys, leaf->count, key);
    if (i < leaf->count && leaf->keys[i] == key) {
        return true;  // update in place
    }
    const Need need = nodes_needed(path, leaf);
    return has_room(need.leaves, need.inner);
}

// ---- lookup ----------------------------------------------------------------

bool BTree::find(Key key, ValueRef *out) const
{
    const Node *n = root_;
    if (!n) {
        return false;
    }
    while (!n->is_leaf) {
        const Inner *in = as_inner(n);
        n = in->child[upper_bound(in->keys, in->count, key)];
    }
    const Leaf *l = as_leaf(n);
    uint16_t i = lower_bound(l->keys, l->count, key);
    if (i == l->count || l->keys[i] != key) {
        return false;
    }
    if (out) {
        *out = l->refs[i];
    }
    return true;
}

// ---- insert ----------------------------------------------------------------

esp_err_t BTree::insert(Key key, const ValueRef &ref)
{
    // Fast path: a key above the current maximum belongs at the end of the
    // rightmost leaf. Increasing keys hit this and skip the descent entirely.
    if (rightmost_ && rightmost_->count > 0 && rightmost_->count < LEAF_CAP &&
        key > rightmost_->keys[rightmost_->count - 1]) {
        rightmost_->keys[rightmost_->count] = key;
        rightmost_->refs[rightmost_->count] = ref;
        ++rightmost_->count;
        ++count_;
        return ESP_OK;
    }

    if (!root_) {
        if (!can_insert(key)) {
            return ESP_ERR_NO_MEM;
        }
        Leaf *l = new_leaf();
        leaf_insert_at(l, 0, key, ref);
        root_ = rightmost_ = l;
        height_ = 1;
        count_ = 1;
        return ESP_OK;
    }

    Path path;
    Leaf *leaf = descend(root_, key, path);
    uint16_t i = lower_bound(leaf->keys, leaf->count, key);
    if (i < leaf->count && leaf->keys[i] == key) {
        leaf->refs[i] = ref;  // update in place
        return ESP_OK;
    }
    const Need need = nodes_needed(path, leaf);
    if (!has_room(need.leaves, need.inner)) {
        return ESP_ERR_NO_MEM;
    }
    ++count_;
    if (leaf->count < LEAF_CAP) {
        leaf_insert_at(leaf, i, key, ref);
        return ESP_OK;
    }

    // Leaf is full: split.
    Leaf *right = new_leaf();
    if (path.right_edge && i == leaf->count) {
        // Appending past the largest key: start a fresh leaf and leave this one
        // 100% full. A 50/50 split here would leave every leaf half empty
        // forever under increasing keys.
        leaf_insert_at(right, 0, key, ref);
    } else {
        const uint16_t left_n = (LEAF_CAP + 1) / 2;
        if (i < left_n) {
            leaf_move_tail(leaf, left_n - 1, right);
            leaf_insert_at(leaf, i, key, ref);
        } else {
            leaf_move_tail(leaf, left_n, right);
            leaf_insert_at(right, i - left_n, key, ref);
        }
    }
    right->next = leaf->next;
    leaf->next = right;
    if (leaf == rightmost_) {
        rightmost_ = right;
    }

    // Push (sep, new_child) up until a parent has room.
    Key sep = right->keys[0];
    Node *new_child = right;
    while (path.depth > 0) {
        --path.depth;
        Inner *p = path.node[path.depth];
        const uint16_t c = path.slot[path.depth];
        if (p->count < INNER_CAP) {
            inner_insert_at(p, c, sep, new_child);
            return ESP_OK;
        }

        Inner *r = new_inner();
        if (path.right_edge && c == p->count) {
            // Same idea as the leaf: p stays full, new_child starts a fresh
            // node on the right edge, and sep moves up unchanged.
            r->child[0] = new_child;
        } else {
            // Merge the new separator into a scratch copy, keep the lower half,
            // move the upper half to r and push the middle key up.
            Key keys[INNER_CAP + 1];
            Node *child[INNER_CAP + 2];
            memcpy(keys, p->keys, c * sizeof(Key));
            keys[c] = sep;
            memcpy(keys + c + 1, p->keys + c, (INNER_CAP - c) * sizeof(Key));
            memcpy(child, p->child, (c + 1) * sizeof(Node *));
            child[c + 1] = new_child;
            memcpy(child + c + 2, p->child + c + 1, (INNER_CAP - c) * sizeof(Node *));

            const uint16_t left_n = (INNER_CAP + 1) / 2;
            p->count = left_n;
            memcpy(p->keys, keys, left_n * sizeof(Key));
            memcpy(p->child, child, (left_n + 1) * sizeof(Node *));
            r->count = INNER_CAP - left_n;
            memcpy(r->keys, keys + left_n + 1, r->count * sizeof(Key));
            memcpy(r->child, child + left_n + 1, (r->count + 1) * sizeof(Node *));
            sep = keys[left_n];
        }
        new_child = r;
    }

    // The root itself split: grow a level.
    Inner *root = new_inner();
    root->count = 1;
    root->keys[0] = sep;
    root->child[0] = root_;
    root->child[1] = new_child;
    root_ = root;
    ++height_;
    return ESP_OK;
}

// ---- erase -----------------------------------------------------------------

bool BTree::erase(Key key)
{
    if (!root_) {
        return false;
    }
    Path path;
    Leaf *leaf = descend(root_, key, path);
    uint16_t i = lower_bound(leaf->keys, leaf->count, key);
    if (i == leaf->count || leaf->keys[i] != key) {
        return false;
    }
    leaf_remove_at(leaf, i);
    --count_;
    // Separators equal to the removed key can stay: they're still valid bounds.

    Node *n = leaf;
    while (path.depth > 0 && n->count < (n->is_leaf ? bt::LEAF_MIN : bt::INNER_MIN)) {
        --path.depth;
        Inner *p = path.node[path.depth];
        if (n->is_leaf) {
            rebalance_leaf(p, path.slot[path.depth]);
        } else {
            rebalance_inner(p, path.slot[path.depth]);
        }
        n = p;
    }

    // Shrink from the top: an inner root with one child hands over to it.
    while (!root_->is_leaf && root_->count == 0) {
        Node *old = root_;
        root_ = as_inner(old)->child[0];
        free_node(old);
        --height_;
    }
    if (root_->is_leaf && root_->count == 0) {
        free_node(root_);
        root_ = nullptr;
        rightmost_ = nullptr;
        height_ = 0;
    }
    return true;
}

// child[idx] of parent is a leaf below LEAF_MIN.
void BTree::rebalance_leaf(Inner *parent, uint16_t idx)
{
    Leaf *child = as_leaf(parent->child[idx]);
    if (idx > 0) {
        Leaf *left = as_leaf(parent->child[idx - 1]);
        if (left->count + child->count <= bt::LEAF_MERGE_MAX) {
            merge_leaves(parent, idx - 1);
            return;
        }
        // Even out: move the top of left to the front of child.
        const uint16_t move = (left->count - child->count) / 2;
        memmove(&child->keys[move], &child->keys[0], child->count * sizeof(Key));
        memmove(&child->refs[move], &child->refs[0], child->count * sizeof(ValueRef));
        memcpy(&child->keys[0], &left->keys[left->count - move], move * sizeof(Key));
        memcpy(&child->refs[0], &left->refs[left->count - move], move * sizeof(ValueRef));
        left->count -= move;
        child->count += move;
        parent->keys[idx - 1] = child->keys[0];
        return;
    }
    if (idx < parent->count) {
        Leaf *right = as_leaf(parent->child[idx + 1]);
        if (child->count + right->count <= bt::LEAF_MERGE_MAX) {
            merge_leaves(parent, idx);
            return;
        }
        // Even out: move the bottom of right to the end of child.
        const uint16_t move = (right->count - child->count) / 2;
        memcpy(&child->keys[child->count], &right->keys[0], move * sizeof(Key));
        memcpy(&child->refs[child->count], &right->refs[0], move * sizeof(ValueRef));
        memmove(&right->keys[0], &right->keys[move], (right->count - move) * sizeof(Key));
        memmove(&right->refs[0], &right->refs[move], (right->count - move) * sizeof(ValueRef));
        child->count += move;
        right->count -= move;
        parent->keys[idx] = right->keys[0];
        return;
    }
    // Only child (a fresh right-edge node): nothing to pair with at this level.
    // The parent is then short too and gets fixed one level up.
}

void BTree::merge_leaves(Inner *parent, uint16_t idx)
{
    Leaf *left = as_leaf(parent->child[idx]);
    Leaf *right = as_leaf(parent->child[idx + 1]);
    leaf_move_tail(right, 0, left);
    left->next = right->next;
    if (right == rightmost_) {
        rightmost_ = left;
    }
    free_node(right);
    inner_remove_at(parent, idx);
}

// child[idx] of parent is an inner node below INNER_MIN. Moving entries
// between inner siblings rotates them through the parent's separator.
void BTree::rebalance_inner(Inner *parent, uint16_t idx)
{
    Inner *child = as_inner(parent->child[idx]);
    const uint16_t b = child->count;
    if (idx > 0) {
        Inner *left = as_inner(parent->child[idx - 1]);
        const uint16_t a = left->count;
        if (a + 1 + b <= bt::INNER_MERGE_MAX) {
            merge_inners(parent, idx - 1);
            return;
        }
        // Left's last `move` children go to child's front; left's key at
        // a - move becomes the new separator, the old separator comes down.
        const uint16_t move = (a - b) / 2;
        memmove(&child->keys[move], &child->keys[0], b * sizeof(Key));
        memmove(&child->child[move], &child->child[0], (b + 1) * sizeof(Node *));
        memcpy(&child->keys[0], &left->keys[a - move + 1], (move - 1) * sizeof(Key));
        child->keys[move - 1] = parent->keys[idx - 1];
        memcpy(&child->child[0], &left->child[a - move + 1], move * sizeof(Node *));
        parent->keys[idx - 1] = left->keys[a - move];
        left->count = a - move;
        child->count = b + move;
        return;
    }
    if (idx < parent->count) {
        Inner *right = as_inner(parent->child[idx + 1]);
        const uint16_t a = right->count;
        if (b + 1 + a <= bt::INNER_MERGE_MAX) {
            merge_inners(parent, idx);
            return;
        }
        // Mirror image: right's first `move` children go to child's end.
        const uint16_t move = (a - b) / 2;
        child->keys[b] = parent->keys[idx];
        memcpy(&child->keys[b + 1], &right->keys[0], (move - 1) * sizeof(Key));
        memcpy(&child->child[b + 1], &right->child[0], move * sizeof(Node *));
        parent->keys[idx] = right->keys[move - 1];
        memmove(&right->keys[0], &right->keys[move], (a - move) * sizeof(Key));
        memmove(&right->child[0], &right->child[move], (a - move + 1) * sizeof(Node *));
        right->count = a - move;
        child->count = b + move;
        return;
    }
    // Only child: fixed one level up, as for leaves.
}

void BTree::merge_inners(Inner *parent, uint16_t idx)
{
    Inner *left = as_inner(parent->child[idx]);
    Inner *right = as_inner(parent->child[idx + 1]);
    const uint16_t a = left->count;
    left->keys[a] = parent->keys[idx];
    memcpy(&left->keys[a + 1], &right->keys[0], right->count * sizeof(Key));
    memcpy(&left->child[a + 1], &right->child[0], (right->count + 1) * sizeof(Node *));
    left->count = a + 1 + right->count;
    free_node(right);
    inner_remove_at(parent, idx);
}

// ---- verification ----------------------------------------------------------

namespace {
struct VerifyState {
    int leaf_depth = -1;
    uint32_t keys = 0;
    const Leaf *prev_leaf = nullptr;
    bool ok = true;
};
}  // namespace

// Every key k in the subtree must satisfy lo <= k < hi (bounds optional).
static void verify_node(const Node *n, int depth, const Key *lo, const Key *hi, VerifyState &st)
{
    const Key *keys = n->is_leaf ? as_leaf(n)->keys : as_inner(n)->keys;
    const uint16_t cap = n->is_leaf ? LEAF_CAP : INNER_CAP;
    if (n->count > cap) {
        st.ok = false;
        return;
    }
    for (uint16_t i = 0; i < n->count; ++i) {
        if ((i > 0 && keys[i - 1] >= keys[i]) || (lo && keys[i] < *lo) || (hi && keys[i] >= *hi)) {
            st.ok = false;
        }
    }
    if (n->is_leaf) {
        const Leaf *l = as_leaf(n);
        if (st.leaf_depth == -1) {
            st.leaf_depth = depth;
        }
        if (depth != st.leaf_depth || (st.prev_leaf && st.prev_leaf->next != l)) {
            st.ok = false;
        }
        st.prev_leaf = l;
        st.keys += l->count;
        return;
    }
    const Inner *in = as_inner(n);
    for (uint16_t i = 0; i <= in->count; ++i) {
        verify_node(in->child[i], depth + 1, i > 0 ? &in->keys[i - 1] : lo,
                    i < in->count ? &in->keys[i] : hi, st);
    }
}

bool BTree::verify() const
{
    if (!root_) {
        return count_ == 0 && height_ == 0 && rightmost_ == nullptr;
    }
    VerifyState st;
    verify_node(root_, 1, nullptr, nullptr, st);
    return st.ok && st.keys == count_ && st.leaf_depth == static_cast<int>(height_) &&
           st.prev_leaf == rightmost_ && rightmost_->next == nullptr;
}

}  // namespace db

#pragma once
// btree.hpp — in-memory B+ tree: Key -> ValueRef (where the record lives in the log).
//
// The index itself is *not* persisted node-by-node to flash. It lives in RAM
// and is rebuilt on boot by replaying the log (optionally starting from an
// index snapshot referenced from dbmeta so boot doesn't scan all 14 MB). That
// avoids in-place node rewrites on NOR flash (erase-before-write, wear) entirely.
//
// Shape (512 B nodes, layouts in btree.cpp):
//   - leaves hold up to 41 (key, ValueRef) pairs and are chained by `next`
//     in key order, so a range scan is: find the first leaf, then walk.
//   - internal nodes hold up to 41 separator keys / 42 children. Separator
//     keys[i] splits child[i] (keys < it) from child[i+1] (keys >= it).
//   - keys come first in every node: a lookup's binary search touches only
//     the header + keys cache lines, never the values/children it skips.
//
// Optimisations for this workload:
//   - Increasing keys (timestamps, counters): a key past the current maximum
//     is appended straight to the rightmost leaf (no descent), and when that
//     leaf is full a fresh one is started instead of splitting 50/50, so
//     leaves stay 100% full instead of 50%. Same at the internal levels.
//   - Rebalance only below 25% full, and merge only when the result is at
//     most 75% full, otherwise redistribute evenly. No split/merge
//     ping-pong when inserts and deletes alternate around a boundary.
//
// Placement ("hot index"):
//   - internal nodes -> mem::Region::Internal first. Every lookup walks the
//     whole path from the root, so these are the hottest bytes in the system.
//     When the SRAM pool is used up they spill to a PSRAM pool.
//   - leaves -> mem::Region::Psram. One PSRAM touch per lookup.
//
// Capacity is fixed by the pool sizes below: 8192 leaves = 4 MB PSRAM,
// up to 336k keys packed (increasing keys), ~230k with random keys. Inner
// pools are sized for the worst case of ~leaves/11 nodes.
//
// Not thread-safe; db.cpp serialises access.

#include <cstddef>
#include <cstdint>
#include <optional>
#include "db/db.hpp"
#include "mem/pool.hpp"

namespace db {

// Where a put record starts in the log partition. The value length lives in
// the record header, which get() reads anyway to check the CRC; keeping it
// out of the tree makes a leaf entry 12 B instead of 16 B (41 vs 31 per leaf).
struct ValueRef {
    uint32_t offset;
};

constexpr size_t NODE_SIZE = 512;          // multiple of the cache line
constexpr size_t LEAF_NODES = 8192;        // PSRAM: 4 MB
constexpr size_t INNER_SRAM_NODES = 128;   // internal SRAM: 64 KB
constexpr size_t INNER_PSRAM_NODES = 768;  // PSRAM overflow for inner nodes: 384 KB

namespace bt {
struct Node;
struct Leaf;
struct Inner;
}  // namespace bt

class BTree {
public:
    esp_err_t init();  // allocates the node pools

    bool find(Key key, ValueRef *out) const;  // out may be nullptr (existence check)
    // Upsert. Updating an existing key never allocates. Inserting a new key
    // returns ESP_ERR_NO_MEM (tree unchanged) unless can_insert(key), so a
    // split chain can never run out of nodes halfway.
    esp_err_t insert(Key key, const ValueRef &ref);
    bool erase(Key key);  // false if the key wasn't there

    // True if insert(key, ...) will succeed: counts the nodes this particular
    // insert would allocate (usually none; one per full node on the path).
    bool can_insert(Key key) const;

    // Full structural check (ordering, separators, depth, leaf chain, counts).
    // O(n); for tests and debugging.
    bool verify() const;

    uint32_t size() const { return count_; }
    uint32_t height() const { return height_; }  // 0 = empty, 1 = root is a leaf
    uint32_t leaf_nodes() const { return leaves_ ? leaves_->in_use() : 0; }
    uint32_t inner_sram_nodes() const { return inner_sram_ ? inner_sram_->in_use() : 0; }
    uint32_t inner_psram_nodes() const { return inner_psram_ ? inner_psram_->in_use() : 0; }

private:
    bt::Leaf *new_leaf();
    bt::Inner *new_inner();
    void free_node(bt::Node *n);
    size_t inner_free() const;
    bool has_room(size_t leaves, size_t inner) const;
    void rebalance_leaf(bt::Inner *parent, uint16_t idx);
    void rebalance_inner(bt::Inner *parent, uint16_t idx);
    void merge_leaves(bt::Inner *parent, uint16_t idx);  // child[idx] absorbs child[idx+1]
    void merge_inners(bt::Inner *parent, uint16_t idx);

    std::optional<mem::Pool> leaves_;       // created in init(), not at static
    std::optional<mem::Pool> inner_sram_;   // construction: PSRAM may not be
    std::optional<mem::Pool> inner_psram_;  // in the heap yet at that point
    bt::Node *root_ = nullptr;
    bt::Leaf *rightmost_ = nullptr;  // leaf holding the largest keys (append fast path)
    uint32_t count_ = 0;
    uint32_t height_ = 0;
};

}  // namespace db

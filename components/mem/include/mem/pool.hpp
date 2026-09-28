#pragma once
// pool.hpp — fixed-size block pool (slab) on top of mem::alloc.
//
// B+ tree nodes are all the same size, allocated/freed constantly. A general
// heap (TLSF in IDF) works, but a pool gives O(1) alloc, zero per-block
// header overhead, no fragmentation, and a hard cap you can reason about
// ("the hot index can hold N nodes"). One Pool per (Region, node size).

#include <cstddef>
#include "mem/mem.hpp"

namespace mem {

class Pool {
public:
    Pool(Region region, size_t block_size, size_t block_count);
    ~Pool();

    Pool(const Pool &) = delete;
    Pool &operator=(const Pool &) = delete;

    void *alloc();          // nullptr when exhausted — caller decides to evict/spill
    void free(void *block);

    size_t capacity() const { return count_; }
    size_t in_use() const { return used_; }

private:
    void *base_ = nullptr;       // one contiguous slab
    void *free_list_ = nullptr;  // intrusive singly-linked list through free blocks
    size_t block_size_;
    size_t count_;
    size_t used_ = 0;
};

}  // namespace mem

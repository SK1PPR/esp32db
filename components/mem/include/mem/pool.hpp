#pragma once
// pool.hpp — fixed-size block pool (slab) on top of mem::alloc.
//
// B+ tree nodes are all the same size, allocated/freed constantly. A general
// heap (TLSF in IDF) works, but a pool gives O(1) alloc, zero per-block
// header overhead, no fragmentation, and a hard cap you can reason about
// ("the hot index can hold N nodes"). One Pool per (Region, node size).
// The blocks are purely an allocation policy: RAM itself is byte-addressable.

#include <cstddef>
#include "mem/mem.hpp"

namespace mem {

class Pool {
public:
    // Allocates its own slab of block_count blocks (freed with the pool).
    Pool(Region region, size_t block_size, size_t block_count);
    // Carves as many blocks as fit into an existing buffer, e.g. a
    // mem::balloon() arena. The pool does not own or free the buffer.
    Pool(void *base, size_t bytes, size_t block_size);
    ~Pool();

    Pool(const Pool &) = delete;
    Pool &operator=(const Pool &) = delete;

    void *alloc();          // nullptr when exhausted — caller decides to evict/spill
    void free(void *block);

    size_t capacity() const { return count_; }
    size_t in_use() const { return used_; }

private:
    void thread_free_list();  // link every block onto free_list_

    void *base_ = nullptr;       // one contiguous slab
    void *free_list_ = nullptr;  // intrusive singly-linked list through free blocks
    size_t block_size_;
    size_t count_;
    size_t used_ = 0;
    bool owns_base_;
};

}  // namespace mem

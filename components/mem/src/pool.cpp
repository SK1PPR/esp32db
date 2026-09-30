// pool.cpp — slab carved into block_count blocks, threaded onto an intrusive
// free list; alloc()/free() pop/push its head. Not thread-safe: the owner
// serialises access (the B+ tree runs under db.cpp's mutex).

#include <cassert>

#include "mem/pool.hpp"

namespace mem {

Pool::Pool(Region region, size_t block_size, size_t block_count)
    : block_size_(block_size), count_(block_count), owns_base_(true)
{
    assert(block_size >= sizeof(void *) && block_size % alignof(void *) == 0);

    base_ = mem::alloc(region, block_size * block_count, 64);
    if (!base_ || block_count == 0) {
        count_ = 0;  // empty pool: alloc() just returns nullptr
        return;
    }
    thread_free_list();
}

Pool::Pool(void *base, size_t bytes, size_t block_size)
    : base_(base), block_size_(block_size), count_(base ? bytes / block_size : 0), owns_base_(false)
{
    assert(block_size >= sizeof(void *) && block_size % alignof(void *) == 0);

    if (count_ == 0) {
        return;
    }
    thread_free_list();
}

void Pool::thread_free_list()
{
    char *p = static_cast<char *>(base_);
    for (size_t i = 0; i + 1 < count_; ++i) {
        *reinterpret_cast<void **>(p + i * block_size_) = p + (i + 1) * block_size_;
    }
    *reinterpret_cast<void **>(p + (count_ - 1) * block_size_) = nullptr;
    free_list_ = base_;
}

Pool::~Pool()
{
    if (owns_base_) {
        mem::free(base_);
    }
    base_ = nullptr;
    free_list_ = nullptr;
}

void *Pool::alloc()
{
    if (!free_list_ || used_ >= count_) {
        return nullptr;
    }
    void *block = free_list_;
    free_list_ = *static_cast<void **>(block);
    ++used_;
    return block;
}

void Pool::free(void *block)
{
    if (!block || used_ == 0) {
        return;
    }
    *static_cast<void **>(block) = free_list_;
    free_list_ = block;
    --used_;
}

}  // namespace mem

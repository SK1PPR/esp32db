// pool.cpp — TODO: carve base_ into block_count blocks, thread them onto
// free_list_, pop/push in alloc()/free(). Add a mutex (or make it per-task)
// once more than one task touches the tree.

#include <cassert>

#include "mem/pool.hpp"

namespace mem {

Pool::Pool(Region region, size_t block_size, size_t block_count)
    : block_size_(block_size), count_(block_count)
{
    assert(block_size >= sizeof(void *) && block_size % alignof(void *) == 0);

    base_ = mem::alloc(region, block_size * block_count, 64);
    if (!base_ || block_count == 0) {
        count_ = 0;  // empty pool: alloc() just returns nullptr
        return;
    }

    char *p = static_cast<char *>(base_);
    for (size_t i = 0; i + 1 < block_count; ++i) {
        *reinterpret_cast<void **>(p + i * block_size) = p + (i + 1) * block_size;
    }
    *reinterpret_cast<void **>(p + (block_count - 1) * block_size) = nullptr;
    free_list_ = base_;
}

Pool::~Pool()
{
    mem::free(base_);
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

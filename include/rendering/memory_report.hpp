#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <vk_mem_alloc.h>

#include "rendering/memory_growth.hpp"

namespace wowee::rendering {

/// The bytes an allocation took, 0 for none.
inline uint64_t allocationBytes(VmaAllocator allocator, VmaAllocation allocation) {
    if (!allocator || !allocation) return 0;
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(allocator, allocation, &info);
    return info.size;
}

/// The capacity a vector holds, in bytes.
template <typename V>
inline uint64_t vectorBytes(const V& v) {
    return uint64_t(v.capacity()) * sizeof(typename V::value_type);
}

}  // namespace wowee::rendering

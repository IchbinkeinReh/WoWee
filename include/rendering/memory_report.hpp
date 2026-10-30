#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <vk_mem_alloc.h>

namespace wowee::rendering {

/// One owner's share of the memory, for /mem: what it holds on the GPU (in
/// Vulkan allocations) and in the process's own heap. On a phone both come
/// out of the same RAM.
struct MemoryEntry {
    std::string name;
    uint64_t gpuBytes = 0;
    uint64_t cpuBytes = 0;
};

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

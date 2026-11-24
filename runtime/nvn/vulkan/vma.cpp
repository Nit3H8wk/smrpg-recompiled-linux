// Vulkan Memory Allocator implementation (header-only library, MSYS2 package vulkan-memory-allocator).
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#if __has_include(<vma/vk_mem_alloc.h>)
#include <vma/vk_mem_alloc.h>
#elif __has_include("../../../third_party/vma/vk_mem_alloc.h")
#include "../../../third_party/vma/vk_mem_alloc.h"  // (vendored: VulkanMemoryAllocator v3.5.0, BSD-2-Clause)
#else
#include <vk_mem_alloc.h>  // (Debian / Ubuntu libvulkan-memory-allocator-dev)
#endif

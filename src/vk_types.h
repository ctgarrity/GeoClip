#pragma once
#include <volk.h>
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <deque>
#include <functional>
#include <span>
#include <vector>

struct DeletionQueue
{
    std::deque<std::function<void()>> deletors;

    void push(std::function<void()>&& fn) { deletors.push_back(std::move(fn)); }

    void flush()
    {
        for (auto it = deletors.rbegin(); it != deletors.rend(); ++it)
            (*it)();
        deletors.clear();
    }
};

struct AllocatedBuffer
{
    VkBuffer          buffer{VK_NULL_HANDLE};
    VmaAllocation     allocation{};
    VmaAllocationInfo info{};
};

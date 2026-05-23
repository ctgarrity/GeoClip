#pragma once
#include <stdexcept>
#include <string>
#include <volk.h>

#define VK_CHECK(expr) \
    do { \
        VkResult _r = (expr); \
        if (_r != VK_SUCCESS) \
            throw std::runtime_error(#expr " failed (VkResult=" + std::to_string(_r) + ")"); \
    } while (0)
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

struct Vertex
{
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
};  // stride = 32 bytes

struct Mesh
{
    AllocatedBuffer vertex_buffer{};
    AllocatedBuffer index_buffer{};
    uint32_t        index_count{0};
};

struct AllocatedImage
{
    VkImage       image{VK_NULL_HANDLE};
    VkImageView   view{VK_NULL_HANDLE};
    VmaAllocation allocation{};
    VkExtent2D    extent{};
    VkFormat      format{};
};

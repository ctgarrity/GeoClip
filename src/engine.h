#pragma once
#include "vk_types.h"
#include <SDL3/SDL.h>
#include <array>

constexpr uint32_t FRAMES_IN_FLIGHT = 2;

struct FrameData
{
    VkCommandPool   cmd_pool;
    VkCommandBuffer cmd;
    VkFence         fence;
};

class Engine
{
public:
    void init();
    void run();
    void cleanup();

private:
    SDL_Window* _window{};

    VkInstance               _instance{};
    VkDebugUtilsMessengerEXT _debug_messenger{};
    VkSurfaceKHR             _surface{};
    VkPhysicalDevice         _physical_device{};
    VkDevice                 _device{};
    VkQueue                  _graphics_queue{};
    uint32_t                 _graphics_queue_family{};

    VkSwapchainKHR           _swapchain{};
    VkFormat                 _swapchain_format{};
    VkExtent2D               _swapchain_extent{};
    std::vector<VkImage>     _swapchain_images;
    std::vector<VkImageView> _swapchain_views;
    AllocatedImage           _depth{};

    std::array<FrameData, FRAMES_IN_FLIGHT> _frames{};
    uint32_t   _frame_number{0};
    FrameData& current_frame() { return _frames[_frame_number % FRAMES_IN_FLIGHT]; }

    glm::vec3 _cam_pos{0.f, 0.f, 3.f};
    float     _yaw{-90.f};
    float     _pitch{0.f};
    bool      _mouse_captured{false};
    float     _cam_speed{3.f};
    float     _mouse_sensitivity{0.1f};

    VmaAllocator _allocator{};

    // _acquire_semaphores: indexed by frame number — prevents reuse before the GPU finishes.
    // _present_semaphores: indexed by swapchain image index — prevents reuse before
    //                      the presentation engine releases that image.
    std::vector<VkSemaphore> _acquire_semaphores;
    std::vector<VkSemaphore> _present_semaphores;

    Mesh _mesh{};

    VkShaderEXT           _vert_shader{};
    VkShaderEXT           _frag_shader{};
    VkDescriptorSetLayout _empty_set_layout{};
    VkPipelineLayout      _pipeline_layout{};
    AllocatedBuffer       _descriptor_buffer{};

    VkShaderEXT      _compute_shader{};
    VkPipelineLayout _compute_layout{};
    AllocatedBuffer  _rotation_buf{};
    VkDeviceAddress  _rotation_buf_bda{0};
    float            _rotation_angle{0.f};
    float            _dt{0.f};

    DeletionQueue _deletions;

    void init_vulkan();
    void init_swapchain();
    void init_commands();
    void init_sync();
    void init_allocator();
    void init_descriptor_buffer();
    void init_shaders();
    void load_mesh();

    void destroy_swapchain();
    void recreate_swapchain();

    bool _swapchain_dirty{false};

    void draw();
};

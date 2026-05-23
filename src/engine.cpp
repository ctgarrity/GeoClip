#include "engine.h"
#include <SDL3/SDL_vulkan.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <unistd.h>

// ─── helpers ────────────────────────────────────────────────────────────────

static std::filesystem::path exe_dir()
{
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n < 0) return ".";
    buf[n] = '\0';
    return std::filesystem::path(buf).parent_path();
}

static std::vector<uint32_t> read_spirv(const std::filesystem::path& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        throw std::runtime_error("failed to open shader: " + path.string());
    size_t sz = f.tellg();
    f.seekg(0);
    std::vector<uint32_t> buf(sz / 4);
    f.read(reinterpret_cast<char*>(buf.data()), sz);
    return buf;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT      severity,
    VkDebugUtilsMessageTypeFlagsEXT             /*type*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void*                                       /*user*/)
{
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        fprintf(stderr, "[vk] %s\n", data->pMessage);
    return VK_FALSE;
}

// ─── public ─────────────────────────────────────────────────────────────────

void Engine::init()
{
    if (!SDL_Init(SDL_INIT_VIDEO))
        throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());
    _window = SDL_CreateWindow("GeoClip", 1280, 720,
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!_window)
        throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());

    init_vulkan();
    init_swapchain();
    init_commands();
    init_sync();
    init_allocator();
    init_descriptor_buffer();
    init_shaders();
}

void Engine::run()
{
    bool running = true;
    SDL_Event event;
    while (running)
    {
        while (SDL_PollEvent(&event))
        {
            if (event.type == SDL_EVENT_QUIT)
                running = false;
            if (event.type == SDL_EVENT_WINDOW_RESIZED ||
                event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
                _swapchain_dirty = true;
        }
        if (_swapchain_dirty)
            recreate_swapchain();
        draw();
    }
    vkDeviceWaitIdle(_device);
}

void Engine::cleanup()
{
    destroy_swapchain();
    _deletions.flush();
    SDL_DestroyWindow(_window);
    SDL_Quit();
}

// ─── init_vulkan ────────────────────────────────────────────────────────────

void Engine::init_vulkan()
{
    if (volkInitialize() != VK_SUCCESS)
        throw std::runtime_error("volkInitialize failed — Vulkan loader not found");

    // --- instance ---
    uint32_t sdl_ext_count = 0;
    const char* const* sdl_exts = SDL_Vulkan_GetInstanceExtensions(&sdl_ext_count);

    std::vector<const char*> instance_exts(sdl_exts, sdl_exts + sdl_ext_count);
#ifndef NDEBUG
    instance_exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
#endif

    VkDebugUtilsMessengerCreateInfoEXT debug_ci{
        .sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                         | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                         | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                         | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = debug_callback,
    };

    VkApplicationInfo app_info{
        .sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "GeoClip",
        .apiVersion       = VK_API_VERSION_1_3,
    };

#ifndef NDEBUG
    const char* layers[]         = { "VK_LAYER_KHRONOS_validation" };
    uint32_t    layer_count      = 1;
    const void* inst_pnext       = &debug_ci;
#else
    const char** layers          = nullptr;
    uint32_t     layer_count     = 0;
    const void*  inst_pnext      = nullptr;
#endif

    VkInstanceCreateInfo inst_ci{
        .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext                   = inst_pnext,
        .pApplicationInfo        = &app_info,
        .enabledLayerCount       = layer_count,
        .ppEnabledLayerNames     = layers,
        .enabledExtensionCount   = (uint32_t)instance_exts.size(),
        .ppEnabledExtensionNames = instance_exts.data(),
    };
    VK_CHECK(vkCreateInstance(&inst_ci, nullptr, &_instance));
    volkLoadInstance(_instance);
    _deletions.push([&] { vkDestroyInstance(_instance, nullptr); });

#ifndef NDEBUG
    VK_CHECK(vkCreateDebugUtilsMessengerEXT(_instance, &debug_ci, nullptr, &_debug_messenger));
    _deletions.push([&] {
        vkDestroyDebugUtilsMessengerEXT(_instance, _debug_messenger, nullptr);
    });
#endif

    // --- surface ---
    if (!SDL_Vulkan_CreateSurface(_window, _instance, nullptr, &_surface))
        throw std::runtime_error(std::string("SDL_Vulkan_CreateSurface failed: ") + SDL_GetError());
    _deletions.push([&] { vkDestroySurfaceKHR(_instance, _surface, nullptr); });

    // --- physical device ---
    const char* required_device_exts[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_EXT_SHADER_OBJECT_EXTENSION_NAME,
        VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
    };

    uint32_t phys_count = 0;
    vkEnumeratePhysicalDevices(_instance, &phys_count, nullptr);
    std::vector<VkPhysicalDevice> phys_devices(phys_count);
    vkEnumeratePhysicalDevices(_instance, &phys_count, phys_devices.data());

    for (auto& pd : phys_devices)
    {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pd, &props);

        // Check required extensions
        uint32_t ext_count = 0;
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &ext_count, nullptr);
        std::vector<VkExtensionProperties> exts(ext_count);
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &ext_count, exts.data());

        bool all_found = true;
        for (auto& req : required_device_exts)
        {
            bool found = false;
            for (auto& e : exts)
                if (strcmp(e.extensionName, req) == 0) { found = true; break; }
            if (!found) { all_found = false; break; }
        }
        if (!all_found) continue;

        _physical_device = pd;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) break;
    }
    if (_physical_device == VK_NULL_HANDLE)
        throw std::runtime_error("no suitable GPU with required extensions found");

    // --- queue family ---
    uint32_t qf_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(_physical_device, &qf_count, nullptr);
    std::vector<VkQueueFamilyProperties> qf_props(qf_count);
    vkGetPhysicalDeviceQueueFamilyProperties(_physical_device, &qf_count, qf_props.data());

    for (uint32_t i = 0; i < qf_count; ++i)
    {
        VkBool32 present_support = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(_physical_device, i, _surface, &present_support);
        if ((qf_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present_support)
        {
            _graphics_queue_family = i;
            break;
        }
    }

    // --- logical device ---
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_ci{
        .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = _graphics_queue_family,
        .queueCount       = 1,
        .pQueuePriorities = &priority,
    };

    VkPhysicalDeviceVulkan11Features features11{
        .sType                = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
        .shaderDrawParameters = VK_TRUE,  // SV_VertexID emits DrawParameters capability
    };
    VkPhysicalDeviceVulkan12Features features12{
        .sType               = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext               = &features11,
        .bufferDeviceAddress = VK_TRUE,  // required for descriptor buffer device addresses
    };
    VkPhysicalDeviceVulkan13Features features13{
        .sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .pNext            = &features12,
        .synchronization2 = VK_TRUE,
        .dynamicRendering = VK_TRUE,
    };
    VkPhysicalDeviceShaderObjectFeaturesEXT shader_obj_features{
        .sType        = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT,
        .pNext        = &features13,
        .shaderObject = VK_TRUE,
    };
    VkPhysicalDeviceDescriptorBufferFeaturesEXT desc_buf_features{
        .sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT,
        .pNext            = &shader_obj_features,
        .descriptorBuffer = VK_TRUE,
    };
    VkPhysicalDeviceFeatures2 features2{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &desc_buf_features,
    };

    VkDeviceCreateInfo dev_ci{
        .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext                   = &features2,
        .queueCreateInfoCount    = 1,
        .pQueueCreateInfos       = &queue_ci,
        .enabledExtensionCount   = (uint32_t)std::size(required_device_exts),
        .ppEnabledExtensionNames = required_device_exts,
    };
    VK_CHECK(vkCreateDevice(_physical_device, &dev_ci, nullptr, &_device));
    volkLoadDevice(_device);
    _deletions.push([&] { vkDestroyDevice(_device, nullptr); });

    vkGetDeviceQueue(_device, _graphics_queue_family, 0, &_graphics_queue);
}

// ─── init_swapchain ─────────────────────────────────────────────────────────

void Engine::init_swapchain()
{
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(_physical_device, _surface, &caps);

    uint32_t fmt_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(_physical_device, _surface, &fmt_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fmt_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(_physical_device, _surface, &fmt_count, formats.data());

    VkSurfaceFormatKHR chosen = formats[0];
    for (auto& f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        { chosen = f; break; }

    _swapchain_format = chosen.format;

    // On Wayland (and some other compositors) currentExtent is UINT32_MAX,
    // meaning the swapchain decides the size — query the window's pixel size instead.
    if (caps.currentExtent.width != UINT32_MAX)
    {
        _swapchain_extent = caps.currentExtent;
    }
    else
    {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(_window, &w, &h);
        _swapchain_extent = {
            std::clamp((uint32_t)w, caps.minImageExtent.width, caps.maxImageExtent.width),
            std::clamp((uint32_t)h, caps.minImageExtent.height, caps.maxImageExtent.height),
        };
    }

    uint32_t image_count = std::min(caps.minImageCount + 1,
        caps.maxImageCount == 0 ? UINT32_MAX : caps.maxImageCount);

    VkSwapchainCreateInfoKHR sc_ci{
        .sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface          = _surface,
        .minImageCount    = image_count,
        .imageFormat      = chosen.format,
        .imageColorSpace  = chosen.colorSpace,
        .imageExtent      = _swapchain_extent,
        .imageArrayLayers = 1,
        .imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform     = caps.currentTransform,
        .compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode      = VK_PRESENT_MODE_FIFO_KHR,
        .clipped          = VK_TRUE,
    };
    VK_CHECK(vkCreateSwapchainKHR(_device, &sc_ci, nullptr, &_swapchain));

    uint32_t img_count = 0;
    vkGetSwapchainImagesKHR(_device, _swapchain, &img_count, nullptr);
    _swapchain_images.resize(img_count);
    vkGetSwapchainImagesKHR(_device, _swapchain, &img_count, _swapchain_images.data());

    _swapchain_views.resize(img_count);
    for (uint32_t i = 0; i < img_count; ++i)
    {
        VkImageViewCreateInfo view_ci{
            .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image    = _swapchain_images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format   = _swapchain_format,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        VK_CHECK(vkCreateImageView(_device, &view_ci, nullptr, &_swapchain_views[i]));
    }

    // Semaphores are tied to swapchain image count — create them here so they
    // are rebuilt whenever init_swapchain() is called (initial + recreation).
    VkSemaphoreCreateInfo sem_ci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    _acquire_semaphores.resize(img_count);
    _present_semaphores.resize(img_count);
    for (uint32_t i = 0; i < img_count; ++i)
    {
        VK_CHECK(vkCreateSemaphore(_device, &sem_ci, nullptr, &_acquire_semaphores[i]));
        VK_CHECK(vkCreateSemaphore(_device, &sem_ci, nullptr, &_present_semaphores[i]));
    }
}

// ─── init_commands ──────────────────────────────────────────────────────────

void Engine::init_commands()
{
    VkCommandPoolCreateInfo pool_ci{
        .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = _graphics_queue_family,
    };
    VkCommandBufferAllocateInfo alloc_ci{
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };

    for (auto& frame : _frames)
    {
        VK_CHECK(vkCreateCommandPool(_device, &pool_ci, nullptr, &frame.cmd_pool));
        alloc_ci.commandPool = frame.cmd_pool;
        VK_CHECK(vkAllocateCommandBuffers(_device, &alloc_ci, &frame.cmd));
        _deletions.push([this, &frame] {
            vkDestroyCommandPool(_device, frame.cmd_pool, nullptr);
        });
    }
}

// ─── init_sync ──────────────────────────────────────────────────────────────

void Engine::init_sync()
{
    VkFenceCreateInfo fence_ci{
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    for (auto& frame : _frames)
    {
        VK_CHECK(vkCreateFence(_device, &fence_ci, nullptr, &frame.fence));
        VkFence f = frame.fence;  // capture value, not reference, to avoid aliasing on recreate
        _deletions.push([this, f] { vkDestroyFence(_device, f, nullptr); });
    }
}

// ─── destroy_swapchain / recreate_swapchain ─────────────────────────────────

void Engine::destroy_swapchain()
{
    vkDeviceWaitIdle(_device);
    for (auto v : _swapchain_views)
        vkDestroyImageView(_device, v, nullptr);
    _swapchain_views.clear();
    _swapchain_images.clear();
    for (auto s : _acquire_semaphores)
        vkDestroySemaphore(_device, s, nullptr);
    for (auto s : _present_semaphores)
        vkDestroySemaphore(_device, s, nullptr);
    _acquire_semaphores.clear();
    _present_semaphores.clear();
    vkDestroySwapchainKHR(_device, _swapchain, nullptr);
    _swapchain = VK_NULL_HANDLE;
}

void Engine::recreate_swapchain()
{
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(_window, &w, &h);
    if (w == 0 || h == 0)
        return;  // minimized — skip until we have a real size

    destroy_swapchain();
    init_swapchain();  // also recreates semaphores
    _swapchain_dirty = false;
}

// ─── init_allocator ─────────────────────────────────────────────────────────

void Engine::init_allocator()
{
    VmaVulkanFunctions vma_funcs{
        .vkGetInstanceProcAddr = vkGetInstanceProcAddr,
        .vkGetDeviceProcAddr   = vkGetDeviceProcAddr,
    };
    VmaAllocatorCreateInfo alloc_ci{
        .flags            = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
        .physicalDevice   = _physical_device,
        .device           = _device,
        .pVulkanFunctions = &vma_funcs,
        .instance         = _instance,
        .vulkanApiVersion = VK_API_VERSION_1_3,
    };
    vmaCreateAllocator(&alloc_ci, &_allocator);
    _deletions.push([&] { vmaDestroyAllocator(_allocator); });
}

// ─── init_descriptor_buffer ─────────────────────────────────────────────────

void Engine::init_descriptor_buffer()
{
    // Empty descriptor set layout using the descriptor buffer extension
    VkDescriptorSetLayoutCreateInfo layout_ci{
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
        .bindingCount = 0,
    };
    VK_CHECK(vkCreateDescriptorSetLayout(_device, &layout_ci, nullptr, &_empty_set_layout));
    _deletions.push([&] {
        vkDestroyDescriptorSetLayout(_device, _empty_set_layout, nullptr);
    });

    // Pipeline layout — required for vkCmdSetDescriptorBufferOffsetsEXT
    VkPipelineLayoutCreateInfo pl_ci{
        .sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts    = &_empty_set_layout,
    };
    VK_CHECK(vkCreatePipelineLayout(_device, &pl_ci, nullptr, &_pipeline_layout));
    _deletions.push([&] {
        vkDestroyPipelineLayout(_device, _pipeline_layout, nullptr);
    });

    // Allocate the descriptor buffer (empty layout may report size 0; allocate at least 4)
    VkDeviceSize layout_size = 0;
    vkGetDescriptorSetLayoutSizeEXT(_device, _empty_set_layout, &layout_size);
    layout_size = std::max(layout_size, VkDeviceSize{4});

    VkBufferCreateInfo buf_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size  = layout_size,
        .usage = VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT
               | VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT
               | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
    };
    VmaAllocationCreateInfo vma_ci{
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
               | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO,
    };
    VK_CHECK(vmaCreateBuffer(_allocator, &buf_ci, &vma_ci,
        &_descriptor_buffer.buffer,
        &_descriptor_buffer.allocation,
        &_descriptor_buffer.info));
    _deletions.push([&] {
        vmaDestroyBuffer(_allocator, _descriptor_buffer.buffer, _descriptor_buffer.allocation);
    });
}

// ─── init_shaders ───────────────────────────────────────────────────────────

void Engine::init_shaders()
{
    auto base       = exe_dir();
    auto vert_spirv = read_spirv(base / "shaders/triangle.vert.spv");
    auto frag_spirv = read_spirv(base / "shaders/triangle.frag.spv");

    VkShaderCreateInfoEXT shader_infos[2] = {
        {
            .sType          = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
            .flags          = VK_SHADER_CREATE_LINK_STAGE_BIT_EXT,
            .stage          = VK_SHADER_STAGE_VERTEX_BIT,
            .nextStage      = VK_SHADER_STAGE_FRAGMENT_BIT,
            .codeType       = VK_SHADER_CODE_TYPE_SPIRV_EXT,
            .codeSize       = vert_spirv.size() * sizeof(uint32_t),
            .pCode          = vert_spirv.data(),
            .pName          = "main",
            .setLayoutCount = 1,
            .pSetLayouts    = &_empty_set_layout,
        },
        {
            .sType          = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
            .flags          = VK_SHADER_CREATE_LINK_STAGE_BIT_EXT,
            .stage          = VK_SHADER_STAGE_FRAGMENT_BIT,
            .nextStage      = 0,
            .codeType       = VK_SHADER_CODE_TYPE_SPIRV_EXT,
            .codeSize       = frag_spirv.size() * sizeof(uint32_t),
            .pCode          = frag_spirv.data(),
            .pName          = "main",
            .setLayoutCount = 1,
            .pSetLayouts    = &_empty_set_layout,
        },
    };

    VkShaderEXT shaders[2];
    VK_CHECK(vkCreateShadersEXT(_device, 2, shader_infos, nullptr, shaders));
    _vert_shader = shaders[0];
    _frag_shader = shaders[1];
    _deletions.push([&] {
        vkDestroyShaderEXT(_device, _vert_shader, nullptr);
        vkDestroyShaderEXT(_device, _frag_shader, nullptr);
    });
}

// ─── draw ───────────────────────────────────────────────────────────────────

void Engine::draw()
{
    auto& frame = current_frame();

    vkWaitForFences(_device, 1, &frame.fence, VK_TRUE, 1'000'000'000);

    // Rotating acquire semaphore — indexed by frame number so we don't reuse
    // one that the presentation engine might still be holding.
    uint32_t    acq_idx     = _frame_number % (uint32_t)_acquire_semaphores.size();
    VkSemaphore acquire_sem = _acquire_semaphores[acq_idx];

    uint32_t image_index = 0;
    VkResult acq_result  = vkAcquireNextImageKHR(_device, _swapchain, 1'000'000'000,
        acquire_sem, VK_NULL_HANDLE, &image_index);

    if (acq_result == VK_ERROR_OUT_OF_DATE_KHR)
    {
        _swapchain_dirty = true;
        return;
    }

    // Reset fence only after we know we'll submit; otherwise the fence stays
    // signaled and the next wait returns instantly without a matching submit.
    vkResetFences(_device, 1, &frame.fence);

    VkSemaphore present_sem = _present_semaphores[image_index];

    vkResetCommandPool(_device, frame.cmd_pool, 0);

    VkCommandBufferBeginInfo begin{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(frame.cmd, &begin);

    // Transition: UNDEFINED → COLOR_ATTACHMENT_OPTIMAL
    VkImageMemoryBarrier2 barrier{
        .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask     = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .srcAccessMask    = 0,
        .dstStageMask     = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask    = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout        = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .image            = _swapchain_images[image_index],
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    VkDependencyInfo dep{
        .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers    = &barrier,
    };
    vkCmdPipelineBarrier2(frame.cmd, &dep);

    // Dynamic rendering
    VkRenderingAttachmentInfo color_att{
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = _swapchain_views[image_index],
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue  = {.color = {0.1f, 0.1f, 0.1f, 1.0f}},
    };
    VkRenderingInfo rendering{
        .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea           = {{0, 0}, _swapchain_extent},
        .layerCount           = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments    = &color_att,
    };
    vkCmdBeginRendering(frame.cmd, &rendering);

    // Bind descriptor buffer
    VkBufferDeviceAddressInfo addr_info{
        .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = _descriptor_buffer.buffer,
    };
    VkDescriptorBufferBindingInfoEXT binding{
        .sType   = VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT,
        .address = vkGetBufferDeviceAddress(_device, &addr_info),
        .usage   = VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT
                 | VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT,
    };
    vkCmdBindDescriptorBuffersEXT(frame.cmd, 1, &binding);

    uint32_t     buf_index = 0;
    VkDeviceSize offset    = 0;
    vkCmdSetDescriptorBufferOffsetsEXT(frame.cmd,
        VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline_layout,
        0, 1, &buf_index, &offset);

    // Bind shader objects
    VkShaderStageFlagBits stages[2] = {
        VK_SHADER_STAGE_VERTEX_BIT,
        VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    VkShaderEXT shaders[2] = {_vert_shader, _frag_shader};
    vkCmdBindShadersEXT(frame.cmd, 2, stages, shaders);

    // Dynamic state — shader objects carry no implicit state
    VkViewport viewport{
        .x = 0, .y = 0,
        .width  = (float)_swapchain_extent.width,
        .height = (float)_swapchain_extent.height,
        .minDepth = 0.0f, .maxDepth = 1.0f,
    };
    VkRect2D scissor{{0, 0}, _swapchain_extent};
    vkCmdSetViewportWithCount(frame.cmd, 1, &viewport);
    vkCmdSetScissorWithCount(frame.cmd, 1, &scissor);
    vkCmdSetPrimitiveTopology(frame.cmd, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    vkCmdSetPrimitiveRestartEnable(frame.cmd, VK_FALSE);
    vkCmdSetRasterizerDiscardEnable(frame.cmd, VK_FALSE);
    vkCmdSetDepthBiasEnable(frame.cmd, VK_FALSE);
    vkCmdSetCullMode(frame.cmd, VK_CULL_MODE_NONE);
    vkCmdSetFrontFace(frame.cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdSetPolygonModeEXT(frame.cmd, VK_POLYGON_MODE_FILL);
    vkCmdSetDepthTestEnable(frame.cmd, VK_FALSE);
    vkCmdSetDepthWriteEnable(frame.cmd, VK_FALSE);
    vkCmdSetStencilTestEnable(frame.cmd, VK_FALSE);
    VkSampleMask sample_mask = 0xFFFFFFFF;
    vkCmdSetRasterizationSamplesEXT(frame.cmd, VK_SAMPLE_COUNT_1_BIT);
    vkCmdSetSampleMaskEXT(frame.cmd, VK_SAMPLE_COUNT_1_BIT, &sample_mask);
    vkCmdSetAlphaToCoverageEnableEXT(frame.cmd, VK_FALSE);
    VkColorComponentFlags color_mask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkBool32 blend_enable = VK_FALSE;
    vkCmdSetColorBlendEnableEXT(frame.cmd, 0, 1, &blend_enable);
    vkCmdSetColorWriteMaskEXT(frame.cmd, 0, 1, &color_mask);
    vkCmdSetVertexInputEXT(frame.cmd, 0, nullptr, 0, nullptr);

    vkCmdDraw(frame.cmd, 3, 1, 0, 0);
    vkCmdEndRendering(frame.cmd);

    // Transition: COLOR_ATTACHMENT_OPTIMAL → PRESENT_SRC_KHR
    barrier.srcStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstStageMask  = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
    barrier.dstAccessMask = 0;
    barrier.oldLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier2(frame.cmd, &dep);

    vkEndCommandBuffer(frame.cmd);

    // Submit (synchronization2)
    VkSemaphoreSubmitInfo wait_info{
        .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = acquire_sem,
        .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    };
    VkSemaphoreSubmitInfo signal_info{
        .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = present_sem,
        .stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,
    };
    VkCommandBufferSubmitInfo cmd_info{
        .sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = frame.cmd,
    };
    VkSubmitInfo2 submit{
        .sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount   = 1,
        .pWaitSemaphoreInfos      = &wait_info,
        .commandBufferInfoCount   = 1,
        .pCommandBufferInfos      = &cmd_info,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos    = &signal_info,
    };
    vkQueueSubmit2(_graphics_queue, 1, &submit, frame.fence);

    VkPresentInfoKHR present{
        .sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores    = &present_sem,
        .swapchainCount     = 1,
        .pSwapchains        = &_swapchain,
        .pImageIndices      = &image_index,
    };
    VkResult present_result = vkQueuePresentKHR(_graphics_queue, &present);
    if (present_result == VK_ERROR_OUT_OF_DATE_KHR || present_result == VK_SUBOPTIMAL_KHR)
        _swapchain_dirty = true;

    _frame_number++;
}

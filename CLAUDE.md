# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

```bash
# First-time setup
git submodule update --init --recursive

# Configure + build (debug)
cmake --preset debug
cmake --build build/debug

# Configure + build (release)
cmake --preset release
cmake --build build/release

# Run
./build/debug/GeoClip
```

Presets use Ninja and write all artifacts to `build/<preset>/`. `compile_commands.json` lands in `build/debug/` and is picked up by clangd automatically.

## Architecture

C++23 Vulkan graphics application. Source lives under `src/`, GLSL shaders under `shaders/`. All dependencies are git submodules under `third_party/` consumed via `add_subdirectory`.

### Source files

| File | Role |
|---|---|
| `src/main.cpp` | Entry point — creates `Engine`, calls init/run/cleanup |
| `src/engine.h/.cpp` | `Engine` class — owns all Vulkan state, renders the frame |
| `src/vk_types.h` | Common includes, `DeletionQueue`, `AllocatedBuffer` |
| `src/vma_impl.cpp` | Single TU that defines `VMA_IMPLEMENTATION` |

### Rendering approach

- **No render passes / framebuffers** — `VK_KHR_dynamic_rendering` (`vkCmdBeginRendering`)
- **No PSOs** — `VK_EXT_shader_object`; shaders bound per-draw with `vkCmdBindShadersEXT`; all rasterisation state set via dynamic-state commands each frame
- **Descriptor heap** — `VK_EXT_descriptor_buffer`; descriptors live in a GPU buffer; bound with `vkCmdBindDescriptorBuffersEXT` + `vkCmdSetDescriptorBufferOffsetsEXT`
- **Synchronisation** — `VK_KHR_synchronization2` (`vkCmdPipelineBarrier2`, `vkQueueSubmit2`)
- **Memory** — VMA with `VMA_STATIC_VULKAN_FUNCTIONS 0` / `VMA_DYNAMIC_VULKAN_FUNCTIONS 0`; volk function pointers supplied via `VmaVulkanFunctions`
- **Frame overlap** — 2 frames in flight (`FRAMES_IN_FLIGHT`); each has its own command pool, semaphores, and fence

### Shaders

GLSL compiled to SPIR-V at build time by `glslc`. `.spv` files land in `build/<preset>/shaders/` and are loaded at runtime relative to the working directory. Add new shaders with the `add_shader(GeoClip <file>)` CMake helper.

### Dependency roles

| Library | Role |
|---|---|
| **volk** | Vulkan meta-loader — loads all Vulkan function pointers at runtime. Include `<volk.h>` instead of `<vulkan/vulkan.h>` everywhere. Never link `Vulkan::Vulkan` directly to application targets. |
| **SDL3** | Windowing, input, Vulkan surface creation. Built statically. |
| **ImGui** | Immediate-mode debug UI. Compiled in-tree (no upstream CMake) with the SDL3 + Vulkan backends. Uses `IMGUI_IMPL_VULKAN_USE_VOLK` so its Vulkan backend dispatches through volk. |
| **VMA** | GPU memory allocation (`<vk_mem_alloc.h>`). Header-only at the CMake level; target is `GPUOpen::VulkanMemoryAllocator`. |
| **fastgltf** | glTF 2.0 scene/mesh loading. CMake target is bare `fastgltf` (alias `fastgltf::fastgltf` also works). |
| **glm** | Math (vectors, matrices, quaternions). Header-only; target is `glm::glm`. |

### Adding a new source file

Add it to `target_sources(GeoClip ...)` or a new `add_library` in `CMakeLists.txt`. There is no glob — all sources are listed explicitly.

### Adding a new third-party library

1. `git submodule add <url> third_party/<name>`
2. Add cache-variable overrides (disable tests/examples) then `add_subdirectory` in `CMakeLists.txt`
3. Add the target to `target_link_libraries(GeoClip ...)`

# GeoClip

A minimal Vulkan 1.3 renderer written in C++23, using modern Vulkan extensions exclusively — no render passes, no pipeline state objects, no descriptor pools.

Currently renders the KhronosGroup **DamagedHelmet** glTF model with Blinn-Phong lighting, a D32 depth buffer, and Y-axis rotation.

## Prerequisites

| Requirement | Notes |
|---|---|
| Vulkan 1.3 GPU | Must support `VK_EXT_shader_object` and `VK_EXT_descriptor_buffer` |
| CMake ≥ 3.25 | Required for preset support |
| Ninja | Used by all CMake presets |
| `slangc` | Slang compiler — compiles `.slang` shaders to SPIR-V 1.4 at build time |
| GCC 13+ or Clang 17+ | C++23 required |

## Build

```bash
git submodule update --init --recursive

# Debug
cmake --preset debug
cmake --build build/debug

# Release
cmake --preset release
cmake --build build/release
```

## Run

```bash
# Run from the project root — the glTF asset path is relative
./build/debug/GeoClip
```

## Rendering architecture

| Extension | Effect |
|---|---|
| `VK_KHR_dynamic_rendering` | No render passes or framebuffers — `vkCmdBeginRendering` directly |
| `VK_EXT_shader_object` | No PSOs — shaders bound per-draw; all rasterisation state set dynamically each frame |
| `VK_EXT_descriptor_buffer` | Descriptors live in a GPU buffer instead of descriptor pools |
| `VK_KHR_synchronization2` | `vkCmdPipelineBarrier2` / `vkQueueSubmit2` |
| volk | Vulkan meta-loader — function pointers loaded at runtime; no link against `Vulkan::Vulkan` |

## Dependencies

All dependencies are git submodules under `third_party/`.

| Library | Role |
|---|---|
| [SDL3](https://github.com/libsdl-org/SDL) | Window, input, Vulkan surface |
| [volk](https://github.com/zeux/volk) | Vulkan meta-loader |
| [VulkanMemoryAllocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | GPU memory allocation |
| [glm](https://github.com/g-truc/glm) | Math |
| [fastgltf](https://github.com/spnda/fastgltf) | glTF 2.0 loading |
| [ImGui](https://github.com/ocornut/imgui) | Debug UI (integrated, not yet used) |
| [glTF-Sample-Assets](https://github.com/KhronosGroup/glTF-Sample-Assets) | Test mesh (DamagedHelmet) |

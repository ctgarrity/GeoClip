cmake_minimum_required(VERSION 3.25)
project(GeoClip LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Vulkan REQUIRED)

# SDL3 — static, no tests
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/SDL)

# glm — header-only math library
set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/glm)

# volk — Vulkan meta-loader (loads function pointers at runtime)
add_subdirectory(third_party/volk)

# VMA — header-only memory allocator
add_subdirectory(third_party/VulkanMemoryAllocator)

# FastGLTF
set(FASTGLTF_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(FASTGLTF_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/fastgltf)

# ImGui — no upstream CMake; compiled as a static lib with SDL3+Vulkan backends
add_library(imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_demo.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_sdl3.cpp
    third_party/imgui/backends/imgui_impl_vulkan.cpp
)
target_include_directories(imgui PUBLIC
    third_party/imgui
    third_party/imgui/backends
)
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK)
target_link_libraries(imgui PUBLIC SDL3::SDL3 volk)

# Slang shader compilation
# Each .slang file contains both vertex and fragment entry points and is compiled
# to two separate SPIR-V files: <name>.vert.spv and <name>.frag.spv
find_program(SLANGC slangc REQUIRED)

function(add_slang_shader TARGET SHADER_NAME)
    set(src "${CMAKE_SOURCE_DIR}/shaders/${SHADER_NAME}.slang")
    set(vert_spv "${CMAKE_BINARY_DIR}/shaders/${SHADER_NAME}.vert.spv")
    set(frag_spv "${CMAKE_BINARY_DIR}/shaders/${SHADER_NAME}.frag.spv")
    set(slang_flags -target spirv -profile spirv_1_4 -emit-spirv-directly)
    add_custom_command(
        OUTPUT  ${vert_spv} ${frag_spv}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/shaders"
        COMMAND ${SLANGC} ${src} -entry vertexMain   -stage vertex   ${slang_flags} -o ${vert_spv}
        COMMAND ${SLANGC} ${src} -entry fragmentMain -stage fragment ${slang_flags} -o ${frag_spv}
        DEPENDS ${src}
        VERBATIM
    )
    target_sources(${TARGET} PRIVATE ${vert_spv} ${frag_spv})
endfunction()

function(add_slang_compute_shader TARGET SHADER_NAME)
    set(src      "${CMAKE_SOURCE_DIR}/shaders/${SHADER_NAME}.slang")
    set(comp_spv "${CMAKE_BINARY_DIR}/shaders/${SHADER_NAME}.comp.spv")
    set(slang_flags -target spirv -profile spirv_1_4 -emit-spirv-directly)
    add_custom_command(
        OUTPUT  ${comp_spv}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/shaders"
        COMMAND ${SLANGC} ${src} -entry computeMain -stage compute ${slang_flags} -o ${comp_spv}
        DEPENDS ${src}
        VERBATIM
    )
    target_sources(${TARGET} PRIVATE ${comp_spv})
endfunction()

add_executable(GeoClip src/main.cpp src/engine.cpp src/vma_impl.cpp)
target_link_libraries(GeoClip PRIVATE
    SDL3::SDL3
    glm::glm
    volk
    GPUOpen::VulkanMemoryAllocator
    fastgltf
    imgui
)
target_include_directories(GeoClip PRIVATE src)

add_slang_shader(GeoClip triangle)
add_slang_compute_shader(GeoClip rotate)

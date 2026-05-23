#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <fastgltf/core.hpp>
#include <glm/glm.hpp>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>
#include <vk_mem_alloc.h>
#include <volk.h>

int main()
{
    if (volkInitialize() != VK_SUCCESS)
        return 1;

    if (!SDL_Init(SDL_INIT_VIDEO))
        return 1;

    SDL_Window* window = SDL_CreateWindow("GeoClip", 1280, 720, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!window)
    {
        SDL_Quit();
        return 1;
    }

    bool running = true;
    SDL_Event event;
    while (running)
    {
        while (SDL_PollEvent(&event))
            if (event.type == SDL_EVENT_QUIT)
                running = false;
    }

    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

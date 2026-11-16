// SpeedBreaker launcher. GPL-3.0-or-later (see COPYING).
// The launcher's window: SDL3 + Vulkan + Dear ImGui (as ImGui's SDL3/Vulkan example).
#pragma once
#include "hardware.h"

#include <string>
#include <vector>

struct SDL_Window;

namespace launcher::gfx
{
    // Creates the window and Vulkan (lists every GPU into hw.gpus). False: no Vulkan.
    bool Init(const char* title, int w, int h, Hardware& hw, std::string& error);
    // Begin a frame (false: skip it, the window is minimized).
    bool BeginFrame();
    void EndFrame();
    void Shutdown();
    SDL_Window* Window();
    // The title bar's (and taskbar's) icon: PNGs, the largest first.
    bool SetWindowIcon(const std::vector<std::string>& pngs);
}

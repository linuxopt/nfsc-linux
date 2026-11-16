// SpeedBreaker launcher. GPL-3.0-or-later (see COPYING).
// Follows Dear ImGui's example_sdl3_vulkan (MIT).
#include "gfx.h"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <cstdio>
#include <cstring>
#include <format>
#include <vector>

namespace launcher::gfx
{
    namespace
    {
        SDL_Window* s_window = nullptr;
        VkInstance s_instance = VK_NULL_HANDLE;
        VkPhysicalDevice s_physical = VK_NULL_HANDLE;
        VkDevice s_device = VK_NULL_HANDLE;
        uint32_t s_queueFamily = uint32_t(-1);
        VkQueue s_queue = VK_NULL_HANDLE;
        VkDescriptorPool s_pool = VK_NULL_HANDLE;
        ImGui_ImplVulkanH_Window s_wd;
        uint32_t s_minImages = 2;
        bool s_rebuild = false;

        void Check(VkResult r)
        {
            if (r == VK_SUCCESS)
                return;
            fprintf(stderr, "[launcher] Vulkan error %d\n", int(r));
            if (r < 0)
                abort();
        }
    }

    SDL_Window* Window() { return s_window; }

    bool Init(const char* title, int w, int h, Hardware& hw, std::string& error)
    {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
        {
            error = std::format("SDL: {}", SDL_GetError());
            return false;
        }
        float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
        if (scale <= 0) scale = 1;
        s_window = SDL_CreateWindow(title, int(w * scale), int(h * scale), SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (!s_window)
        {
            error = std::format("window: {}", SDL_GetError());
            return false;
        }
        if (const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(s_window)))
        {
            hw.displayW = mode->w;
            hw.displayH = mode->h;
            hw.refresh = mode->refresh_rate > 0 ? int(mode->refresh_rate + 0.5f) : 60;
        }

        uint32_t extCount = 0;
        const char* const* sdlExt = SDL_Vulkan_GetInstanceExtensions(&extCount);
        std::vector<const char*> exts(sdlExt, sdlExt + extCount);
        VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
        app.pApplicationName = "SpeedBreaker Carbon Launcher";
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        ici.pApplicationInfo = &app;
        ici.enabledExtensionCount = uint32_t(exts.size());
        ici.ppEnabledExtensionNames = exts.data();
        if (vkCreateInstance(&ici, nullptr, &s_instance) != VK_SUCCESS)
        {
            error = "no Vulkan driver (the game needs Vulkan 1.2)";
            return false;
        }

        uint32_t count = 0;
        vkEnumeratePhysicalDevices(s_instance, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(s_instance, &count, devices.data());
        hw.gpus.clear();
        for (uint32_t i = 0; i < count; i++)
        {
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(devices[i], &p);
            VkPhysicalDeviceMemoryProperties m;
            vkGetPhysicalDeviceMemoryProperties(devices[i], &m);
            VkPhysicalDeviceFeatures f;
            vkGetPhysicalDeviceFeatures(devices[i], &f);
            Gpu g;
            g.index = i;
            g.name = p.deviceName;
            g.type = int(p.deviceType);
            g.vendor = p.vendorID;
            g.bc = f.textureCompressionBC;
            for (uint32_t k = 0; k < m.memoryHeapCount; k++)
                if (m.memoryHeaps[k].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                    g.vram = std::max<uint64_t>(g.vram, m.memoryHeaps[k].size);
            hw.gpus.push_back(g);
        }
        Classify(hw);
        // The launcher draws with the integrated GPU when there is one (it
        // leaves the dedicated one cool); any that can present will do.
        s_physical = devices.empty() ? VK_NULL_HANDLE : devices[hw.best >= 0 ? hw.best : 0];
        for (uint32_t i = 0; i < count; i++)
            if (hw.gpus[i].type == 1)
                s_physical = devices[i];

        VkSurfaceKHR surface;
        if (!SDL_Vulkan_CreateSurface(s_window, s_instance, nullptr, &surface))
        {
            error = std::format("surface: {}", SDL_GetError());
            return false;
        }
        auto pickQueue = [&](VkPhysicalDevice dev) {
            uint32_t n = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(dev, &n, nullptr);
            std::vector<VkQueueFamilyProperties> q(n);
            vkGetPhysicalDeviceQueueFamilyProperties(dev, &n, q.data());
            for (uint32_t i = 0; i < n; i++)
            {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, surface, &present);
                if ((q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present)
                    return i;
            }
            return uint32_t(-1);
        };
        s_queueFamily = s_physical ? pickQueue(s_physical) : uint32_t(-1);
        for (uint32_t i = 0; s_queueFamily == uint32_t(-1) && i < count; i++)
        {
            s_physical = devices[i];
            s_queueFamily = pickQueue(s_physical);
        }
        if (s_queueFamily == uint32_t(-1))
        {
            error = "no GPU can draw to this window";
            return false;
        }
        const char* devExt[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        qci.queueFamilyIndex = s_queueFamily;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = 1;
        dci.ppEnabledExtensionNames = devExt;
        Check(vkCreateDevice(s_physical, &dci, nullptr, &s_device));
        vkGetDeviceQueue(s_device, s_queueFamily, 0, &s_queue);
        VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16 };
        VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dpci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        dpci.maxSets = 16;
        dpci.poolSizeCount = 1;
        dpci.pPoolSizes = &poolSize;
        Check(vkCreateDescriptorPool(s_device, &dpci, nullptr, &s_pool));

        s_wd.Surface = surface;
        const VkFormat formats[] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8_UNORM, VK_FORMAT_R8G8B8_UNORM };
        s_wd.SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(s_physical, surface, formats, 4, VK_COLORSPACE_SRGB_NONLINEAR_KHR);
        VkPresentModeKHR modes[] = { VK_PRESENT_MODE_FIFO_KHR };
        s_wd.PresentMode = ImGui_ImplVulkanH_SelectPresentMode(s_physical, surface, modes, 1);
        int fw, fh;
        SDL_GetWindowSizeInPixels(s_window, &fw, &fh);
        ImGui_ImplVulkanH_CreateOrResizeWindow(s_instance, s_physical, s_device, &s_wd, s_queueFamily, nullptr, fw, fh, s_minImages, 0);
        SDL_ShowWindow(s_window);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        io.IniFilename = nullptr;
        ImGui_ImplSDL3_InitForVulkan(s_window);
        ImGui_ImplVulkan_InitInfo ii{};
        ii.ApiVersion = VK_API_VERSION_1_1;
        ii.Instance = s_instance;
        ii.PhysicalDevice = s_physical;
        ii.Device = s_device;
        ii.QueueFamily = s_queueFamily;
        ii.Queue = s_queue;
        ii.DescriptorPool = s_pool;
        ii.MinImageCount = s_minImages;
        ii.ImageCount = s_wd.ImageCount;
        ii.PipelineInfoMain.RenderPass = s_wd.RenderPass;
        ii.PipelineInfoMain.Subpass = 0;
        ii.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        ii.CheckVkResultFn = Check;
        ImGui_ImplVulkan_Init(&ii);
        return true;
    }

    bool BeginFrame()
    {
        if (SDL_GetWindowFlags(s_window) & SDL_WINDOW_MINIMIZED)
            return false;
        int fw, fh;
        SDL_GetWindowSizeInPixels(s_window, &fw, &fh);
        if (fw > 0 && fh > 0 && (s_rebuild || s_wd.Width != fw || s_wd.Height != fh))
        {
            ImGui_ImplVulkan_SetMinImageCount(s_minImages);
            ImGui_ImplVulkanH_CreateOrResizeWindow(s_instance, s_physical, s_device, &s_wd, s_queueFamily, nullptr, fw, fh, s_minImages, 0);
            s_wd.FrameIndex = 0;
            s_rebuild = false;
        }
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        return true;
    }

    void EndFrame()
    {
        ImGui::Render();
        ImDrawData* dd = ImGui::GetDrawData();
        if (dd->DisplaySize.x <= 0 || dd->DisplaySize.y <= 0)
            return;
        s_wd.ClearValue.color = { { 0.06f, 0.06f, 0.08f, 1.0f } };
        VkSemaphore acquired = s_wd.FrameSemaphores[s_wd.SemaphoreIndex].ImageAcquiredSemaphore;
        VkSemaphore done = s_wd.FrameSemaphores[s_wd.SemaphoreIndex].RenderCompleteSemaphore;
        VkResult r = vkAcquireNextImageKHR(s_device, s_wd.Swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &s_wd.FrameIndex);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
            s_rebuild = true;
        if (r == VK_ERROR_OUT_OF_DATE_KHR)
            return;
        ImGui_ImplVulkanH_Frame* fd = &s_wd.Frames[s_wd.FrameIndex];
        Check(vkWaitForFences(s_device, 1, &fd->Fence, VK_TRUE, UINT64_MAX));
        Check(vkResetFences(s_device, 1, &fd->Fence));
        Check(vkResetCommandPool(s_device, fd->CommandPool, 0));
        VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        Check(vkBeginCommandBuffer(fd->CommandBuffer, &bi));
        VkRenderPassBeginInfo rp{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        rp.renderPass = s_wd.RenderPass;
        rp.framebuffer = fd->Framebuffer;
        rp.renderArea.extent.width = uint32_t(s_wd.Width);
        rp.renderArea.extent.height = uint32_t(s_wd.Height);
        rp.clearValueCount = 1;
        rp.pClearValues = &s_wd.ClearValue;
        vkCmdBeginRenderPass(fd->CommandBuffer, &rp, VK_SUBPASS_CONTENTS_INLINE);
        ImGui_ImplVulkan_RenderDrawData(dd, fd->CommandBuffer);
        vkCmdEndRenderPass(fd->CommandBuffer);
        Check(vkEndCommandBuffer(fd->CommandBuffer));
        VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &acquired;
        si.pWaitDstStageMask = &wait;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &fd->CommandBuffer;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &done;
        Check(vkQueueSubmit(s_queue, 1, &si, fd->Fence));
        VkPresentInfoKHR pi{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &done;
        pi.swapchainCount = 1;
        pi.pSwapchains = &s_wd.Swapchain;
        pi.pImageIndices = &s_wd.FrameIndex;
        r = vkQueuePresentKHR(s_queue, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
            s_rebuild = true;
        s_wd.SemaphoreIndex = (s_wd.SemaphoreIndex + 1) % s_wd.SemaphoreCount;
    }

    void Shutdown()
    {
        if (s_device)
            vkDeviceWaitIdle(s_device);
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        ImGui_ImplVulkanH_DestroyWindow(s_instance, s_device, &s_wd, nullptr);
        if (s_pool) vkDestroyDescriptorPool(s_device, s_pool, nullptr);
        if (s_device) vkDestroyDevice(s_device, nullptr);
        if (s_instance) vkDestroyInstance(s_instance, nullptr);
        SDL_DestroyWindow(s_window);
        SDL_Quit();
    }
}

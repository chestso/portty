// Vulkan device ownership + DMA-BUF export for the GTK4 backend.
//
// bloom creates its own VkInstance/VkDevice (with the external-memory
// extensions enabled) and hands them to SDL's "vulkan" 2D renderer. SDL's own
// device does not enable VK_KHR_external_memory_fd / VK_EXT_external_memory_dma_buf
// / VK_EXT_image_drm_format_modifier, and there is no SDL knob to add them to
// the 2D vulkan renderer's device, so we must own device creation.
//
// The render target is an exportable, DRM-format-modifier VkImage that we wrap
// as an SDL render target (SDL renders the gamma-correct scene straight into
// it). Its memory is exported as a DMA-BUF fd for zero-copy display via
// GdkDmabufTexture.

#ifndef GTK4_VULKAN_H
#define GTK4_VULKAN_H

#ifdef HAVE_VULKAN_DMABUF

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

typedef struct
{
    VkInstance instance;
    VkSurfaceKHR surface;
    VkPhysicalDevice phys;
    VkDevice device;
    uint32_t gfx_qf;
    uint32_t present_qf;
    PFN_vkGetMemoryFdKHR vkGetMemoryFdKHR;
    PFN_vkGetImageDrmFormatModifierPropertiesEXT vkGetImageDrmFormatModifierPropertiesEXT;
    // For the post-render dma-buf ownership-release barrier (portty_vk_export_release).
    VkQueue gfx_queue;
    VkCommandPool cmd_pool;
    VkCommandBuffer release_cmd;
    VkFence release_fence;
    // Human-readable GPU + driver, captured at init for the diagnostics report.
    char device_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE]; // VkPhysicalDeviceProperties.deviceName
    // Sized to hold driverName + driverInfo + the " (open source) — " join.
    char driver_desc[VK_MAX_DRIVER_NAME_SIZE + VK_MAX_DRIVER_INFO_SIZE + 32];
    bool driver_libre; // permissively-licensed open-source driver (Mesa = MIT)
    bool ok;
} PorttyVk;

typedef struct
{
    VkImage image;
    VkDeviceMemory memory;
    SDL_Texture *texture; // SDL render target wrapping `image`
    int width;
    int height;
    int dmabuf_fd; // owned; -1 if none
    uint32_t fourcc;
    uint64_t modifier;
    uint32_t stride;
    uint32_t offset;
    bool released; // ownership released to the foreign queue; reacquire before reuse
} PorttyVkTarget;

// Create our own VkInstance + VkDevice (external-memory extensions enabled) for
// `win`, create the window surface, and populate `props` with the
// SDL_PROP_RENDERER_CREATE_VULKAN_* handles. The caller then sets the renderer
// name ("vulkan") + window pointer on `props` and calls
// SDL_CreateRendererWithProperties. Returns false on any failure.
bool portty_vk_init(PorttyVk *vk, SDL_Window *win, SDL_PropertiesID props);

// (Re)create the exportable render target at w x h: an exportable
// DRM-modifier VkImage wrapped as an SDL render target, with its memory
// exported as a DMA-BUF. Destroys any prior contents of `t` first.
bool portty_vk_target_create(PorttyVk *vk, SDL_Renderer *r, int w, int h,
                             PorttyVkTarget *t);
void portty_vk_target_destroy(PorttyVk *vk, PorttyVkTarget *t);

// Wait for all GPU work (SDL's render into the export image) to complete before
// the compositor scans out the DMA-BUF.
void portty_vk_finish(PorttyVk *vk);

// Hand the just-rendered export image off to the external (compositor/GTK)
// consumer: a queue-family-ownership release to VK_QUEUE_FAMILY_FOREIGN_EXT plus
// a transition to VK_IMAGE_LAYOUT_GENERAL, submitted on our graphics queue and
// fence-waited. This is what makes our writes visible to the importing device in
// the linear DMA-BUF (a plain vkDeviceWaitIdle does not). Call after SDL has
// finished rendering into `t->image` and left it in SHADER_READ_ONLY_OPTIMAL
// (i.e. after SDL_SetRenderTarget(NULL) + SDL_FlushRenderer).
void portty_vk_export_release(PorttyVk *vk, PorttyVkTarget *t);

// Reclaim a previously-released target from the foreign importer before SDL
// renders into it again: a queue-family ACQUIRE (FOREIGN -> graphics) plus a
// transition back to SHADER_READ_ONLY_OPTIMAL (the layout SDL still tracks for
// it). Must run before SDL touches the image. Submitted on the graphics queue
// and fence-waited.
void portty_vk_export_acquire(PorttyVk *vk, PorttyVkTarget *t);

void portty_vk_shutdown(PorttyVk *vk);

#endif // HAVE_VULKAN_DMABUF
#endif // GTK4_VULKAN_H

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
    bool ok;
} BloomVk;

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
} BloomVkTarget;

// Create our own VkInstance + VkDevice (external-memory extensions enabled) for
// `win`, create the window surface, and populate `props` with the
// SDL_PROP_RENDERER_CREATE_VULKAN_* handles. The caller then sets the renderer
// name ("vulkan") + window pointer on `props` and calls
// SDL_CreateRendererWithProperties. Returns false on any failure.
bool bloom_vk_init(BloomVk *vk, SDL_Window *win, SDL_PropertiesID props);

// (Re)create the exportable render target at w x h: an exportable
// DRM-modifier VkImage wrapped as an SDL render target, with its memory
// exported as a DMA-BUF. Destroys any prior contents of `t` first.
bool bloom_vk_target_create(BloomVk *vk, SDL_Renderer *r, int w, int h,
                            BloomVkTarget *t);
void bloom_vk_target_destroy(BloomVk *vk, BloomVkTarget *t);

// Wait for all GPU work (SDL's render into the export image) to complete before
// the compositor scans out the DMA-BUF.
void bloom_vk_finish(BloomVk *vk);

void bloom_vk_shutdown(BloomVk *vk);

#endif // HAVE_VULKAN_DMABUF
#endif // GTK4_VULKAN_H

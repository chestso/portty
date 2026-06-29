#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gtk4_vulkan.h"

#ifdef HAVE_VULKAN_DMABUF

#include "common.h"
#include <SDL3/SDL_vulkan.h>
#include <drm_fourcc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// VK_FORMAT_R8G8B8A8_UNORM is memory order R,G,B,A, which is DRM_FORMAT_ABGR8888
// (DRM names channels MSB->LSB of the LE word: A<<24|B<<16|G<<8|R) and
// SDL_PIXELFORMAT_RGBA32. GTK advertises ABGR8888 + LINEAR on this display.
#define PORTTY_VK_IMAGE_FORMAT    VK_FORMAT_R8G8B8A8_UNORM
#define PORTTY_SDL_TEXTURE_FORMAT SDL_PIXELFORMAT_RGBA32
#define PORTTY_DRM_FOURCC         DRM_FORMAT_ABGR8888

static const char *const REQUIRED_DEVICE_EXTS[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
};
#define REQUIRED_DEVICE_EXT_COUNT \
    (sizeof(REQUIRED_DEVICE_EXTS) / sizeof(REQUIRED_DEVICE_EXTS[0]))

static bool phys_has_exts(VkPhysicalDevice pd)
{
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL);
    VkExtensionProperties *e = calloc(n, sizeof(*e));
    if (!e)
        return false;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &n, e);
    bool ok = true;
    for (size_t k = 0; k < REQUIRED_DEVICE_EXT_COUNT; k++) {
        bool found = false;
        for (uint32_t i = 0; i < n; i++)
            if (strcmp(e[i].extensionName, REQUIRED_DEVICE_EXTS[k]) == 0) {
                found = true;
                break;
            }
        if (!found) {
            ok = false;
            break;
        }
    }
    free(e);
    return ok;
}

// Pick a physical device that supports the required extensions plus a graphics
// queue and present to `surface`. Prefer discrete GPUs. Fills *gfx/*pres.
static VkPhysicalDevice pick_physical(VkInstance inst, VkSurfaceKHR surface,
                                      uint32_t *gfx, uint32_t *pres)
{
    uint32_t npd = 0;
    vkEnumeratePhysicalDevices(inst, &npd, NULL);
    if (npd == 0)
        return VK_NULL_HANDLE;
    VkPhysicalDevice *pds = calloc(npd, sizeof(*pds));
    if (!pds)
        return VK_NULL_HANDLE;
    vkEnumeratePhysicalDevices(inst, &npd, pds);

    VkPhysicalDevice chosen = VK_NULL_HANDLE;
    uint32_t chosen_gfx = UINT32_MAX, chosen_pres = UINT32_MAX;
    int chosen_discrete = 0;

    for (uint32_t d = 0; d < npd; d++) {
        if (!phys_has_exts(pds[d]))
            continue;
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pds[d], &nq, NULL);
        VkQueueFamilyProperties *qf = calloc(nq, sizeof(*qf));
        if (!qf)
            continue;
        vkGetPhysicalDeviceQueueFamilyProperties(pds[d], &nq, qf);
        uint32_t g = UINT32_MAX, p = UINT32_MAX;
        for (uint32_t q = 0; q < nq; q++) {
            if ((qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && g == UINT32_MAX)
                g = q;
            VkBool32 ps = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(pds[d], q, surface, &ps);
            if (ps && p == UINT32_MAX)
                p = q;
        }
        free(qf);
        if (g == UINT32_MAX || p == UINT32_MAX)
            continue;

        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pds[d], &props);
        int discrete = (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);
        if (chosen == VK_NULL_HANDLE || (discrete && !chosen_discrete)) {
            chosen = pds[d];
            chosen_gfx = g;
            chosen_pres = p;
            chosen_discrete = discrete;
        }
    }
    free(pds);
    if (chosen != VK_NULL_HANDLE) {
        *gfx = chosen_gfx;
        *pres = chosen_pres;
    }
    return chosen;
}

bool portty_vk_init(PorttyVk *vk, SDL_Window *win, SDL_PropertiesID props)
{
    memset(vk, 0, sizeof(*vk));
    vk->surface = VK_NULL_HANDLE;

    if (!SDL_Vulkan_LoadLibrary(NULL)) {
        vlog("Vulkan: SDL_Vulkan_LoadLibrary failed: %s\n", SDL_GetError());
        return false;
    }

    Uint32 n_iext = 0;
    const char *const *iext = SDL_Vulkan_GetInstanceExtensions(&n_iext);
    if (!iext) {
        vlog("Vulkan: SDL_Vulkan_GetInstanceExtensions failed: %s\n",
             SDL_GetError());
        return false;
    }

    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                              .pApplicationName = "portty",
                              .apiVersion = VK_API_VERSION_1_2 };
    VkInstanceCreateInfo ici = { .sType =
                                     VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                 .pApplicationInfo = &app,
                                 .enabledExtensionCount = n_iext,
                                 .ppEnabledExtensionNames = iext };
    if (vkCreateInstance(&ici, NULL, &vk->instance) != VK_SUCCESS) {
        vlog("Vulkan: vkCreateInstance failed\n");
        return false;
    }

    if (!SDL_Vulkan_CreateSurface(win, vk->instance, NULL, &vk->surface)) {
        vlog("Vulkan: SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
        portty_vk_shutdown(vk);
        return false;
    }

    vk->phys = pick_physical(vk->instance, vk->surface, &vk->gfx_qf,
                             &vk->present_qf);
    if (vk->phys == VK_NULL_HANDLE) {
        vlog("Vulkan: no physical device with required dmabuf extensions\n");
        portty_vk_shutdown(vk);
        return false;
    }
    VkPhysicalDeviceProperties pprops;
    vkGetPhysicalDeviceProperties(vk->phys, &pprops);
    snprintf(vk->device_name, sizeof(vk->device_name), "%s", pprops.deviceName);

    // Driver identity (Vulkan 1.1+ core) so the diagnostics report can tell e.g.
    // the open-source NVK/nouveau stack from the proprietary NVIDIA driver.
    // Classify by string (header-independent: avoids referencing driver-ID enum
    // constants that may be absent in older Vulkan headers).
    VkPhysicalDeviceDriverProperties dprops = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES
    };
    VkPhysicalDeviceProperties2 props2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &dprops
    };
    vkGetPhysicalDeviceProperties2(vk->phys, &props2);
    // Mesa GPU drivers (NVK, RADV, ANV, …) are MIT-licensed — permissive open
    // source. The diagnostics report colours those green.
    vk->driver_libre = strstr(dprops.driverInfo, "Mesa") != NULL ||
                       strstr(dprops.driverName, "Mesa") != NULL;
    const char *origin = NULL;
    if (vk->driver_libre)
        origin = "open source";
    else if (strstr(dprops.driverName, "NVIDIA") || strstr(dprops.driverName, "proprietary"))
        origin = "proprietary";
    if (origin)
        snprintf(vk->driver_desc, sizeof(vk->driver_desc), "%s (%s) — %s",
                 dprops.driverName, origin, dprops.driverInfo);
    else
        snprintf(vk->driver_desc, sizeof(vk->driver_desc), "%s — %s",
                 dprops.driverName, dprops.driverInfo);

    vlog("Vulkan: GPU '%s' driver '%s' gfxQF=%u presQF=%u\n", vk->device_name,
         vk->driver_desc, vk->gfx_qf, vk->present_qf);

    float pri = 1.0f;
    VkDeviceQueueCreateInfo qci[2];
    uint32_t nqci = 0;
    qci[nqci++] = (VkDeviceQueueCreateInfo){
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = vk->gfx_qf,
        .queueCount = 1,
        .pQueuePriorities = &pri
    };
    if (vk->present_qf != vk->gfx_qf)
        qci[nqci++] = (VkDeviceQueueCreateInfo){
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = vk->present_qf,
            .queueCount = 1,
            .pQueuePriorities = &pri
        };

    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = nqci,
        .pQueueCreateInfos = qci,
        .enabledExtensionCount = REQUIRED_DEVICE_EXT_COUNT,
        .ppEnabledExtensionNames = REQUIRED_DEVICE_EXTS
    };
    if (vkCreateDevice(vk->phys, &dci, NULL, &vk->device) != VK_SUCCESS) {
        vlog("Vulkan: vkCreateDevice failed\n");
        portty_vk_shutdown(vk);
        return false;
    }

    vk->vkGetMemoryFdKHR =
        (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(vk->device, "vkGetMemoryFdKHR");
    vk->vkGetImageDrmFormatModifierPropertiesEXT =
        (PFN_vkGetImageDrmFormatModifierPropertiesEXT)vkGetDeviceProcAddr(
            vk->device, "vkGetImageDrmFormatModifierPropertiesEXT");
    if (!vk->vkGetMemoryFdKHR ||
        !vk->vkGetImageDrmFormatModifierPropertiesEXT) {
        vlog("Vulkan: dmabuf export function pointers unresolved\n");
        portty_vk_shutdown(vk);
        return false;
    }

    // Resources for the per-frame dma-buf ownership-release barrier. We submit
    // it on the SAME graphics queue SDL uses (we own the device), sequenced
    // after SDL_FlushRenderer's submit, so no cross-queue contention.
    vkGetDeviceQueue(vk->device, vk->gfx_qf, 0, &vk->gfx_queue);
    VkCommandPoolCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = vk->gfx_qf
    };
    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    };
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (vkCreateCommandPool(vk->device, &cpci, NULL, &vk->cmd_pool) !=
            VK_SUCCESS ||
        (cbai.commandPool = vk->cmd_pool,
         vkAllocateCommandBuffers(vk->device, &cbai, &vk->release_cmd)) !=
            VK_SUCCESS ||
        vkCreateFence(vk->device, &fci, NULL, &vk->release_fence) != VK_SUCCESS) {
        vlog("Vulkan: failed to create export-release command resources\n");
        portty_vk_shutdown(vk);
        return false;
    }

    // Hand our instance/surface/device/queues to SDL's vulkan renderer.
    SDL_SetPointerProperty(props, SDL_PROP_RENDERER_CREATE_VULKAN_INSTANCE_POINTER,
                           vk->instance);
    SDL_SetNumberProperty(props, SDL_PROP_RENDERER_CREATE_VULKAN_SURFACE_NUMBER,
                          (Sint64)(uintptr_t)vk->surface);
    SDL_SetPointerProperty(
        props, SDL_PROP_RENDERER_CREATE_VULKAN_PHYSICAL_DEVICE_POINTER, vk->phys);
    SDL_SetPointerProperty(props, SDL_PROP_RENDERER_CREATE_VULKAN_DEVICE_POINTER,
                           vk->device);
    SDL_SetNumberProperty(
        props, SDL_PROP_RENDERER_CREATE_VULKAN_GRAPHICS_QUEUE_FAMILY_INDEX_NUMBER,
        vk->gfx_qf);
    SDL_SetNumberProperty(
        props, SDL_PROP_RENDERER_CREATE_VULKAN_PRESENT_QUEUE_FAMILY_INDEX_NUMBER,
        vk->present_qf);

    vk->ok = true;
    return true;
}

static uint32_t find_memory_type(VkPhysicalDevice pd, uint32_t type_bits,
                                 VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((type_bits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    // Fall back to any compatible type.
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if (type_bits & (1u << i))
            return i;
    return 0;
}

bool portty_vk_target_create(PorttyVk *vk, SDL_Renderer *r, int w, int h,
                             PorttyVkTarget *t)
{
    portty_vk_target_destroy(vk, t);
    memset(t, 0, sizeof(*t));
    t->dmabuf_fd = -1;
    t->width = w;
    t->height = h;

    uint64_t mods[1] = { DRM_FORMAT_MOD_LINEAR };
    VkImageDrmFormatModifierListCreateInfoEXT modlist = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
        .drmFormatModifierCount = 1,
        .pDrmFormatModifiers = mods
    };
    VkExternalMemoryImageCreateInfo extimg = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &modlist,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
    };
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &extimg,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = PORTTY_VK_IMAGE_FORMAT,
        .extent = { (uint32_t)w, (uint32_t)h, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };
    if (vkCreateImage(vk->device, &ici, NULL, &t->image) != VK_SUCCESS) {
        vlog("Vulkan: vkCreateImage(%dx%d) failed\n", w, h);
        return false;
    }

    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(vk->device, t->image, &mr);
    VkExportMemoryAllocateInfo expinfo = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
    };
    VkMemoryDedicatedAllocateInfo ded = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = &expinfo,
        .image = t->image
    };
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &ded,
        .allocationSize = mr.size,
        .memoryTypeIndex = find_memory_type(vk->phys, mr.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    };
    if (vkAllocateMemory(vk->device, &mai, NULL, &t->memory) != VK_SUCCESS) {
        vlog("Vulkan: vkAllocateMemory failed\n");
        portty_vk_target_destroy(vk, t);
        return false;
    }
    vkBindImageMemory(vk->device, t->image, t->memory, 0);

    // Export the memory as a DMA-BUF fd (persistent; dup'd per frame for GTK).
    VkMemoryGetFdInfoKHR gfi = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .memory = t->memory,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
    };
    if (vk->vkGetMemoryFdKHR(vk->device, &gfi, &t->dmabuf_fd) != VK_SUCCESS ||
        t->dmabuf_fd < 0) {
        vlog("Vulkan: vkGetMemoryFdKHR failed\n");
        portty_vk_target_destroy(vk, t);
        return false;
    }

    VkImageDrmFormatModifierPropertiesEXT dmprops = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT
    };
    vk->vkGetImageDrmFormatModifierPropertiesEXT(vk->device, t->image, &dmprops);
    t->modifier = dmprops.drmFormatModifier;
    t->fourcc = PORTTY_DRM_FOURCC;

    VkImageSubresource sub = { .aspectMask =
                                   VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT };
    VkSubresourceLayout sl;
    vkGetImageSubresourceLayout(vk->device, t->image, &sub, &sl);
    t->stride = (uint32_t)sl.rowPitch;
    t->offset = (uint32_t)sl.offset;

    // Wrap the VkImage as an SDL render target; SDL renders straight into it.
    SDL_PropertiesID tp = SDL_CreateProperties();
    SDL_SetNumberProperty(tp, SDL_PROP_TEXTURE_CREATE_VULKAN_TEXTURE_NUMBER,
                          (Sint64)(uintptr_t)t->image);
    SDL_SetNumberProperty(tp, SDL_PROP_TEXTURE_CREATE_VULKAN_LAYOUT_NUMBER,
                          VK_IMAGE_LAYOUT_UNDEFINED);
    SDL_SetNumberProperty(tp, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER,
                          SDL_TEXTUREACCESS_TARGET);
    SDL_SetNumberProperty(tp, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER,
                          PORTTY_SDL_TEXTURE_FORMAT);
    SDL_SetNumberProperty(tp, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, w);
    SDL_SetNumberProperty(tp, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, h);
    t->texture = SDL_CreateTextureWithProperties(r, tp);
    SDL_DestroyProperties(tp);
    if (!t->texture) {
        vlog("Vulkan: wrap VkImage as SDL target failed: %s\n", SDL_GetError());
        portty_vk_target_destroy(vk, t);
        return false;
    }

    vlog("Vulkan dmabuf target %dx%d: fourcc=0x%x mod=0x%llx stride=%u "
         "offset=%u fd=%d\n",
         w, h, t->fourcc, (unsigned long long)t->modifier, t->stride, t->offset,
         t->dmabuf_fd);
    return true;
}

void portty_vk_target_destroy(PorttyVk *vk, PorttyVkTarget *t)
{
    if (t->texture) {
        SDL_DestroyTexture(t->texture);
        t->texture = NULL;
    }
    if (t->dmabuf_fd >= 0) {
        close(t->dmabuf_fd);
        t->dmabuf_fd = -1;
    }
    if (vk->device) {
        if (t->image) {
            vkDestroyImage(vk->device, t->image, NULL);
            t->image = VK_NULL_HANDLE;
        }
        if (t->memory) {
            vkFreeMemory(vk->device, t->memory, NULL);
            t->memory = VK_NULL_HANDLE;
        }
    }
}

void portty_vk_finish(PorttyVk *vk)
{
    if (vk->device)
        vkDeviceWaitIdle(vk->device);
}

// Submit a single queue-family-ownership image barrier on the graphics queue
// and fence-wait for it.
static void submit_ownership_barrier(PorttyVk *vk, VkImage image,
                                     VkImageLayout old_layout,
                                     VkImageLayout new_layout, uint32_t src_qf,
                                     uint32_t dst_qf,
                                     VkAccessFlags src_access,
                                     VkAccessFlags dst_access)
{
    if (!vk->device || !vk->release_cmd || image == VK_NULL_HANDLE)
        return;

    vkResetCommandBuffer(vk->release_cmd, 0);
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };
    vkBeginCommandBuffer(vk->release_cmd, &bi);
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = src_qf,
        .dstQueueFamilyIndex = dst_qf,
        .image = image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
    };
    vkCmdPipelineBarrier(vk->release_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL,
                         1, &barrier);
    vkEndCommandBuffer(vk->release_cmd);

    vkResetFences(vk->device, 1, &vk->release_fence);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .commandBufferCount = 1,
                        .pCommandBuffers = &vk->release_cmd };
    vkQueueSubmit(vk->gfx_queue, 1, &si, vk->release_fence);
    vkWaitForFences(vk->device, 1, &vk->release_fence, VK_TRUE, UINT64_MAX);
}

void portty_vk_export_release(PorttyVk *vk, PorttyVkTarget *t)
{
    // Release ownership to the foreign (compositor/GTK importer) queue family
    // and move to GENERAL (the layout GTK imports dma-bufs as). SDL leaves the
    // target in SHADER_READ_ONLY_OPTIMAL after SDL_SetRenderTarget(NULL)+flush.
    // NOTE: the actual cross-device coherence is forced by the 1x1 readback the
    // caller does while the target is still bound (see platform_gtk4.c); this
    // barrier only handles the ownership/layout handoff for scanout.
    submit_ownership_barrier(vk, t->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_IMAGE_LAYOUT_GENERAL, vk->gfx_qf,
                             VK_QUEUE_FAMILY_FOREIGN_EXT,
                             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0);
}

void portty_vk_export_acquire(PorttyVk *vk, PorttyVkTarget *t)
{
    // Reclaim from the foreign importer and restore the layout SDL still tracks
    // (SHADER_READ_ONLY_OPTIMAL) so SDL can render into it again.
    submit_ownership_barrier(vk, t->image, VK_IMAGE_LAYOUT_GENERAL,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_QUEUE_FAMILY_FOREIGN_EXT, vk->gfx_qf, 0,
                             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
}

void portty_vk_shutdown(PorttyVk *vk)
{
    if (vk->device) {
        vkDeviceWaitIdle(vk->device);
        if (vk->release_fence) {
            vkDestroyFence(vk->device, vk->release_fence, NULL);
            vk->release_fence = VK_NULL_HANDLE;
        }
        if (vk->cmd_pool) {
            vkDestroyCommandPool(vk->device, vk->cmd_pool, NULL);
            vk->cmd_pool = VK_NULL_HANDLE;
        }
        vkDestroyDevice(vk->device, NULL);
        vk->device = VK_NULL_HANDLE;
    }
    if (vk->surface && vk->instance) {
        vkDestroySurfaceKHR(vk->instance, vk->surface, NULL);
        vk->surface = VK_NULL_HANDLE;
    }
    if (vk->instance) {
        vkDestroyInstance(vk->instance, NULL);
        vk->instance = VK_NULL_HANDLE;
    }
    vk->ok = false;
}

#endif // HAVE_VULKAN_DMABUF

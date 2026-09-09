#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/poll.h>
#include <assert.h>
#include <dlfcn.h>

#include <vulkan/vulkan.h>
#include <vulkan/vk_icd.h>
#include <vulkan/vk_layer.h>
#include <libdrm/drm_fourcc.h>

#ifdef VK_USE_PLATFORM_XCB_KHR
#include <X11/xshmfence.h>
#include <xcb/xcb.h>
#include <xcb/dri3.h>
#include <xcb/present.h>
#include <xcb/shm.h>
#include <errno.h>
#include <sys/syscall.h>
#else
#include <wayland-client.h>
#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "linux-explicit-synchronization-unstable-v1-client-protocol.h"
#endif

#ifndef VK_LAYER_EXPORT
#define VK_LAYER_EXPORT __attribute__((visibility("default")))
#endif

#define vkGetMemoryFdKHR(x...) _vkGetMemoryFdKHR(x)
#define vkImportFenceFdKHR(x...) _vkImportFenceFdKHR(x)
#define vkImportSemaphoreFdKHR(x...) _vkImportSemaphoreFdKHR(x)
#define vkGetSemaphoreFdKHR(x...) _vkGetSemaphoreFdKHR(x)
static PFN_vkGetMemoryFdKHR _vkGetMemoryFdKHR;
static PFN_vkImportFenceFdKHR _vkImportFenceFdKHR;
static PFN_vkImportSemaphoreFdKHR _vkImportSemaphoreFdKHR;
static PFN_vkGetSemaphoreFdKHR _vkGetSemaphoreFdKHR;

// see weston's simple-dmabuf-egl.c example
// TODO:
// - don't assume server will support modifiers
// - vsync
// - don't ignore allocation callbacks

#define ALIGN_POT(x, pot_align) (((x) + (pot_align) - 1) & ~((pot_align) - 1))
#define ARRAY_SIZE(x) (sizeof(x)/sizeof(*(x)))

struct buffer {
   VkImage image;
   uint32_t present_idx;
   int busy;
#ifdef VK_USE_PLATFORM_XCB_KHR
   xcb_pixmap_t pixmap;
   struct xshmfence *shm_fence;
   uint32_t sync_fence;
#else
   struct wl_buffer *buffer;
   struct zwp_linux_buffer_release_v1 *buffer_release;
   int release_fence_fd;
#endif
};

struct VkSurfaceKHR_T {
#ifdef VK_USE_PLATFORM_XCB_KHR
   VkIcdSurfaceXcb icd;
#else
   VkIcdSurfaceWayland icd;
   struct zwp_linux_dmabuf_v1 *dmabuf;
   struct zwp_linux_explicit_synchronization_v1 *explicit_sync;
   struct zwp_linux_surface_synchronization_v1 *sync;
   /* proxy wrapper around the event queue */
   struct wl_display *wl_display_wrapper;
   struct wl_event_queue *queue;
#endif
};

struct VkSwapchainKHR_T {
   VkSurfaceKHR surface;
   uint32_t image_count;
   uint32_t present_idx;
   VkDeviceMemory memory;
   struct buffer image[4];
};

#ifdef VK_USE_PLATFORM_WAYLAND_KHR
static void
buffer_fenced_release(void *data, struct zwp_linux_buffer_release_v1 *release, int32_t fence)
{
   struct buffer *buffer = data;

   assert(release == buffer->buffer_release);
   assert(buffer->release_fence_fd == -1);

   buffer->busy = 0;
   buffer->release_fence_fd = fence;
   zwp_linux_buffer_release_v1_destroy(buffer->buffer_release);
   buffer->buffer_release = NULL;
}

static void
buffer_immediate_release(void *data, struct zwp_linux_buffer_release_v1 *release)
{
   struct buffer *buffer = data;

   assert(release == buffer->buffer_release);
   assert(buffer->release_fence_fd == -1);

   buffer->busy = 0;
   zwp_linux_buffer_release_v1_destroy(buffer->buffer_release);
   buffer->buffer_release = NULL;
}

static const struct zwp_linux_buffer_release_v1_listener buffer_release_listener = {
       buffer_fenced_release,
       buffer_immediate_release,
};
#endif

static VkDevice _device;

VkResult
vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *info, const VkAllocationCallbacks *alloc, VkSwapchainKHR *p_swapchain)
{
   VkResult result;

   VkSwapchainKHR swapchain = malloc(sizeof(*swapchain));
   if (!swapchain)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   assert((info->flags & ~VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR) == 0);
   assert(info->minImageCount >= 2 && info->minImageCount <= 4);
   assert(info->imageColorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
   assert(info->imageArrayLayers == 1);
   assert(info->preTransform == VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR);
   assert(info->compositeAlpha == VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR);
   assert(info->presentMode == VK_PRESENT_MODE_FIFO_KHR);

   swapchain->surface = info->surface;
   swapchain->image_count = info->minImageCount;
   swapchain->present_idx = 0;

   // XXX need to make sure server supports UBWC modifier..
   VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = info->imageFormat,
      .extent = {
         .width = info->imageExtent.width,
         .height = info->imageExtent.height,
         .depth = 1,
      },
      .mipLevels = 1,
      .arrayLayers = info->imageArrayLayers,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_LINEAR,
      .usage = info->imageUsage,
      .sharingMode = info->imageSharingMode,
      .queueFamilyIndexCount = info->queueFamilyIndexCount,
      .pQueueFamilyIndices = info->pQueueFamilyIndices,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      // TODO: pass through mutable info if VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR is set
   };

   for (uint32_t i = 0; i < swapchain->image_count; i++) {
      result = vkCreateImage(device, &image_info, alloc, &swapchain->image[i].image);
      if (result != VK_SUCCESS) {
         swapchain->image_count = i;
         goto fail;
      }
   }

   VkMemoryRequirements reqs;
   vkGetImageMemoryRequirements(device, swapchain->image[0].image, &reqs);

   // XXX apply alignment to reqs.size?

   result = vkAllocateMemory(device, &(VkMemoryAllocateInfo) {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = reqs.size * swapchain->image_count,
      .memoryTypeIndex = 0, // XXX choose a memory type
      .pNext = &(VkExportMemoryAllocateInfo) {
         .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
         .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
      },
   }, alloc, &swapchain->memory);
   if (result != VK_SUCCESS)
      goto fail;

   int dma_buf_fd;
   result = vkGetMemoryFdKHR(device, &(VkMemoryGetFdInfoKHR) {
      .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
      .memory = swapchain->memory,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   }, &dma_buf_fd);
   assert(result == VK_SUCCESS);

   // note: vkBindImageMemory will never fail
   for (uint32_t i = 0; i < swapchain->image_count; i++)
      vkBindImageMemory(device, swapchain->image[i].image, swapchain->memory, reqs.size * i);


   // layout/format of the images:
   uint32_t pitch, drm_format;
   uint64_t modifier = DRM_FORMAT_MOD_LINEAR;//QCOM_COMPRESSED;
   VkSubresourceLayout layout;
   vkGetImageSubresourceLayout(device, swapchain->image[0].image, &(VkImageSubresource) {VK_IMAGE_ASPECT_COLOR_BIT}, &layout);
   pitch = layout.rowPitch;
   switch (info->imageFormat) {
   case VK_FORMAT_B8G8R8A8_SRGB:
   case VK_FORMAT_B8G8R8A8_UNORM:
      drm_format = DRM_FORMAT_XRGB8888; // alpha?
      break;
   case VK_FORMAT_R8G8B8A8_SRGB:
   case VK_FORMAT_R8G8B8A8_UNORM:
      drm_format = DRM_FORMAT_XBGR8888; // alpha?
      break;
   default:
      assert(!"unsupported swapchain format");
      break;
   }

   for (uint32_t i = 0; i < swapchain->image_count; i++) {
      struct buffer *image = &swapchain->image[i];
#ifdef VK_USE_PLATFORM_XCB_KHR
      xcb_connection_t *conn = swapchain->surface->icd.connection;
      xcb_void_cookie_t cookie;

      image->pixmap = xcb_generate_id(conn);

      assert(drm_format == DRM_FORMAT_XRGB8888); // 24 depth 32 bpp format
      cookie = xcb_dri3_pixmap_from_buffers_checked(
         swapchain->surface->icd.connection,
         image->pixmap,
         swapchain->surface->icd.window,
         1, // num_planes
         info->imageExtent.width,
         info->imageExtent.height,
         pitch, reqs.size * i, 0, 0, 0, 0, 0, 0, // row_pitches/offsets
         24, 32, // depth, bpp
         modifier, (int[4]) { dup(dma_buf_fd), -1, -1, -1 }
      );
      xcb_discard_reply(conn, cookie.sequence);

      int fence_fd = xshmfence_alloc_shm();
      assert(fence_fd >= 0);

      image->shm_fence = xshmfence_map_shm(fence_fd);
      assert(image->shm_fence);

      image->sync_fence = xcb_generate_id(conn);
      xcb_dri3_fence_from_fd(conn, image->pixmap, image->sync_fence, false, fence_fd);
      // _checked?

      xshmfence_trigger(image->shm_fence);
#else
      struct zwp_linux_buffer_params_v1 *params =
         zwp_linux_dmabuf_v1_create_params(swapchain->surface->dmabuf);
      assert(params);

      zwp_linux_buffer_params_v1_add(params, dma_buf_fd, 0, reqs.size * i, pitch, modifier >> 32, modifier & 0xffffffff);

      image->buffer = zwp_linux_buffer_params_v1_create_immed(params,
         info->imageExtent.width, info->imageExtent.height, drm_format, 0);
      assert(image->buffer);
      zwp_linux_buffer_params_v1_destroy(params);
      image->buffer_release = NULL;
      image->release_fence_fd = -1;
#endif

      image->busy = 0;
      image->present_idx = 0;
   }

   close(dma_buf_fd);

   *p_swapchain = swapchain;
   return VK_SUCCESS;
fail:
   for (uint32_t i = 0; i < swapchain->image_count; i++)
      vkDestroyImage(device, swapchain->image[i].image, alloc);
   free(swapchain); // XXX
   return result;
}

void
vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *alloc)
{
   for (uint32_t i = 0; i < swapchain->image_count; i++) {
      struct buffer *image = &swapchain->image[i];
      vkDestroyImage(device, image->image, alloc);

#ifdef VK_USE_PLATFORM_XCB_KHR
      xcb_connection_t *conn = swapchain->surface->icd.connection;
      xcb_void_cookie_t cookie;

      cookie = xcb_sync_destroy_fence(conn, image->sync_fence);
      xcb_discard_reply(conn, cookie.sequence);
      xshmfence_unmap_shm(image->shm_fence);

      cookie = xcb_free_pixmap(conn, image->pixmap);
      xcb_discard_reply(conn, cookie.sequence);
#else
      if (image->buffer_release)
         zwp_linux_buffer_release_v1_destroy(image->buffer_release);
      wl_buffer_destroy(image->buffer);
      if (image->release_fence_fd >= 0)
         close(image->release_fence_fd);
#endif
   }
   vkFreeMemory(device, swapchain->memory, alloc);

   free(swapchain);
}

VkResult
vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t *count, VkImage *images)
{
   if (!images) {
      *count = swapchain->image_count;
      return VK_SUCCESS;
   }

   for (uint32_t i = 0; i < *count && i < swapchain->image_count; i++)
      images[i] = swapchain->image[i].image;

   if (*count < swapchain->image_count)
      return VK_INCOMPLETE;
   *count = swapchain->image_count;
   return VK_SUCCESS;
}

VkResult
vkAcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR *info, uint32_t *image_index)
{
   VkSwapchainKHR swapchain = info->swapchain;
   VkResult result;

   uint32_t present_idx = ~0u, idx;

#ifdef VK_USE_PLATFORM_XCB_KHR
   for (uint32_t i = 0; i < swapchain->image_count; i++) {
      struct buffer *b = &swapchain->image[i];
      if (b->busy)
         continue;

      /* choose the earliest presented available image */
      if (present_idx > b->present_idx) {
         present_idx = b->present_idx;
         idx = i;
      }
   }

   if (present_idx == ~0u) {
      printf("TODO: blocking acquire when all images are in acquired state?\n");
      return VK_NOT_READY;
   }

   struct buffer *image = &swapchain->image[idx];

   if (info->timeout) {
      // note: this can wait longer than timeout, but should be relatively short
      xshmfence_await(image->shm_fence);
   } else {
      if (!xshmfence_query(image->shm_fence))
         return VK_NOT_READY;
   }

   // no explicit sync, fill output fences with signaled state

   if (info->fence) {
      result = vkImportFenceFdKHR(device, &(VkImportFenceFdInfoKHR) {
         .sType = VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR,
         .fence = info->fence,
         .flags = VK_FENCE_IMPORT_TEMPORARY_BIT,
         .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,
         .fd = -1,
      });
      assert(result == VK_SUCCESS);
   }
   if (info->semaphore) {
      result = vkImportSemaphoreFdKHR(device, &(VkImportSemaphoreFdInfoKHR) {
         .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
         .semaphore = info->semaphore,
         .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
         .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
         .fd = -1,
      });
      assert(result == VK_SUCCESS);
   }

#else

again:;
   int ret = wl_display_dispatch_queue_pending(swapchain->surface->icd.display, swapchain->surface->queue);
   if (ret < 0)
      return VK_ERROR_OUT_OF_DATE_KHR; // TODO: is this right?

   for (uint32_t i = 0; i < swapchain->image_count; i++) {
      struct buffer *b = &swapchain->image[i];
      if (b->busy)
         continue;

      /* choose the earliest presented available image */
      if (present_idx > b->present_idx) {
         present_idx = b->present_idx;
         idx = i;
      }
   }

   if (present_idx == ~0u) {
      if (info->timeout) {
         // XXX
         //printf("TODO: wait for timeout in vkAcquireNextImage\n");
         int ret = wl_display_dispatch_queue(swapchain->surface->icd.display, swapchain->surface->queue);
         if (ret < 0)
            return VK_ERROR_OUT_OF_DATE_KHR;
         goto again;
      }
      return VK_NOT_READY;
   }
   struct buffer *image = &swapchain->image[idx];

   // note: can avoid the extra dup() syscalls

   if (info->fence) {
      result = vkImportFenceFdKHR(device, &(VkImportFenceFdInfoKHR) {
         .sType = VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR,
         .fence = info->fence,
         .flags = VK_FENCE_IMPORT_TEMPORARY_BIT,
         .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,
         .fd = dup(image->release_fence_fd),
      });
      assert(result == VK_SUCCESS);
   }
   if (info->semaphore) {
      result = vkImportSemaphoreFdKHR(device, &(VkImportSemaphoreFdInfoKHR) {
         .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
         .semaphore = info->semaphore,
         .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
         .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
         .fd = dup(image->release_fence_fd),
      });
      assert(result == VK_SUCCESS);
   }

   close(image->release_fence_fd);
   image->release_fence_fd = -1;

#endif
   image->busy = 1;

   *image_index = idx;
   return VK_SUCCESS;
}

VkResult
vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *info)
{
   VkDevice device = _device;

   VkResult result = VK_SUCCESS;

   //static int x;
   //printf("vkQueuePresentKHR %d\n", x++);

   // XXX
   assert(info->swapchainCount == 1);
   assert(info->waitSemaphoreCount <= 1);

   int sync_fd = -1;
   if (info->waitSemaphoreCount) {
      result = vkGetSemaphoreFdKHR(device, &(VkSemaphoreGetFdInfoKHR) {
         .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
         .semaphore = info->pWaitSemaphores[0],
         .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
      }, &sync_fd);
      if (result != VK_SUCCESS)
         goto ret;
   }

   VkSwapchainKHR swapchain = info->pSwapchains[0];
   uint32_t idx = info->pImageIndices[0];
   struct buffer *image = &swapchain->image[idx];

   // missing: damage with VkPresentRegionKHR

   image->present_idx = swapchain->present_idx++;

#ifdef VK_USE_PLATFORM_XCB_KHR
   if (sync_fd != -1) {
      // no explicit sync support for x11 - need to block
      struct pollfd fds = { .fd = sync_fd, .events = POLLIN };
      int ret;
      do {
         ret = syscall(__NR_ppoll, &fds, 1, NULL, NULL);
      } while (ret == -EINTR || ret == -EAGAIN);
      assert(ret == 1);
      close(sync_fd);
   }


   image->busy = 0;

   xcb_void_cookie_t cookie = xcb_present_pixmap_checked(
      swapchain->surface->icd.connection,
      swapchain->surface->icd.window,
      image->pixmap,
      image->present_idx,
      XCB_NONE,
      XCB_NONE, // image->update_area
      0,
      0,
      XCB_NONE,
      XCB_NONE,
      image->sync_fence,
      XCB_PRESENT_OPTION_ASYNC | XCB_PRESENT_OPTION_SUBOPTIMAL, // async for any mode except FIFO with xwayland?
      0, // target_msc
      0, 0, 0, NULL);
   xcb_generic_error_t *error = xcb_request_check(swapchain->surface->icd.connection, cookie);
   assert(!error);
#else
   struct wl_surface *surface = swapchain->surface->icd.surface;

   if (sync_fd != -1) {
      zwp_linux_surface_synchronization_v1_set_acquire_fence(swapchain->surface->sync, sync_fd);
      close(sync_fd);
   }

   image->buffer_release = zwp_linux_surface_synchronization_v1_get_release(swapchain->surface->sync);
   assert(image->buffer_release);
   zwp_linux_buffer_release_v1_add_listener(image->buffer_release, &buffer_release_listener, image);

   wl_surface_attach(surface, image->buffer, 0, 0);
   wl_surface_damage(surface, 0, 0, INT32_MAX, INT32_MAX);
   wl_surface_commit(surface);
#endif

ret:
   if (info->pResults)
      info->pResults[0] = result;
   return result;
}

#ifdef VK_USE_PLATFORM_XCB_KHR

VkResult
vkCreateXcbSurfaceKHR(VkInstance instance, const VkXcbSurfaceCreateInfoKHR *info, const VkAllocationCallbacks *alloc, VkSurfaceKHR *p_surface)
{
   VkSurfaceKHR surface = malloc(sizeof(*surface));
   assert(surface);

   surface->icd.base.platform = VK_ICD_WSI_PLATFORM_XCB;
   surface->icd.connection = info->connection;
   surface->icd.window = info->window;

   *p_surface = surface;
   return VK_SUCCESS;
}

#include <X11/Xlib-xcb.h>

VkResult
vkCreateXlibSurfaceKHR(VkInstance instance, const VkXlibSurfaceCreateInfoKHR *info, const VkAllocationCallbacks *alloc, VkSurfaceKHR *p_surface)
{
   VkSurfaceKHR surface = malloc(sizeof(*surface));
   assert(surface);

   surface->icd.base.platform = VK_ICD_WSI_PLATFORM_XLIB;
   surface->icd.connection = XGetXCBConnection(info->dpy); // not really correct to convert to XCB here, but should be OK?
   surface->icd.window = (xcb_window_t) info->window;

   *p_surface = surface;
   return VK_SUCCESS;
}

#else

static void
dmabuf_modifiers(void *data, struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf,
		 uint32_t format, uint32_t modifier_hi, uint32_t modifier_lo)
{
   /* TOOO */
}

static void
dmabuf_format(void *data, struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf, uint32_t format)
{
   /* XXX: deprecated */
}

static const struct zwp_linux_dmabuf_v1_listener dmabuf_listener = {
   dmabuf_format,
   dmabuf_modifiers
};

static void
registry_handle_global(void *data, struct wl_registry *registry, uint32_t id, const char *interface, uint32_t version)
{
   VkSurfaceKHR surface = data;

   if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0 && version >= 3) {
      surface->dmabuf = wl_registry_bind(registry, id, &zwp_linux_dmabuf_v1_interface, 3);
      zwp_linux_dmabuf_v1_add_listener(surface->dmabuf, &dmabuf_listener, surface);
   } else if (strcmp(interface, zwp_linux_explicit_synchronization_v1_interface.name) == 0) {
      surface->explicit_sync = wl_registry_bind(registry, id, &zwp_linux_explicit_synchronization_v1_interface, 1);
   }
}

static void
registry_handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
   registry_handle_global,
   registry_handle_global_remove
};

VkResult
vkCreateWaylandSurfaceKHR(VkInstance instance, const VkWaylandSurfaceCreateInfoKHR *info, const VkAllocationCallbacks *alloc, VkSurfaceKHR *p_surface)
{
   VkSurfaceKHR surface = malloc(sizeof(*surface));
   assert(surface);

   surface->icd.base.platform = VK_ICD_WSI_PLATFORM_WAYLAND;
   surface->icd.display = info->display;
   surface->icd.surface = info->surface;

   surface->queue = wl_display_create_queue(info->display);
   assert(surface->queue); // VK_ERROR_OUT_OF_HOST_MEMORY

   surface->wl_display_wrapper = wl_proxy_create_wrapper(info->display);
   assert(surface->wl_display_wrapper); // VK_ERROR_OUT_OF_HOST_MEMORY

   wl_proxy_set_queue((struct wl_proxy *) surface->wl_display_wrapper, surface->queue);

   struct wl_registry *registry = wl_display_get_registry(surface->wl_display_wrapper);
   assert(registry); // VK_ERROR_OUT_OF_HOST_MEMORY

   wl_registry_add_listener(registry, &registry_listener, surface);

   surface->dmabuf = NULL;
   surface->explicit_sync = NULL;
   wl_display_roundtrip_queue(info->display, surface->queue);
   assert(surface->dmabuf && surface->explicit_sync);

   // missing: query supported formats and modifiers

   wl_registry_destroy(registry);

   surface->sync = zwp_linux_explicit_synchronization_v1_get_synchronization(surface->explicit_sync, info->surface);
   assert(surface->sync);

   *p_surface = surface;
   return VK_SUCCESS;
}

#endif

void
vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks *alloc)
{
   printf("TODO: vkDestroySurfaceKHR\n");
   free(surface);
}

// queries

VkResult
vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice pdev, uint32_t queue_family_index, VkSurfaceKHR surface, VkBool32 *p_supported)
{
   *p_supported = VK_TRUE;
   return VK_SUCCESS;
}

VkResult
vkGetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice pdev, const VkPhysicalDeviceSurfaceInfo2KHR *info, VkSurfaceCapabilities2KHR *caps)
{
   caps->surfaceCapabilities = (VkSurfaceCapabilitiesKHR) {
      .minImageCount = 2,
      .maxImageCount = 4,
      .currentExtent = { 0xFFFFFFFF, 0xFFFFFFFF },
      .minImageExtent = { 1, 1 },
      .maxImageExtent = { 8192, 8192 }, // XXX
      .maxImageArrayLayers = 1,
      .supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
      .currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
      .supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
      .supportedUsageFlags = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                             VK_IMAGE_USAGE_SAMPLED_BIT |
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                             VK_IMAGE_USAGE_STORAGE_BIT |
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
   };
   return VK_SUCCESS;
}

VkResult
vkGetPhysicalDeviceSurfaceFormats2KHR(VkPhysicalDevice pdev, const VkPhysicalDeviceSurfaceInfo2KHR *info, uint32_t *count, VkSurfaceFormat2KHR *formats)
{

// missing: support more formats based on compositor support
#ifdef VK_USE_PLATFORM_XCB_KHR
   static const VkFormat fmt_list[] = {
      VK_FORMAT_B8G8R8A8_SRGB,
      VK_FORMAT_B8G8R8A8_UNORM,
   };
#else
   static const VkFormat fmt_list[] = {
      VK_FORMAT_R8G8B8A8_SRGB,
      VK_FORMAT_R8G8B8A8_UNORM,
      VK_FORMAT_B8G8R8A8_SRGB,
      VK_FORMAT_B8G8R8A8_UNORM,
   };
#endif

   if (!formats) {
      *count = ARRAY_SIZE(fmt_list);
      return VK_SUCCESS;
   }

   for (uint32_t i = 0; i < *count && i < ARRAY_SIZE(fmt_list); i++) {
      formats[i].surfaceFormat.format = fmt_list[i];
      formats[i].surfaceFormat.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
   }

   if (*count < ARRAY_SIZE(fmt_list))
      return VK_INCOMPLETE;

   *count = ARRAY_SIZE(fmt_list);
   return VK_SUCCESS;
}

VkResult
vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice pdev, VkSurfaceKHR surface, uint32_t *count, VkPresentModeKHR *modes)
{
   if (!modes) {
      *count = 1;
      return VK_SUCCESS;
   }

   if (*count == 0)
      return VK_INCOMPLETE;

   modes[0] = VK_PRESENT_MODE_FIFO_KHR;
   *count = 1;
   return VK_SUCCESS;
}

VkResult
vkGetDeviceGroupPresentCapabilitiesKHR(VkDevice device, VkDeviceGroupPresentCapabilitiesKHR *caps)
{
   memset(caps->presentMask, 0, sizeof(caps->presentMask));
   caps->presentMask[0] = 0x1;
   caps->modes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
   return VK_SUCCESS;
}

VkResult
vkGetDeviceGroupSurfacePresentModesKHR(VkDevice device, VkSurfaceKHR surface, VkDeviceGroupPresentModeFlagsKHR *modes)
{
   *modes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
   return VK_SUCCESS;
}

VkResult
vkGetPhysicalDevicePresentRectanglesKHR(VkPhysicalDevice pdev, VkSurfaceKHR surface, uint32_t *count, VkRect2D *rects)
{
   if (!rects) {
      *count = 1;
      return VK_SUCCESS;
   }

   if (*count == 0)
      return VK_INCOMPLETE;

   rects[0] = (VkRect2D) { .extent = { 8192, 8192} }; // XXX
   *count = 1;
   return VK_SUCCESS;
}

#ifdef VK_USE_PLATFORM_XCB_KHR

VkBool32 vkGetPhysicalDeviceXcbPresentationSupportKHR(VkPhysicalDevice pdev, uint32_t queue_family_index, xcb_connection_t *connection, xcb_visualid_t visual_id)
{
   // check if compatible?
   return VK_TRUE;
}

VkBool32 vkGetPhysicalDeviceXlibPresentationSupportKHR(VkPhysicalDevice pdev, uint32_t queue_family_index, Display *dpy, VisualID visual_id)
{
   // check if compatible?
   return VK_TRUE;
}

#else

VkBool32 vkGetPhysicalDeviceWaylandPresentationSupportKHR(VkPhysicalDevice pdev, uint32_t queue_family_index, struct wl_display *display)
{
   // check if wl_display is compatible?
   return VK_TRUE;
}

#endif

// legacy functions

VkResult
vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t *image_index)
{
   return vkAcquireNextImage2KHR(device, &(VkAcquireNextImageInfoKHR) {
      .sType = VK_STRUCTURE_TYPE_ACQUIRE_NEXT_IMAGE_INFO_KHR,
      .swapchain = swapchain,
      .timeout = timeout,
      .semaphore = semaphore,
      .fence = fence,
   }, image_index);
}

VkResult
vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice pdev, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR *caps)
{
   VkSurfaceCapabilities2KHR caps2;
   caps2.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR;
   caps2.pNext = NULL;
   VkPhysicalDeviceSurfaceInfo2KHR info = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR,
      .surface = surface,
   };
   VkResult result = vkGetPhysicalDeviceSurfaceCapabilities2KHR(pdev, &info, &caps2);
   *caps = caps2.surfaceCapabilities;
   return result;
}

VkResult
vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice pdev, VkSurfaceKHR surface, uint32_t *count, VkSurfaceFormatKHR *formats)
{
   VkPhysicalDeviceSurfaceInfo2KHR info = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR,
      .surface = surface,
   };
   if (!formats)
      return vkGetPhysicalDeviceSurfaceFormats2KHR(pdev, &info, count, NULL);

   VkSurfaceFormat2KHR formats2[*count];
   for (uint32_t i = 0; i < *count; i++) {
      formats2[i].sType = VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR;
      formats2[i].pNext = NULL;
   }
   VkResult result = vkGetPhysicalDeviceSurfaceFormats2KHR(pdev, &info, count, formats2);
   if (!(result == VK_SUCCESS || result == VK_INCOMPLETE))
      return result;
   for (uint32_t i = 0; i < *count; i++)
      formats[i] = formats2[i].surfaceFormat;
   return result;
}

// layer stuff:

static PFN_vkGetInstanceProcAddr _vkGetInstanceProcAddr;
static PFN_vkEnumerateDeviceExtensionProperties _vkEnumerateDeviceExtensionProperties;

static PFN_vkGetDeviceProcAddr _vkGetDeviceProcAddr;

static VkExtensionProperties instance_extensions[] = {
#ifdef VK_USE_PLATFORM_XCB_KHR
   {VK_KHR_XCB_SURFACE_EXTENSION_NAME, VK_KHR_XCB_SURFACE_SPEC_VERSION},
   {VK_KHR_XLIB_SURFACE_EXTENSION_NAME, VK_KHR_XLIB_SURFACE_SPEC_VERSION},
#else
   {VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME, VK_KHR_WAYLAND_SURFACE_SPEC_VERSION},
#endif
};
static VkExtensionProperties device_extensions[] = {};

VkResult
vkEnumerateInstanceLayerProperties(uint32_t *p_count, VkLayerProperties *prop)
{
   if (p_count)
      *p_count = 1;

   if (prop) {
      strcpy(prop->layerName, "VK_LAYER_WSI");
      strcpy(prop->description, "VK_LAYER_WSI");
      prop->implementationVersion = 1;
      prop->specVersion = VK_API_VERSION_1_0;
   }
   return VK_SUCCESS;
}

VkResult
vkEnumerateDeviceLayerProperties(VkPhysicalDevice pdev, uint32_t *p_count, VkLayerProperties *prop)
{
   return vkEnumerateInstanceLayerProperties(p_count, prop);
}

VkResult
vkEnumerateInstanceExtensionProperties(const char *name, uint32_t *p_count, VkExtensionProperties *prop)
{
   if (!name || strcmp(name, "VK_LAYER_WSI"))
      return VK_ERROR_LAYER_NOT_PRESENT;

   uint32_t count = ARRAY_SIZE(instance_extensions);

   if (!prop) {
      *p_count = count;
      return VK_SUCCESS;
   }

   VkResult result = VK_INCOMPLETE;
   if (*p_count >= count) {
      *p_count = count;
      result = VK_SUCCESS;
   }

   memcpy(prop, instance_extensions, *p_count * sizeof(*prop));
   return result;
}

VkResult
vkEnumerateDeviceExtensionProperties(VkPhysicalDevice pdev, const char *name, uint32_t *p_count, VkExtensionProperties *prop)
{
   if (!name || strcmp(name, "VK_LAYER_WSI"))
      return _vkEnumerateDeviceExtensionProperties(pdev, name, p_count, prop);

   uint32_t count = ARRAY_SIZE(device_extensions);

   if (!prop) {
      *p_count = count;
      return VK_SUCCESS;
   }

   VkResult result = VK_INCOMPLETE;
   if (*p_count >= count) {
      *p_count = count;
      result = VK_SUCCESS;
   }

   memcpy(prop, device_extensions, *p_count * sizeof(*prop));
   return result;
}

VkResult
vkCreateInstance(const VkInstanceCreateInfo *info, const VkAllocationCallbacks *alloc, VkInstance *p_instance)
{
   VkLayerInstanceCreateInfo *layerCreateInfo = (VkLayerInstanceCreateInfo *)info->pNext;

   // step through the chain of pNext until we get to the link info
   while (layerCreateInfo && (layerCreateInfo->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO ||
                              layerCreateInfo->function != VK_LAYER_LINK_INFO)) {
      layerCreateInfo = (VkLayerInstanceCreateInfo *)layerCreateInfo->pNext;
   }

   if (layerCreateInfo == NULL) {
      // No loader instance create info
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   PFN_vkGetInstanceProcAddr gpa = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
   // move chain on for next layer
   layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

#if 0
   /* filter out extensions from create info */
   const char *new_exts[64];
   VkInstanceCreateInfo new_info = *info;
   new_info.enabledExtensionCount = 0;
   new_info.ppEnabledExtensionNames = new_exts;
   for (uint32_t i = 0; i < info->enabledExtensionCount; i++) {
      if (!strcmp(info->ppEnabledExtensionNames[i], "VK_KHR_xcb_surface") ||
          !strcmp(info->ppEnabledExtensionNames[i], "VK_KHR_xlib_surface") ||
          !strcmp(info->ppEnabledExtensionNames[i], "VK_KHR_wayland_surface"))
          continue;
      new_exts[new_info.enabledExtensionCount++] = info->ppEnabledExtensionNames[i];
   }
#endif

   /* */
   PFN_vkCreateInstance vkCreateInstance = (void*) gpa(VK_NULL_HANDLE, "vkCreateInstance");
   VkResult result = vkCreateInstance(info, alloc, p_instance);
   if (result != VK_SUCCESS)
      return result;

   _vkGetInstanceProcAddr = (void*) gpa(*p_instance, "vkGetInstanceProcAddr");
   _vkEnumerateDeviceExtensionProperties = (void*) gpa(*p_instance, "vkEnumerateDeviceExtensionProperties");
   return result;
}

VkResult
vkCreateDevice(VkPhysicalDevice pdev, const VkDeviceCreateInfo *info, const VkAllocationCallbacks *alloc, VkDevice *p_device)
{
  VkLayerDeviceCreateInfo *layerCreateInfo = (VkLayerDeviceCreateInfo *)info->pNext;

  // step through the chain of pNext until we get to the link info
  while(layerCreateInfo && (layerCreateInfo->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO ||
                            layerCreateInfo->function != VK_LAYER_LINK_INFO))
  {
    layerCreateInfo = (VkLayerDeviceCreateInfo *)layerCreateInfo->pNext;
  }

  if(layerCreateInfo == NULL)
  {
    // No loader instance create info
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  PFN_vkGetInstanceProcAddr gipa = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  PFN_vkGetDeviceProcAddr gdpa = layerCreateInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;
  // move chain on for next layer
  layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

  PFN_vkCreateDevice createFunc = (PFN_vkCreateDevice)gipa(VK_NULL_HANDLE, "vkCreateDevice");

   // insert dependencies in createinfo
   VkDeviceCreateInfo new_info = *info;
   const char *new_exts[64];
   memcpy(new_exts, info->ppEnabledExtensionNames, info->enabledExtensionCount * sizeof(new_exts[0]));
   new_info.ppEnabledExtensionNames = new_exts;
#define ADD_EXTENSION(EXT) ({ \
   bool found = false; \
   for (uint32_t i = 0; i < info->enabledExtensionCount; i++) { \
      if (!strcmp(info->ppEnabledExtensionNames[i], EXT)) { \
         found = true; \
         break; \
      } \
   } \
   if (!found) \
      new_exts[new_info.enabledExtensionCount++] = EXT; \
})
   ADD_EXTENSION(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
   ADD_EXTENSION(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
   ADD_EXTENSION(VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME);
   ADD_EXTENSION(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);

  VkResult ret = createFunc(pdev, &new_info, alloc, p_device);
  if (ret != VK_SUCCESS)
     return ret;

  _vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)gdpa(*p_device, "vkGetDeviceProcAddr");
  _vkGetMemoryFdKHR = (void*)gdpa(*p_device, "vkGetMemoryFdKHR");
  _vkImportFenceFdKHR = (void*)gdpa(*p_device, "vkImportFenceFdKHR");
  _vkImportSemaphoreFdKHR = (void*)gdpa(*p_device, "vkImportSemaphoreFdKHR");
  _vkGetSemaphoreFdKHR = (void*)gdpa(*p_device, "vkGetSemaphoreFdKHR");

  return VK_SUCCESS;
}

#define FUNCTION(x) if (!strcmp(name, #x)) return (void*) x;

VK_LAYER_EXPORT PFN_vkVoidFunction vkGetDeviceProcAddr(VkDevice device, const char *name)
{
   // device functions
   FUNCTION(vkGetDeviceProcAddr);
   FUNCTION(vkEnumerateDeviceLayerProperties);
   FUNCTION(vkEnumerateDeviceExtensionProperties);
   FUNCTION(vkCreateDevice);

   FUNCTION(vkCreateSwapchainKHR);
   FUNCTION(vkDestroySwapchainKHR);
   FUNCTION(vkGetSwapchainImagesKHR);
   FUNCTION(vkAcquireNextImage2KHR);
   FUNCTION(vkQueuePresentKHR);

   FUNCTION(vkGetPhysicalDeviceSurfaceSupportKHR);
   FUNCTION(vkGetPhysicalDeviceSurfaceCapabilities2KHR);
   FUNCTION(vkGetPhysicalDeviceSurfaceFormats2KHR);
   FUNCTION(vkGetPhysicalDeviceSurfacePresentModesKHR);
   FUNCTION(vkGetDeviceGroupPresentCapabilitiesKHR);
   FUNCTION(vkGetDeviceGroupSurfacePresentModesKHR);
   FUNCTION(vkGetPhysicalDevicePresentRectanglesKHR);
#ifdef VK_USE_PLATFORM_XCB_KHR
   FUNCTION(vkGetPhysicalDeviceXcbPresentationSupportKHR);
   FUNCTION(vkGetPhysicalDeviceXlibPresentationSupportKHR);
#else
   FUNCTION(vkGetPhysicalDeviceWaylandPresentationSupportKHR);
#endif
   FUNCTION(vkAcquireNextImageKHR);
   FUNCTION(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
   FUNCTION(vkGetPhysicalDeviceSurfaceFormatsKHR);

   return _vkGetDeviceProcAddr(device, name);
}

VK_LAYER_EXPORT PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance, const char *name)
{
   // instance functions
   FUNCTION(vkGetInstanceProcAddr);
   FUNCTION(vkEnumerateInstanceLayerProperties);
   FUNCTION(vkEnumerateInstanceExtensionProperties);
   FUNCTION(vkCreateInstance);

#ifdef VK_USE_PLATFORM_XCB_KHR
   FUNCTION(vkCreateXcbSurfaceKHR);
   FUNCTION(vkCreateXlibSurfaceKHR);
#else
   FUNCTION(vkCreateWaylandSurfaceKHR);
#endif
   FUNCTION(vkDestroySurfaceKHR);

   // device functions
   FUNCTION(vkGetDeviceProcAddr);
   FUNCTION(vkEnumerateDeviceLayerProperties);
   FUNCTION(vkEnumerateDeviceExtensionProperties);
   FUNCTION(vkCreateDevice);

   FUNCTION(vkCreateSwapchainKHR);
   FUNCTION(vkDestroySwapchainKHR);
   FUNCTION(vkGetSwapchainImagesKHR);
   FUNCTION(vkAcquireNextImage2KHR);
   FUNCTION(vkQueuePresentKHR);

   FUNCTION(vkGetPhysicalDeviceSurfaceSupportKHR);
   FUNCTION(vkGetPhysicalDeviceSurfaceCapabilities2KHR);
   FUNCTION(vkGetPhysicalDeviceSurfaceFormats2KHR);
   FUNCTION(vkGetPhysicalDeviceSurfacePresentModesKHR);
   FUNCTION(vkGetDeviceGroupPresentCapabilitiesKHR);
   FUNCTION(vkGetDeviceGroupSurfacePresentModesKHR);
   FUNCTION(vkGetPhysicalDevicePresentRectanglesKHR);
#ifdef VK_USE_PLATFORM_XCB_KHR
   FUNCTION(vkGetPhysicalDeviceXcbPresentationSupportKHR);
   FUNCTION(vkGetPhysicalDeviceXlibPresentationSupportKHR);
#else
   FUNCTION(vkGetPhysicalDeviceWaylandPresentationSupportKHR);
#endif
   FUNCTION(vkAcquireNextImageKHR);
   FUNCTION(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
   FUNCTION(vkGetPhysicalDeviceSurfaceFormatsKHR);

   return _vkGetInstanceProcAddr(instance, name);
}

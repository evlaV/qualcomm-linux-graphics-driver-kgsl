#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <libdrm/msm_drm.h>
#include <gbm.h>

// msm_drm.h in libdrm is out of date:
#define MSM_BO_CACHED_COHERENT 0x080000
#define MSM_INFO_GET_OFFSET 0
#define drm_msm_gem_info drm_msm_gem_info2
struct drm_msm_gem_info {
	__u32 handle;         /* in */
	__u32 info;           /* in - one of MSM_INFO_* */
	__u64 value;          /* in or out */
	__u32 len;            /* in or out */
	__u32 pad;
};

struct gbm_bo {
   uint32_t handle;
   int fd;
   uint32_t format;
   uint32_t width;
   uint32_t height;
   uint32_t meta_size;
   uint32_t size;
   //void *map;
   struct gbm_device *gbm_dev;
};

struct gbm_device {
   int fd;
   uint32_t bo_free_count;
   uint8_t bo_free_idx[256];
   struct gbm_bo bo[256];
};

#define DIV_ROUND_UP( A, B )  ( ((A) + (B) - 1) / (B) )
#define ALIGN_POT(x, pot_align) (((x) + (pot_align) - 1) & ~((pot_align) - 1))

#define GBM_ERROR_NONE 0x0

#define GBM_PERFORM_CPU_MAP_FOR_BO                  0x6
#define GBM_PERFORM_GET_BO_SIZE                     0x8
#define GBM_PERFORM_GET_UBWC_STATUS                 0x19
#define GBM_PERFORM_GET_RGB_DATA_ADDRESS            0x20
#define GBM_PERFORM_SET_GPU_ADDR_FOR_BO             0x21
#define GBM_PERFORM_GET_METADATA_ION_FD             0x24
#define GBM_PERFORM_GET_BO_ALIGNED_WIDTH            0x27
#define GBM_PERFORM_GET_BO_ALIGNED_HEIGHT           0x28
#define GBM_PERFORM_GET_RENDER_DEVICE_NAME          0x36

#if 0
int gbM_perform(int operation, ... )
{
   va_list args;

   va_start(args, operation);

   switch (operation) {
   case GBM_PERFORM_CPU_MAP_FOR_BO: {
      struct gbm_bo *bo = va_arg(args, struct gbm_bo *);
      void **map_addr_handle = va_arg(args,void **);
      *map_addr_handle = bo->map;
   } break;
   case GBM_PERFORM_GET_BO_SIZE: {
      struct gbm_bo *bo = va_arg(args, struct gbm_bo *);
      size_t* size = va_arg(args,size_t*);
      *size = bo->size;
   } break;
   case GBM_PERFORM_GET_UBWC_STATUS: {
      struct gbm_bo *bo = va_arg(args, struct gbm_bo *);
      int *ubwc_status = va_arg(args,int *);
      *ubwc_status = bo->meta_size != 0;
   } break;
   case GBM_PERFORM_GET_RGB_DATA_ADDRESS: {
      struct gbm_bo *bo = va_arg(args, struct gbm_bo *);
      void **rgb_data = va_arg(args, void **);
      *rgb_data = bo->map + bo->meta_size;
   } break;
   case GBM_PERFORM_SET_GPU_ADDR_FOR_BO: {
      struct gbm_bo *bo = va_arg(args, struct gbm_bo *);
      uint64_t gpu_addr = va_arg(args,uint64_t);
      printf("gbm_perform: ignoring set gpu_addr to %lx\n", gpu_addr);
   } break;
   case GBM_PERFORM_GET_METADATA_ION_FD: {
      struct gbm_bo *bo = va_arg(args, struct gbm_bo *);
      int *metadata_fd = va_arg(args,int *);
      *metadata_fd = bo->fd;
   } break;
   case GBM_PERFORM_GET_BO_ALIGNED_WIDTH: {
      struct gbm_bo *bo = va_arg(args, struct gbm_bo *);
      uint32_t *width = va_arg(args, uint32_t *);
      *width = ALIGN_POT(bo->width, 64);
   } break;
   case GBM_PERFORM_GET_BO_ALIGNED_HEIGHT: {
      struct gbm_bo *bo = va_arg(args, struct gbm_bo *);
      uint32_t *height = va_arg(args, uint32_t *);
      *height = ALIGN_POT(bo->height, 16);
   } break;
   case GBM_PERFORM_GET_RENDER_DEVICE_NAME: {
      char *render_dev_name = va_arg(args,char *);
      strcpy(render_dev_name, "/dev/kgsl-3d0");
   } break;
   default:
      printf("gbm_perform %x\n", operation);
      assert(0);
   }
   return GBM_ERROR_NONE;
}
#endif

int gbM_perform(int operation, ... )
{
   va_list args;

   va_start(args, operation);

   assert(operation == GBM_PERFORM_GET_RENDER_DEVICE_NAME);

   char *render_dev_name = va_arg(args,char *);
   strcpy(render_dev_name, "/dev/kgsl-3d0");

   return GBM_ERROR_NONE;
}

struct gbm_device * gbM_create_device(int fd)
{
   struct gbm_device *d = malloc(sizeof(*d));
   d->fd = fd;
   d->bo_free_count = 256;
   for (int i = 0; i < 256; i++)
      d->bo_free_idx[i] = 255-i;
   return d;
}

// missing locks for these
static struct gbm_bo* new_bo(struct gbm_device *gbm_dev)
{
   if (gbm_dev->bo_free_count == 0)
      return NULL;
   struct gbm_bo *bo = &gbm_dev->bo[gbm_dev->bo_free_idx[--gbm_dev->bo_free_count]];
}

static void free_bo(struct gbm_device *gbm_dev, struct gbm_bo *bo)
{
   gbm_dev->bo_free_idx[gbm_dev->bo_free_count++] = bo - gbm_dev->bo;
}

int gbM_device_get_fd(struct gbm_device *d)
{
   return d->fd;
}


void gbM_device_destroy(struct gbm_device *d)
{
   close(d->fd);
   free(d);
}

#define GBM_BO_USAGE_UBWC_ALIGNED_QTI     0x00100000
#define GBM_BO_USAGE_HW_RENDERING_QTI     0x02000000

#define GBM_FORMAT_RAW8                   0x50000004

struct gbm_bo *
gbM_bo_create(struct gbm_device *gbm_dev, uint32_t width, uint32_t height, uint32_t format, uint32_t usage)
{
   assert(format == GBM_FORMAT_RAW8);
   assert(usage == (GBM_BO_USE_RENDERING|GBM_BO_USAGE_HW_RENDERING_QTI|GBM_BO_USAGE_UBWC_ALIGNED_QTI));
   assert(height == 1);

   struct gbm_bo *bo = new_bo(gbm_dev);
   assert(bo);

   bo->format = format;
   bo->width = width;
   bo->height = height;
   bo->meta_size = 0;
   bo->size = width;
   bo->gbm_dev = gbm_dev;

   struct drm_msm_gem_new req = {
      .size = bo->size,
      .flags = MSM_BO_CACHED_COHERENT,
   };

   int ret = ioctl(gbm_dev->fd, DRM_IOCTL_MSM_GEM_NEW, &req);
   if (ret)
      return NULL;

   struct drm_prime_handle handle = {
      .handle = req.handle,
      .flags = DRM_RDWR | DRM_CLOEXEC,
      .fd = -1,
   };
   ret = ioctl(gbm_dev->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &handle);
   assert(!ret);

#if 0
    struct drm_msm_gem_info req2 = {
      .handle = req.handle,
      .info = MSM_INFO_GET_OFFSET,
   };

   ret = ioctl(gbm_dev->fd, DRM_IOCTL_MSM_GEM_INFO, &req2);
   assert(!ret);
#endif

   bo->handle = req.handle;
   bo->fd = handle.fd;
   //bo->map = mmap(0, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED, gbm_dev->fd, req2.value);
   //assert(bo->map != MAP_FAILED);

   return bo;
}

void gbM_bo_destroy(struct gbm_bo *bo)
{
   close(bo->fd);
   //munmap(bo->map, bo->size);
   ioctl(bo->gbm_dev->fd, DRM_IOCTL_GEM_CLOSE, &(struct drm_gem_close) { .handle = bo->handle });
   free_bo(bo->gbm_dev, bo);
}


int gbM_bo_get_fd(struct gbm_bo *bo)
{
   // this is supposed to return a new file descriptor, but qcom's implementation is broken?
   return bo->fd;
}

unsigned int
gbM_bo_get_format(struct gbm_bo *bo)
{
   return bo->format;
}

unsigned int
gbM_bo_get_width(struct gbm_bo *bo)
{
   return bo->width;
}

unsigned int
gbM_bo_get_height(struct gbm_bo *bo)
{
   return bo->height;
}

unsigned int
gbM_bo_get_stride(struct gbm_bo *bo)
{
   return ALIGN_POT(bo->width, 64) * 4;
}

union gbm_bo_handle
gbM_bo_get_handle(struct gbm_bo *bo)
{
   //assert(0);
   // driver does nothing with this?
   return (union gbm_bo_handle) {.u32=10000000};
}

struct gbm_bo *
gbM_bo_import(struct gbm_device *gbm_dev, uint32_t type, void *buffer, uint32_t usage)
{
   assert(type == GBM_BO_IMPORT_FD);
   struct gbm_import_fd_data *info = buffer;

   assert(info->format == GBM_FORMAT_RAW8);

   struct gbm_bo *bo = new_bo(gbm_dev);
   assert(bo);

   // driver does a useless re-import for exportable memory?

   bo->fd = dup3(bo->fd, -1, O_CLOEXEC);
   bo->format = info->format;
   bo->width = 0;
   bo->height = 1;
   bo->meta_size = 0;
   bo->size = 0;
   bo->gbm_dev = gbm_dev;

   return bo;
}

struct gbm_bo *
gbM_bo_create_with_modifiers(struct gbm_device *gbm, uint32_t width, uint32_t height, uint32_t format, const uint64_t *modifiers, const unsigned int count)
{
   assert(0);
}



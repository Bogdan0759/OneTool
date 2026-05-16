#include "i915.h"

#include "../drm/drm_internal.h"

#include <drm/i915_drm.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define SRAPI_I915_MI_BATCH_BUFFER_END 0x05000000u
#define SRAPI_I915_XY_COLOR_BLT ((2u << 29) | (0x50u << 22))
#define SRAPI_I915_BLT_WRITE_ALPHA (1u << 21)
#define SRAPI_I915_BLT_WRITE_RGB (1u << 20)

static int get_param(int fd, int param, int *out) {
    drm_i915_getparam_t gp;
    int value = 0;

    memset(&gp, 0, sizeof(gp));
    gp.param = param;
    gp.value = &value;
    if (srapi_drm_ioctl(fd, DRM_IOCTL_I915_GETPARAM, &gp) != 0) {
        return 0;
    }

    *out = value;
    return 1;
}

static int query_driver_name(int fd, char *name, size_t name_size) {
    struct drm_version version;

    if (name == NULL || name_size == 0) {
        return 0;
    }
    memset(name, 0, name_size);
    memset(&version, 0, sizeof(version));
    version.name = name;
    version.name_len = name_size - 1;

    if (srapi_drm_ioctl(fd, DRM_IOCTL_VERSION, &version) != 0) {
        return 0;
    }
    name[name_size - 1] = '\0';
    return 1;
}

srapi_result_t srapi_i915_query_fd(int fd, const char *path, srapi_i915_probe_t *out) {
    char driver[32];
    int chipset_id = 0;

    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (fd < 0 || out == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }

    if (!query_driver_name(fd, driver, sizeof(driver))) {
        srapi_set_error("i915: DRM_IOCTL_VERSION failed on %s: %s",
                        path != NULL ? path : "fd", strerror(errno));
        return SRAPI_ERROR;
    }
    if (strcmp(driver, "i915") != 0) {
        return SRAPI_ERROR_UNSUPPORTED;
    }

    if (!get_param(fd, I915_PARAM_CHIPSET_ID, &chipset_id)) {
        srapi_set_error("i915: GETPARAM CHIPSET_ID failed on %s: %s",
                        path != NULL ? path : "fd", strerror(errno));
        return SRAPI_ERROR_UNSUPPORTED;
    }

    out->available = 1;
    if (path != NULL) {
        snprintf(out->path, sizeof(out->path), "%s", path);
    }
    out->chipset_id = (uint32_t)chipset_id;
    get_param(fd, I915_PARAM_HAS_GEM, &out->has_gem);
    get_param(fd, I915_PARAM_HAS_EXECBUF2, &out->has_execbuf2);
    get_param(fd, I915_PARAM_HAS_BLT, &out->has_blt);
    get_param(fd, I915_PARAM_HAS_EXEC_FENCE, &out->has_exec_fence);
    get_param(fd, I915_PARAM_CS_TIMESTAMP_FREQUENCY, &out->cs_timestamp_frequency);

    srapi_debugf("i915 probe %s chipset=0x%x gem=%d execbuf2=%d blt=%d fence=%d cs_ts=%d",
                 out->path[0] != '\0' ? out->path : "fd",
                 out->chipset_id,
                 out->has_gem,
                 out->has_execbuf2,
                 out->has_blt,
                 out->has_exec_fence,
                 out->cs_timestamp_frequency);
    return SRAPI_OK;
}

srapi_result_t srapi_i915_probe_path(const char *path, srapi_i915_probe_t *out) {
    int fd;
    srapi_result_t r;

    if (path == NULL || path[0] == '\0' || out == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }

    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        srapi_set_error("i915: open %s failed: %s", path, strerror(errno));
        return SRAPI_ERROR;
    }

    r = srapi_i915_query_fd(fd, path, out);
    close(fd);
    if (r == SRAPI_ERROR_UNSUPPORTED) {
        srapi_set_error("i915: %s is not an i915 DRM node", path);
    }
    return r;
}

srapi_result_t srapi_i915_probe_any(srapi_i915_probe_t *out) {
    char path[64];

    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (out == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }

    for (int i = 128; i < 136; i++) {
        snprintf(path, sizeof(path), "/dev/dri/renderD%d", i);
        if (srapi_i915_probe_path(path, out) == SRAPI_OK) {
            return SRAPI_OK;
        }
    }
    for (int i = 0; i < 8; i++) {
        snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        if (srapi_i915_probe_path(path, out) == SRAPI_OK) {
            return SRAPI_OK;
        }
    }

    srapi_set_error("i915: no usable i915 DRM node found");
    return SRAPI_ERROR_UNSUPPORTED;
}

srapi_result_t srapi_i915_create_buffer(
    srapi_device_t *device,
    const srapi_buffer_desc_t *desc,
    srapi_buffer_t **out
) {
    struct drm_i915_gem_create create;
    struct drm_i915_gem_mmap mmap_arg;
    srapi_buffer_t *buffer;

    if (out != NULL) {
        *out = NULL;
    }
    if (device == NULL || desc == NULL || out == NULL || desc->size == 0 || device->fd < 0) {
        return SRAPI_ERROR_BAD_ARG;
    }

    memset(&create, 0, sizeof(create));
    create.size = desc->size;
    if (srapi_drm_ioctl(device->fd, DRM_IOCTL_I915_GEM_CREATE, &create) != 0) {
        srapi_set_error("i915: GEM_CREATE size=%zu failed on %s: %s",
                        desc->size, device->path, strerror(errno));
        return SRAPI_ERROR_UNSUPPORTED;
    }

    buffer = calloc(1, sizeof(*buffer));
    if (buffer == NULL) {
        srapi_i915_destroy_gem(device->fd, create.handle);
        return SRAPI_ERROR_OOM;
    }

    memset(&mmap_arg, 0, sizeof(mmap_arg));
    mmap_arg.handle = create.handle;
    mmap_arg.size = create.size;
    if (srapi_drm_ioctl(device->fd, DRM_IOCTL_I915_GEM_MMAP, &mmap_arg) != 0) {
        srapi_set_error("i915: GEM_MMAP handle=%u size=%llu failed: %s",
                        create.handle, (unsigned long long)create.size, strerror(errno));
        free(buffer);
        srapi_i915_destroy_gem(device->fd, create.handle);
        return SRAPI_ERROR;
    }

    buffer->device = device;
    buffer->backend = SRAPI_BACKEND_GPU;
    buffer->size = desc->size;
    buffer->usage = desc->usage;
    buffer->data = (void *)(uintptr_t)mmap_arg.addr_ptr;
    buffer->gpu_handle = create.handle;
    buffer->gpu_size = create.size;
    buffer->gpu_memory = 1;
    if (desc->initial_data != NULL) {
        memcpy(buffer->data, desc->initial_data, desc->size);
    }

    *out = buffer;
    srapi_debugf("i915 buffer create path=%s handle=%u size=%zu alloc=%llu usage=0x%x",
                 device->path,
                 buffer->gpu_handle,
                 buffer->size,
                 (unsigned long long)buffer->gpu_size,
                 buffer->usage);
    return SRAPI_OK;
}

static srapi_result_t create_gem_object(
    srapi_device_t *device,
    uint64_t size,
    uint32_t *handle,
    uint64_t *alloc_size,
    void **map
) {
    struct drm_i915_gem_create create;
    struct drm_i915_gem_mmap mmap_arg;

    if (device == NULL || handle == NULL || alloc_size == NULL || map == NULL ||
        size == 0 || device->fd < 0) {
        return SRAPI_ERROR_BAD_ARG;
    }

    memset(&create, 0, sizeof(create));
    create.size = size;
    if (srapi_drm_ioctl(device->fd, DRM_IOCTL_I915_GEM_CREATE, &create) != 0) {
        srapi_set_error("i915: GEM_CREATE size=%llu failed on %s: %s",
                        (unsigned long long)size, device->path, strerror(errno));
        return SRAPI_ERROR_UNSUPPORTED;
    }

    memset(&mmap_arg, 0, sizeof(mmap_arg));
    mmap_arg.handle = create.handle;
    mmap_arg.size = create.size;
    if (srapi_drm_ioctl(device->fd, DRM_IOCTL_I915_GEM_MMAP, &mmap_arg) != 0) {
        srapi_set_error("i915: GEM_MMAP handle=%u size=%llu failed: %s",
                        create.handle, (unsigned long long)create.size, strerror(errno));
        srapi_i915_destroy_gem(device->fd, create.handle);
        return SRAPI_ERROR;
    }

    *handle = create.handle;
    *alloc_size = create.size;
    *map = (void *)(uintptr_t)mmap_arg.addr_ptr;
    return SRAPI_OK;
}

srapi_result_t srapi_i915_create_image(
    srapi_device_t *device,
    const srapi_image_desc_t *desc,
    srapi_image_t **out
) {
    srapi_image_t *image;
    uint64_t row_bytes;
    uint64_t size;
    uint32_t handle = 0;
    uint64_t alloc_size = 0;
    void *map = NULL;
    srapi_result_t r;

    if (out != NULL) {
        *out = NULL;
    }
    if (device == NULL || desc == NULL || out == NULL ||
        desc->width == 0 || desc->height == 0 || device->fd < 0) {
        return SRAPI_ERROR_BAD_ARG;
    }
    if (desc->tiling != SRAPI_IMAGE_LINEAR && desc->tiling != SRAPI_IMAGE_OPTIMAL) {
        return SRAPI_ERROR_BAD_ARG;
    }
    if ((uint64_t)desc->width > UINT32_MAX / sizeof(uint32_t)) {
        return SRAPI_ERROR_OVERFLOW;
    }
    row_bytes = (uint64_t)desc->width * sizeof(uint32_t);
    if (desc->height > UINT64_MAX / row_bytes || row_bytes * desc->height > SIZE_MAX) {
        return SRAPI_ERROR_OVERFLOW;
    }
    size = row_bytes * desc->height;

    r = create_gem_object(device, size, &handle, &alloc_size, &map);
    if (r != SRAPI_OK) {
        return r;
    }

    image = calloc(1, sizeof(*image));
    if (image == NULL) {
        munmap(map, alloc_size);
        srapi_i915_destroy_gem(device->fd, handle);
        return SRAPI_ERROR_OOM;
    }

    image->device = device;
    image->backend = SRAPI_BACKEND_GPU;
    image->width = desc->width;
    image->height = desc->height;
    image->pitch = (uint32_t)row_bytes;
    image->tiling = desc->tiling;
    image->usage = desc->usage;
    image->data = map;
    image->gpu_handle = handle;
    image->gpu_size = alloc_size;
    image->gpu_memory = 1;
    if (desc->initial_pixels != NULL) {
        memcpy(image->data, desc->initial_pixels, (size_t)size);
    }

    *out = image;
    srapi_debugf("i915 image create path=%s handle=%u %ux%u pitch=%u alloc=%llu usage=0x%x",
                 device->path,
                 image->gpu_handle,
                 image->width,
                 image->height,
                 image->pitch,
                 (unsigned long long)image->gpu_size,
                 image->usage);
    return SRAPI_OK;
}

srapi_result_t srapi_i915_submit_noop(srapi_device_t *device) {
    static const uint32_t batch_words[2] = {
        SRAPI_I915_MI_BATCH_BUFFER_END,
        0x00000000u,
    };
    srapi_buffer_t *batch = NULL;
    struct drm_i915_gem_exec_object2 obj;
    struct drm_i915_gem_execbuffer2 execbuf;
    srapi_result_t r;

    if (device == NULL || device->fd < 0 || device->gpu_driver != 915) {
        return SRAPI_ERROR_BAD_ARG;
    }

    r = srapi_i915_create_buffer(
        device,
        &(srapi_buffer_desc_t){
            .size = 4096,
            .usage = SRAPI_BUFFER_STORAGE,
            .initial_data = batch_words,
        },
        &batch
    );
    if (r != SRAPI_OK) {
        return r;
    }

    memset(&obj, 0, sizeof(obj));
    obj.handle = batch->gpu_handle;
    obj.flags = EXEC_OBJECT_SUPPORTS_48B_ADDRESS;

    memset(&execbuf, 0, sizeof(execbuf));
    execbuf.buffers_ptr = (uint64_t)(uintptr_t)&obj;
    execbuf.buffer_count = 1;
    execbuf.batch_start_offset = 0;
    execbuf.batch_len = sizeof(batch_words);
    execbuf.flags = I915_EXEC_RENDER;

    if (srapi_drm_ioctl(device->fd, DRM_IOCTL_I915_GEM_EXECBUFFER2, &execbuf) != 0) {
        srapi_set_error("i915: EXECBUFFER2 noop failed on %s: %s",
                        device->path, strerror(errno));
        srapi_gpu_destroy_buffer(batch);
        free(batch);
        return SRAPI_ERROR;
    }

    srapi_debugf("i915 noop submit ok handle=%u batch_len=%u",
                 batch->gpu_handle, execbuf.batch_len);
    srapi_gpu_destroy_buffer(batch);
    free(batch);
    return SRAPI_OK;
}

srapi_result_t srapi_i915_fill_buffer(
    srapi_device_t *device,
    srapi_buffer_t *dst,
    uint32_t width,
    uint32_t height,
    uint32_t pitch,
    uint32_t color
) {
    uint32_t batch_words[8];
    srapi_buffer_t *batch = NULL;
    struct drm_i915_gem_relocation_entry reloc;
    struct drm_i915_gem_exec_object2 objs[2];
    struct drm_i915_gem_execbuffer2 execbuf;
    struct drm_i915_gem_wait wait_arg;
    srapi_result_t r;

    if (device == NULL || dst == NULL || device->fd < 0 || device->gpu_driver != 915 ||
        dst->device != device || dst->gpu_memory != 1 || dst->gpu_handle == 0 ||
        width == 0 || height == 0 || pitch == 0) {
        return SRAPI_ERROR_BAD_ARG;
    }
    if (width > 32767 || height > 32767 || pitch > 0xffff) {
        srapi_set_error("i915: fill dimensions too large %ux%u pitch=%u", width, height, pitch);
        return SRAPI_ERROR_BAD_ARG;
    }
    if ((uint64_t)pitch * height > dst->gpu_size) {
        srapi_set_error("i915: fill outside buffer pitch=%u height=%u alloc=%llu",
                        pitch, height, (unsigned long long)dst->gpu_size);
        return SRAPI_ERROR_BAD_ARG;
    }

    memset(batch_words, 0, sizeof(batch_words));
    batch_words[0] = SRAPI_I915_XY_COLOR_BLT |
                     SRAPI_I915_BLT_WRITE_ALPHA |
                     SRAPI_I915_BLT_WRITE_RGB |
                     0x5u;
    batch_words[1] = (3u << 24) | (0xf0u << 16) | pitch;
    batch_words[2] = 0;
    batch_words[3] = (height << 16) | width;
    batch_words[4] = 0;
    batch_words[5] = 0;
    batch_words[6] = color;
    batch_words[7] = SRAPI_I915_MI_BATCH_BUFFER_END;

    r = srapi_i915_create_buffer(
        device,
        &(srapi_buffer_desc_t){
            .size = 4096,
            .usage = SRAPI_BUFFER_STORAGE,
            .initial_data = batch_words,
        },
        &batch
    );
    if (r != SRAPI_OK) {
        return r;
    }

    memset(&reloc, 0, sizeof(reloc));
    reloc.target_handle = dst->gpu_handle;
    reloc.offset = 4u * sizeof(uint32_t);
    reloc.write_domain = I915_GEM_DOMAIN_RENDER;

    memset(objs, 0, sizeof(objs));
    objs[0].handle = dst->gpu_handle;
    objs[0].alignment = 64;
    objs[0].flags = EXEC_OBJECT_WRITE | EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
    objs[1].handle = batch->gpu_handle;
    objs[1].relocation_count = 1;
    objs[1].relocs_ptr = (uint64_t)(uintptr_t)&reloc;
    objs[1].flags = EXEC_OBJECT_SUPPORTS_48B_ADDRESS;

    memset(&execbuf, 0, sizeof(execbuf));
    execbuf.buffers_ptr = (uint64_t)(uintptr_t)objs;
    execbuf.buffer_count = 2;
    execbuf.batch_len = sizeof(batch_words);
    execbuf.flags = I915_EXEC_BLT;

    if (srapi_drm_ioctl(device->fd, DRM_IOCTL_I915_GEM_EXECBUFFER2, &execbuf) != 0) {
        srapi_set_error("i915: EXECBUFFER2 color fill failed on %s: %s",
                        device->path, strerror(errno));
        srapi_gpu_destroy_buffer(batch);
        free(batch);
        return SRAPI_ERROR;
    }

    memset(&wait_arg, 0, sizeof(wait_arg));
    wait_arg.bo_handle = dst->gpu_handle;
    wait_arg.timeout_ns = 1000000000LL;
    if (srapi_drm_ioctl(device->fd, DRM_IOCTL_I915_GEM_WAIT, &wait_arg) != 0) {
        srapi_set_error("i915: GEM_WAIT fill dst handle=%u failed: %s",
                        dst->gpu_handle, strerror(errno));
        srapi_gpu_destroy_buffer(batch);
        free(batch);
        return SRAPI_ERROR;
    }

    srapi_debugf("i915 fill buffer ok dst=%u batch=%u %ux%u pitch=%u color=0x%08x",
                 dst->gpu_handle, batch->gpu_handle, width, height, pitch, color);
    srapi_gpu_destroy_buffer(batch);
    free(batch);
    return SRAPI_OK;
}

srapi_result_t srapi_i915_fill_image(srapi_device_t *device, srapi_image_t *image, uint32_t color) {
    srapi_buffer_t dst;

    if (device == NULL || image == NULL || image->device != device || image->gpu_memory != 1) {
        return SRAPI_ERROR_BAD_ARG;
    }

    memset(&dst, 0, sizeof(dst));
    dst.device = device;
    dst.backend = image->backend;
    dst.size = (size_t)image->gpu_size;
    dst.usage = image->usage;
    dst.data = image->data;
    dst.gpu_handle = image->gpu_handle;
    dst.gpu_size = image->gpu_size;
    dst.gpu_memory = image->gpu_memory;

    return srapi_i915_fill_buffer(device, &dst, image->width, image->height, image->pitch, color);
}

void srapi_i915_destroy_gem(int fd, uint32_t handle) {
    struct drm_gem_close close_arg;

    if (fd < 0 || handle == 0) {
        return;
    }

    memset(&close_arg, 0, sizeof(close_arg));
    close_arg.handle = handle;
    srapi_debugf("i915 gem close handle=%u", handle);
    srapi_drm_ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close_arg);
}

#include "i915.h"

#include "../drm/drm_internal.h"
#include "i915/render3d.h"

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

static int i915_caps_ready(const srapi_i915_probe_t *probe) {
    return probe != NULL && probe->available != 0 &&
           probe->has_gem != 0 && probe->has_execbuf2 != 0 && probe->has_blt != 0;
}

static srapi_result_t gem_set_domain(int fd, uint32_t handle, uint32_t read_domain, uint32_t write_domain) {
    struct drm_i915_gem_set_domain domain;

    if (fd < 0 || handle == 0) {
        return SRAPI_ERROR_BAD_ARG;
    }

    memset(&domain, 0, sizeof(domain));
    domain.handle = handle;
    domain.read_domains = read_domain;
    domain.write_domain = write_domain;
    if (srapi_drm_ioctl(fd, DRM_IOCTL_I915_GEM_SET_DOMAIN, &domain) != 0) {
        srapi_set_error("i915: GEM_SET_DOMAIN handle=%u read=0x%x write=0x%x failed: %s",
                        handle, read_domain, write_domain, strerror(errno));
        return SRAPI_ERROR;
    }
    return SRAPI_OK;
}

static srapi_result_t gem_wait(int fd, uint32_t handle, int64_t timeout_ns) {
    struct drm_i915_gem_wait wait_arg;

    if (fd < 0 || handle == 0) {
        return SRAPI_ERROR_BAD_ARG;
    }

    memset(&wait_arg, 0, sizeof(wait_arg));
    wait_arg.bo_handle = handle;
    wait_arg.timeout_ns = timeout_ns;
    if (srapi_drm_ioctl(fd, DRM_IOCTL_I915_GEM_WAIT, &wait_arg) != 0) {
        srapi_set_error("i915: GEM_WAIT handle=%u failed: %s", handle, strerror(errno));
        return SRAPI_ERROR;
    }
    return SRAPI_OK;
}

static srapi_result_t create_gem_object_fd(
    int fd,
    const char *path,
    uint64_t size,
    uint32_t *handle,
    uint64_t *alloc_size,
    void **map
) {
    struct drm_i915_gem_create create;
    struct drm_i915_gem_mmap mmap_arg;

    if (fd < 0 || handle == NULL || alloc_size == NULL || map == NULL || size == 0) {
        return SRAPI_ERROR_BAD_ARG;
    }

    memset(&create, 0, sizeof(create));
    create.size = size;
    if (srapi_drm_ioctl(fd, DRM_IOCTL_I915_GEM_CREATE, &create) != 0) {
        srapi_set_error("i915: GEM_CREATE size=%llu failed on %s: %s",
                        (unsigned long long)size, path != NULL ? path : "fd", strerror(errno));
        return SRAPI_ERROR_UNSUPPORTED;
    }

    memset(&mmap_arg, 0, sizeof(mmap_arg));
    mmap_arg.handle = create.handle;
    mmap_arg.size = create.size;
    if (srapi_drm_ioctl(fd, DRM_IOCTL_I915_GEM_MMAP, &mmap_arg) != 0) {
        srapi_set_error("i915: GEM_MMAP handle=%u size=%llu failed: %s",
                        create.handle, (unsigned long long)create.size, strerror(errno));
        srapi_i915_destroy_gem(fd, create.handle);
        return SRAPI_ERROR;
    }

    *handle = create.handle;
    *alloc_size = create.size;
    *map = (void *)(uintptr_t)mmap_arg.addr_ptr;
    return SRAPI_OK;
}

static int i915_device_ok(const srapi_device_t *device) {
    return device != NULL && device->fd >= 0 && device->gpu_driver == 915;
}

static int i915_buffer_ok(const srapi_buffer_t *buffer) {
    return buffer != NULL && buffer->gpu_memory == 1 && buffer->gpu_handle != 0;
}

srapi_result_t srapi_i915_set_tile_cache_enabled(srapi_device_t *device, uint32_t enabled) {
    if (!i915_device_ok(device)) {
        return SRAPI_ERROR_BAD_ARG;
    }

    device->i915_tile_cache_enabled = enabled != 0 ? 1u : 0u;
    if (!device->i915_tile_cache_enabled) {
        memset(device->i915_tile_hashes, 0, sizeof(device->i915_tile_hashes));
    }
    srapi_debugf("i915 tile cache %s", device->i915_tile_cache_enabled ? "enabled" : "disabled");
    return SRAPI_OK;
}

uint32_t srapi_i915_tile_cache_enabled(const srapi_device_t *device) {
    if (!i915_device_ok(device)) {
        return 0;
    }

    return device->i915_tile_cache_enabled;
}

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

    if (!i915_caps_ready(out)) {
        srapi_set_error("i915: unsupported capabilities on %s gem=%d execbuf2=%d blt=%d",
                        out->path[0] != '\0' ? out->path : "fd",
                        out->has_gem, out->has_execbuf2, out->has_blt);
        return SRAPI_ERROR_UNSUPPORTED;
    }

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
        srapi_set_error("i915: %s is not a usable i915 DRM node", path);
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
    srapi_result_t r;

    if (out != NULL) {
        *out = NULL;
    }
    if (device == NULL || desc == NULL || out == NULL || desc->size == 0 || device->fd < 0) {
        return SRAPI_ERROR_BAD_ARG;
    }
    if (device->gpu_driver != 915) {
        return SRAPI_ERROR_UNSUPPORTED;
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
        r = gem_set_domain(device->fd, buffer->gpu_handle, I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);
        if (r != SRAPI_OK) {
            munmap(buffer->data, buffer->gpu_size);
            srapi_i915_destroy_gem(device->fd, buffer->gpu_handle);
            free(buffer);
            return r;
        }
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
    if (device == NULL || handle == NULL || alloc_size == NULL || map == NULL ||
        size == 0 || device->fd < 0) {
        return SRAPI_ERROR_BAD_ARG;
    }

    return create_gem_object_fd(device->fd, device->path, size, handle, alloc_size, map);
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
    if (device->gpu_driver != 915) {
        return SRAPI_ERROR_UNSUPPORTED;
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
        r = gem_set_domain(device->fd, image->gpu_handle, I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);
        if (r != SRAPI_OK) {
            munmap(image->data, image->gpu_size);
            srapi_i915_destroy_gem(device->fd, image->gpu_handle);
            free(image);
            return r;
        }
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

srapi_result_t srapi_i915_buffer_handle(const srapi_buffer_t *buffer, uint32_t *out) {
    if (out != NULL) {
        *out = 0;
    }
    if (out == NULL || !i915_buffer_ok(buffer)) {
        return SRAPI_ERROR_BAD_ARG;
    }

    *out = buffer->gpu_handle;
    return SRAPI_OK;
}

srapi_result_t srapi_i915_buffer_gpu_offset(const srapi_buffer_t *buffer, uint64_t *out) {
    if (out != NULL) {
        *out = 0;
    }
    if (out == NULL || !i915_buffer_ok(buffer)) {
        return SRAPI_ERROR_BAD_ARG;
    }

    *out = buffer->gpu_offset;
    return SRAPI_OK;
}

srapi_result_t srapi_i915_set_domain(
    srapi_device_t *device,
    srapi_buffer_t *buffer,
    uint32_t read_domains,
    uint32_t write_domain
) {
    if (!i915_device_ok(device) || !i915_buffer_ok(buffer) || buffer->device != device) {
        return SRAPI_ERROR_BAD_ARG;
    }

    return gem_set_domain(device->fd, buffer->gpu_handle, read_domains, write_domain);
}

srapi_result_t srapi_i915_wait(srapi_device_t *device, srapi_buffer_t *buffer, int64_t timeout_ns) {
    if (!i915_device_ok(device) || !i915_buffer_ok(buffer) || buffer->device != device) {
        return SRAPI_ERROR_BAD_ARG;
    }

    return gem_wait(device->fd, buffer->gpu_handle, timeout_ns);
}

srapi_result_t srapi_i915_exec(srapi_device_t *device, const srapi_i915_exec_desc_t *desc) {
    struct drm_i915_gem_exec_object2 *objs;
    struct drm_i915_gem_relocation_entry **reloc_sets = NULL;
    struct drm_i915_gem_execbuffer2 execbuf;
    srapi_result_t r = SRAPI_OK;

    if (!i915_device_ok(device) || desc == NULL || !i915_buffer_ok(desc->batch) ||
        desc->batch->device != device || desc->objects == NULL || desc->object_count == 0) {
        return SRAPI_ERROR_BAD_ARG;
    }
    if (desc->batch_start_offset > desc->batch->size ||
        desc->batch_len == 0 ||
        desc->batch_len > desc->batch->size - desc->batch_start_offset ||
        desc->batch_len > UINT32_MAX ||
        desc->batch_start_offset > UINT32_MAX) {
        srapi_set_error("i915 exec: bad batch range offset=%zu len=%zu size=%zu",
                        desc->batch_start_offset, desc->batch_len, desc->batch->size);
        return SRAPI_ERROR_BAD_ARG;
    }

    objs = calloc(desc->object_count, sizeof(*objs));
    if (objs == NULL) {
        return SRAPI_ERROR_OOM;
    }
    reloc_sets = calloc(desc->object_count, sizeof(*reloc_sets));
    if (reloc_sets == NULL) {
        free(objs);
        return SRAPI_ERROR_OOM;
    }

    for (uint32_t i = 0; i < desc->object_count; i++) {
        const srapi_i915_exec_object_t *src = &desc->objects[i];

        if (!i915_buffer_ok(src->buffer) || src->buffer->device != device) {
            srapi_set_error("i915 exec: bad object %u", i);
            r = SRAPI_ERROR_BAD_ARG;
            goto done;
        }

        objs[i].handle = src->buffer->gpu_handle;
        objs[i].alignment = src->alignment;
        objs[i].offset = src->offset;
        objs[i].flags = src->flags;

        if (src->relocation_count > 0) {
            if (src->relocations == NULL) {
                srapi_set_error("i915 exec: object %u has null relocations", i);
                r = SRAPI_ERROR_BAD_ARG;
                goto done;
            }
            reloc_sets[i] = calloc(src->relocation_count, sizeof(*reloc_sets[i]));
            if (reloc_sets[i] == NULL) {
                r = SRAPI_ERROR_OOM;
                goto done;
            }

            for (uint32_t j = 0; j < src->relocation_count; j++) {
                const srapi_i915_relocation_t *reloc = &src->relocations[j];

                if (reloc->target_index >= desc->object_count) {
                    srapi_set_error("i915 exec: relocation target %u outside object list", reloc->target_index);
                    r = SRAPI_ERROR_BAD_ARG;
                    goto done;
                }
                reloc_sets[i][j].offset = reloc->offset;
                reloc_sets[i][j].delta = reloc->delta;
                reloc_sets[i][j].target_handle = desc->objects[reloc->target_index].buffer->gpu_handle;
                reloc_sets[i][j].read_domains = reloc->read_domains;
                reloc_sets[i][j].write_domain = reloc->write_domain;
                reloc_sets[i][j].presumed_offset = reloc->presumed_offset;
            }

            objs[i].relocation_count = src->relocation_count;
            objs[i].relocs_ptr = (uint64_t)(uintptr_t)reloc_sets[i];
        }
    }

    memset(&execbuf, 0, sizeof(execbuf));
    execbuf.buffers_ptr = (uint64_t)(uintptr_t)objs;
    execbuf.buffer_count = desc->object_count;
    execbuf.batch_start_offset = (uint32_t)desc->batch_start_offset;
    execbuf.batch_len = (uint32_t)desc->batch_len;
    execbuf.flags = desc->flags;

    if (srapi_drm_ioctl(device->fd, DRM_IOCTL_I915_GEM_EXECBUFFER2, &execbuf) != 0) {
        srapi_set_error("i915 exec: EXECBUFFER2 failed on %s: %s", device->path, strerror(errno));
        r = SRAPI_ERROR;
        goto done;
    }

    for (uint32_t i = 0; i < desc->object_count; i++) {
        desc->objects[i].offset = objs[i].offset;
        desc->objects[i].buffer->gpu_offset = objs[i].offset;
    }
    srapi_debugf("i915 exec ok engine_flags=0x%llx objects=%u batch_len=%u",
                 (unsigned long long)desc->flags, desc->object_count, execbuf.batch_len);

done:
    if (reloc_sets != NULL) {
        for (uint32_t i = 0; i < desc->object_count; i++) {
            free(reloc_sets[i]);
        }
        free(reloc_sets);
    }
    free(objs);
    return r;
}

srapi_result_t srapi_i915_submit_noop(srapi_device_t *device) {
    static const uint32_t batch_words[] = {
        SRAPI_I915_MI_BATCH_BUFFER_END,
        0x00000000u,
    };
    srapi_buffer_t *batch = NULL;
    srapi_i915_exec_object_t obj;
    srapi_i915_exec_desc_t exec;
    srapi_result_t r;

    if (device == NULL || device->fd < 0 || device->gpu_driver != 915) {
        return SRAPI_ERROR_BAD_ARG;
    }

    r = srapi_i915_create_buffer(
        device,
        &(srapi_buffer_desc_t){
            .size = sizeof(batch_words),
            .usage = SRAPI_BUFFER_STORAGE,
            .initial_data = batch_words,
        },
        &batch
    );
    if (r != SRAPI_OK) {
        return r;
    }

    r = gem_set_domain(device->fd, batch->gpu_handle, I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);
    if (r != SRAPI_OK) {
        srapi_gpu_destroy_buffer(batch);
        free(batch);
        return r;
    }

    memset(&obj, 0, sizeof(obj));
    obj.buffer = batch;
    obj.flags = SRAPI_I915_OBJECT_SUPPORTS_48B_ADDRESS;

    memset(&exec, 0, sizeof(exec));
    exec.batch = batch;
    exec.batch_len = sizeof(batch_words);
    exec.flags = SRAPI_I915_EXEC_RENDER;
    exec.objects = &obj;
    exec.object_count = 1;

    r = srapi_i915_exec(device, &exec);
    if (r != SRAPI_OK) {
        srapi_gpu_destroy_buffer(batch);
        free(batch);
        return r;
    }

    srapi_debugf("i915 noop submit ok handle=%u batch_len=%u",
                 batch->gpu_handle, (unsigned int)sizeof(batch_words));
    srapi_gpu_destroy_buffer(batch);
    free(batch);
    return SRAPI_OK;
}

srapi_result_t srapi_i915_fill_buffer(
    srapi_device_t *device,
    srapi_buffer_t *dst,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height,
    uint32_t pitch,
    uint32_t color
) {
    uint32_t batch_words[8];
    srapi_buffer_t *batch = NULL;
    srapi_i915_relocation_t reloc;
    srapi_i915_exec_object_t objs[2];
    srapi_i915_exec_desc_t exec;
    srapi_result_t r;

    if (device == NULL || dst == NULL || device->fd < 0 || device->gpu_driver != 915 ||
        dst->device != device || dst->gpu_memory != 1 || dst->gpu_handle == 0 ||
        width == 0 || height == 0 || pitch == 0) {
        return SRAPI_ERROR_BAD_ARG;
    }
    if (x > 32767 || y > 32767 || width > 32767 || height > 32767 ||
        x + width > 32767 || y + height > 32767 || pitch > 0xffff) {
        srapi_set_error("i915: fill dimensions too large %u,%u %ux%u pitch=%u",
                        x, y, width, height, pitch);
        return SRAPI_ERROR_BAD_ARG;
    }
    if ((uint64_t)pitch * height > dst->gpu_size) {
        srapi_set_error("i915: fill outside buffer pitch=%u height=%u alloc=%llu",
                        pitch, height, (unsigned long long)dst->gpu_size);
        return SRAPI_ERROR_BAD_ARG;
    }
    if ((uint64_t)y * pitch + (uint64_t)(x + width) * sizeof(uint32_t) > dst->gpu_size ||
        (uint64_t)(y + height) * pitch > dst->gpu_size) {
        srapi_set_error("i915: fill rect outside buffer %u,%u %ux%u pitch=%u alloc=%llu",
                        x, y, width, height, pitch, (unsigned long long)dst->gpu_size);
        return SRAPI_ERROR_BAD_ARG;
    }

    memset(batch_words, 0, sizeof(batch_words));
    batch_words[0] = SRAPI_I915_XY_COLOR_BLT |
                     SRAPI_I915_BLT_WRITE_ALPHA |
                     SRAPI_I915_BLT_WRITE_RGB |
                     0x5u;
    batch_words[1] = (3u << 24) | (0xf0u << 16) | pitch;
    batch_words[2] = (y << 16) | x;
    batch_words[3] = ((y + height) << 16) | (x + width);
    batch_words[4] = 0;
    batch_words[5] = 0;
    batch_words[6] = color;
    batch_words[7] = SRAPI_I915_MI_BATCH_BUFFER_END;

    r = srapi_i915_create_buffer(
        device,
        &(srapi_buffer_desc_t){
            .size = sizeof(batch_words),
            .usage = SRAPI_BUFFER_STORAGE,
            .initial_data = batch_words,
        },
        &batch
    );
    if (r != SRAPI_OK) {
        return r;
    }

    r = gem_set_domain(device->fd, batch->gpu_handle, I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);
    if (r != SRAPI_OK) {
        srapi_gpu_destroy_buffer(batch);
        free(batch);
        return r;
    }

    memset(&reloc, 0, sizeof(reloc));
    reloc.target_index = 0;
    reloc.offset = 4u * sizeof(uint32_t);
    reloc.write_domain = SRAPI_I915_DOMAIN_RENDER;

    memset(objs, 0, sizeof(objs));
    objs[0].buffer = dst;
    objs[0].alignment = 64;
    objs[0].flags = SRAPI_I915_OBJECT_WRITE | SRAPI_I915_OBJECT_SUPPORTS_48B_ADDRESS;
    objs[1].buffer = batch;
    objs[1].relocations = &reloc;
    objs[1].relocation_count = 1;
    objs[1].flags = SRAPI_I915_OBJECT_SUPPORTS_48B_ADDRESS;

    memset(&exec, 0, sizeof(exec));
    exec.batch = batch;
    exec.batch_len = sizeof(batch_words);
    exec.flags = SRAPI_I915_EXEC_BLT;
    exec.objects = objs;
    exec.object_count = 2;

    r = srapi_i915_exec(device, &exec);
    if (r != SRAPI_OK) {
        srapi_gpu_destroy_buffer(batch);
        free(batch);
        return r;
    }

    r = gem_wait(device->fd, dst->gpu_handle, 1000000000LL);
    if (r != SRAPI_OK) {
        srapi_gpu_destroy_buffer(batch);
        free(batch);
        return r;
    }
    r = gem_set_domain(device->fd, dst->gpu_handle, I915_GEM_DOMAIN_CPU, 0);
    if (r != SRAPI_OK) {
        srapi_gpu_destroy_buffer(batch);
        free(batch);
        return r;
    }

    srapi_debugf("i915 fill buffer ok dst=%u batch=%u %u,%u %ux%u pitch=%u color=0x%08x",
                 dst->gpu_handle, batch->gpu_handle, x, y, width, height, pitch, color);
    srapi_gpu_destroy_buffer(batch);
    free(batch);
    return SRAPI_OK;
}

static srapi_result_t i915_fill_handle_fd(
    int fd,
    const char *path,
    uint32_t dst_handle,
    uint64_t dst_size,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height,
    uint32_t pitch,
    uint32_t color
) {
    uint32_t batch_words[8];
    uint32_t batch_handle = 0;
    uint64_t batch_size = 0;
    void *batch_map = NULL;
    struct drm_i915_gem_relocation_entry reloc;
    struct drm_i915_gem_exec_object2 objs[2];
    struct drm_i915_gem_execbuffer2 execbuf;
    srapi_result_t r;

    if (fd < 0 || dst_handle == 0 || dst_size == 0 ||
        width == 0 || height == 0 || pitch == 0) {
        return SRAPI_ERROR_BAD_ARG;
    }
    if (x > 32767 || y > 32767 || width > 32767 || height > 32767 ||
        x + width > 32767 || y + height > 32767 || pitch > 0xffff) {
        srapi_set_error("i915: fill dimensions too large %u,%u %ux%u pitch=%u",
                        x, y, width, height, pitch);
        return SRAPI_ERROR_BAD_ARG;
    }
    if ((uint64_t)y * pitch + (uint64_t)(x + width) * sizeof(uint32_t) > dst_size ||
        (uint64_t)(y + height) * pitch > dst_size) {
        srapi_set_error("i915: fill rect outside handle=%u %u,%u %ux%u pitch=%u alloc=%llu",
                        dst_handle, x, y, width, height, pitch, (unsigned long long)dst_size);
        return SRAPI_ERROR_BAD_ARG;
    }

    memset(batch_words, 0, sizeof(batch_words));
    batch_words[0] = SRAPI_I915_XY_COLOR_BLT |
                     SRAPI_I915_BLT_WRITE_ALPHA |
                     SRAPI_I915_BLT_WRITE_RGB |
                     0x5u;
    batch_words[1] = (3u << 24) | (0xf0u << 16) | pitch;
    batch_words[2] = (y << 16) | x;
    batch_words[3] = ((y + height) << 16) | (x + width);
    batch_words[4] = 0;
    batch_words[5] = 0;
    batch_words[6] = color;
    batch_words[7] = SRAPI_I915_MI_BATCH_BUFFER_END;

    r = create_gem_object_fd(fd, path, sizeof(batch_words), &batch_handle, &batch_size, &batch_map);
    if (r != SRAPI_OK) {
        return r;
    }
    memcpy(batch_map, batch_words, sizeof(batch_words));
    r = gem_set_domain(fd, batch_handle, I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);
    if (r != SRAPI_OK) {
        munmap(batch_map, batch_size);
        srapi_i915_destroy_gem(fd, batch_handle);
        return r;
    }

    memset(&reloc, 0, sizeof(reloc));
    reloc.target_handle = dst_handle;
    reloc.offset = 4u * sizeof(uint32_t);
    reloc.write_domain = I915_GEM_DOMAIN_RENDER;

    memset(objs, 0, sizeof(objs));
    objs[0].handle = dst_handle;
    objs[0].alignment = 64;
    objs[0].flags = EXEC_OBJECT_WRITE | EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
    objs[1].handle = batch_handle;
    objs[1].relocation_count = 1;
    objs[1].relocs_ptr = (uint64_t)(uintptr_t)&reloc;
    objs[1].flags = EXEC_OBJECT_SUPPORTS_48B_ADDRESS;

    memset(&execbuf, 0, sizeof(execbuf));
    execbuf.buffers_ptr = (uint64_t)(uintptr_t)objs;
    execbuf.buffer_count = 2;
    execbuf.batch_len = sizeof(batch_words);
    execbuf.flags = I915_EXEC_BLT;

    if (srapi_drm_ioctl(fd, DRM_IOCTL_I915_GEM_EXECBUFFER2, &execbuf) != 0) {
        srapi_set_error("i915: EXECBUFFER2 screen fill failed on %s: %s",
                        path != NULL ? path : "fd", strerror(errno));
        munmap(batch_map, batch_size);
        srapi_i915_destroy_gem(fd, batch_handle);
        return SRAPI_ERROR;
    }

    r = gem_wait(fd, dst_handle, 1000000000LL);
    if (r == SRAPI_OK) {
        r = gem_set_domain(fd, dst_handle, I915_GEM_DOMAIN_CPU, 0);
    }
    srapi_debugf("i915 fill handle ok dst=%u batch=%u %u,%u %ux%u pitch=%u color=0x%08x",
                 dst_handle, batch_handle, x, y, width, height, pitch, color);
    munmap(batch_map, batch_size);
    srapi_i915_destroy_gem(fd, batch_handle);
    return r;
}

static srapi_result_t image_as_buffer(srapi_device_t *device, srapi_image_t *image, srapi_buffer_t *out) {
    if (device == NULL || image == NULL || out == NULL ||
        image->device != device || image->gpu_memory != 1) {
        return SRAPI_ERROR_BAD_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->device = device;
    out->backend = image->backend;
    out->size = (size_t)image->gpu_size;
    out->usage = image->usage;
    out->data = image->data;
    out->gpu_handle = image->gpu_handle;
    out->gpu_size = image->gpu_size;
    out->gpu_memory = image->gpu_memory;
    return SRAPI_OK;
}

srapi_result_t srapi_i915_fill_rect_image(
    srapi_device_t *device,
    srapi_image_t *image,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height,
    uint32_t color
) {
    srapi_buffer_t dst;
    srapi_result_t r;

    if (image == NULL || width == 0 || height == 0 ||
        x >= image->width || y >= image->height ||
        width > image->width - x || height > image->height - y) {
        return SRAPI_ERROR_BAD_ARG;
    }

    r = image_as_buffer(device, image, &dst);
    if (r != SRAPI_OK) {
        return r;
    }
    return srapi_i915_fill_buffer(device, &dst, x, y, width, height, image->pitch, color);
}

srapi_result_t srapi_i915_fill_image(srapi_device_t *device, srapi_image_t *image, uint32_t color) {
    if (image == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }

    return srapi_i915_fill_rect_image(device, image, 0, 0, image->width, image->height, color);
}

srapi_result_t srapi_i915_fill_framebuffer_rect(
    srapi_device_t *device,
    srapi_framebuffer_t *target,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height,
    uint32_t color
) {
    if (!i915_device_ok(device) || target == NULL ||
        target->gpu_fd < 0 || target->gpu_handle == 0 || target->gpu_size == 0) {
        return SRAPI_ERROR_BAD_ARG;
    }
    if (width == 0 || height == 0 || x >= target->width || y >= target->height ||
        width > target->width - x || height > target->height - y) {
        return SRAPI_ERROR_BAD_ARG;
    }

    return i915_fill_handle_fd(
        target->gpu_fd,
        device->path,
        target->gpu_handle,
        target->gpu_size,
        x,
        y,
        width,
        height,
        target->pitch,
        color
    );
}

static int clip_to_rect(
    int32_t x,
    int32_t y,
    uint32_t width,
    uint32_t height,
    int32_t clip_x,
    int32_t clip_y,
    uint32_t clip_width,
    uint32_t clip_height,
    uint32_t *out_x,
    uint32_t *out_y,
    uint32_t *out_width,
    uint32_t *out_height
) {
    int64_t x0 = x;
    int64_t y0 = y;
    int64_t x1 = (int64_t)x + width;
    int64_t y1 = (int64_t)y + height;
    int64_t cx0 = clip_x;
    int64_t cy0 = clip_y;
    int64_t cx1 = (int64_t)clip_x + clip_width;
    int64_t cy1 = (int64_t)clip_y + clip_height;

    if (width == 0 || height == 0 || clip_width == 0 || clip_height == 0) {
        return 0;
    }
    if (x0 < cx0) x0 = cx0;
    if (y0 < cy0) y0 = cy0;
    if (x1 > cx1) x1 = cx1;
    if (y1 > cy1) y1 = cy1;
    if (x0 >= x1 || y0 >= y1) {
        return 0;
    }

    *out_x = (uint32_t)x0;
    *out_y = (uint32_t)y0;
    *out_width = (uint32_t)(x1 - x0);
    *out_height = (uint32_t)(y1 - y0);
    return 1;
}

static uint64_t hash_mix_u64(uint64_t h, uint64_t v) {
    h ^= v;
    h *= 1099511628211ull;
    return h;
}

static uint64_t hash_mix_bytes(uint64_t h, const void *data, size_t len) {
    const uint8_t *bytes = (const uint8_t *)data;

    for (size_t i = 0; i < len; i++) {
        h = hash_mix_u64(h, bytes[i]);
    }
    return h;
}

static uint64_t i915_shader_hash(const srapi_shader_t *shader) {
    uint64_t h = 1469598103934665603ull;

    if (shader == NULL) {
        return 0;
    }

    h = hash_mix_u64(h, (uint64_t)shader->word_count);
    h = hash_mix_u64(h, (uint64_t)shader->inst_count);
    h = hash_mix_u64(h, (uint64_t)shader->uniform_count);
    h = hash_mix_bytes(h, shader->bytecode, shader->word_count * sizeof(uint32_t));
    h = hash_mix_bytes(h, shader->uniforms, shader->uniform_count * sizeof(float));
    return h;
}

static uint64_t i915_tile_command_hash(
    const srapi_command_t *op,
    int scissor_enabled,
    int32_t scissor_x,
    int32_t scissor_y,
    uint32_t scissor_width,
    uint32_t scissor_height,
    int32_t tile_x,
    int32_t tile_y,
    uint32_t tile_width,
    uint32_t tile_height
) {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint64_t h = 1469598103934665603ull;

    switch (op->kind) {
        case SRAPI_COMMAND_CLEAR:
        case SRAPI_COMMAND_FILL_RECT:
        case SRAPI_COMMAND_SHADE_RECT:
            if (!clip_to_rect(op->x0, op->y0, op->width, op->height,
                              scissor_enabled ? scissor_x : tile_x,
                              scissor_enabled ? scissor_y : tile_y,
                              scissor_enabled ? scissor_width : tile_width,
                              scissor_enabled ? scissor_height : tile_height,
                              &x, &y, &width, &height)) {
                return 0;
            }
            h = hash_mix_u64(h, (uint64_t)op->kind);
            h = hash_mix_u64(h, x);
            h = hash_mix_u64(h, y);
            h = hash_mix_u64(h, width);
            h = hash_mix_u64(h, height);
            h = hash_mix_u64(h, op->color);
            if (op->kind == SRAPI_COMMAND_SHADE_RECT) {
                h = hash_mix_u64(h, i915_shader_hash(op->shader));
            }
            return h;
        case SRAPI_COMMAND_DRAW_LINE: {
            int32_t min_x = op->x0 < op->x1 ? op->x0 : op->x1;
            int32_t max_x = op->x0 > op->x1 ? op->x0 : op->x1;
            int32_t min_y = op->y0 < op->y1 ? op->y0 : op->y1;
            int32_t max_y = op->y0 > op->y1 ? op->y0 : op->y1;

            if (!clip_to_rect(min_x, min_y, (uint32_t)(max_x - min_x + 1), (uint32_t)(max_y - min_y + 1),
                              scissor_enabled ? scissor_x : tile_x,
                              scissor_enabled ? scissor_y : tile_y,
                              scissor_enabled ? scissor_width : tile_width,
                              scissor_enabled ? scissor_height : tile_height,
                              &x, &y, &width, &height)) {
                return 0;
            }
            h = hash_mix_u64(h, (uint64_t)op->kind);
            h = hash_mix_u64(h, op->x0);
            h = hash_mix_u64(h, op->y0);
            h = hash_mix_u64(h, op->x1);
            h = hash_mix_u64(h, op->y1);
            h = hash_mix_u64(h, op->color);
            return h;
        }
        case SRAPI_COMMAND_FILL_TRIANGLE: {
            int32_t min_x = op->x0 < op->x1 ? op->x0 : op->x1;
            int32_t max_x = op->x0 > op->x1 ? op->x0 : op->x1;
            int32_t min_y = op->y0 < op->y1 ? op->y0 : op->y1;
            int32_t max_y = op->y0 > op->y1 ? op->y0 : op->y1;

            if (op->x2 < min_x) min_x = op->x2;
            if (op->x2 > max_x) max_x = op->x2;
            if (op->y2 < min_y) min_y = op->y2;
            if (op->y2 > max_y) max_y = op->y2;
            if (!clip_to_rect(min_x, min_y, (uint32_t)(max_x - min_x + 1), (uint32_t)(max_y - min_y + 1),
                              scissor_enabled ? scissor_x : tile_x,
                              scissor_enabled ? scissor_y : tile_y,
                              scissor_enabled ? scissor_width : tile_width,
                              scissor_enabled ? scissor_height : tile_height,
                              &x, &y, &width, &height)) {
                return 0;
            }
            h = hash_mix_u64(h, (uint64_t)op->kind);
            h = hash_mix_u64(h, op->x0);
            h = hash_mix_u64(h, op->y0);
            h = hash_mix_u64(h, op->x1);
            h = hash_mix_u64(h, op->y1);
            h = hash_mix_u64(h, op->x2);
            h = hash_mix_u64(h, op->y2);
            h = hash_mix_u64(h, op->color);
            return h;
        }
        case SRAPI_COMMAND_SET_SCISSOR:
            return hash_mix_u64(hash_mix_u64(hash_mix_u64(hash_mix_u64(h, op->kind), op->x0), op->y0), op->width ^ ((uint64_t)op->height << 32));
        case SRAPI_COMMAND_SET_VIEWPORT:
            return hash_mix_u64(hash_mix_u64(hash_mix_u64(hash_mix_u64(h, op->kind), op->x0), op->y0), op->width ^ ((uint64_t)op->height << 32));
        case SRAPI_COMMAND_SET_BLEND:
            return hash_mix_u64(hash_mix_u64(h, op->kind), (uint64_t)op->blend_mode);
        default:
            return hash_mix_u64(h, (uint64_t)op->kind);
    }
}

static uint64_t i915_tile_hash(
    const srapi_cmd_buffer_t *cmd,
    int32_t tile_x,
    int32_t tile_y,
    uint32_t tile_width,
    uint32_t tile_height,
    uint32_t target_width,
    uint32_t target_height
) {
    int scissor_enabled = 0;
    int32_t scissor_x = 0;
    int32_t scissor_y = 0;
    uint32_t scissor_width = target_width;
    uint32_t scissor_height = target_height;
    uint64_t h = 1469598103934665603ull;

    for (size_t i = 0; i < cmd->count; i++) {
        const srapi_command_t *op = &cmd->items[i];
        uint64_t ch = i915_tile_command_hash(
            op,
            scissor_enabled,
            scissor_x,
            scissor_y,
            scissor_width,
            scissor_height,
            tile_x,
            tile_y,
            tile_width,
            tile_height
        );

        switch (op->kind) {
            case SRAPI_COMMAND_SET_SCISSOR:
                scissor_enabled = 1;
                scissor_x = op->x0;
                scissor_y = op->y0;
                scissor_width = op->width;
                scissor_height = op->height;
                break;
            case SRAPI_COMMAND_SET_VIEWPORT:
            case SRAPI_COMMAND_SET_BLEND:
                break;
            default:
                break;
        }

        h = hash_mix_u64(h, ch);
    }
    return h;
}

static void i915_fill_rect_pixels(
    srapi_framebuffer_t *target,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height,
    uint32_t color
) {
    for (uint32_t row = y; row < y + height; row++) {
        uint32_t *pixels = (uint32_t *)((uint8_t *)target->pixels + (size_t)row * target->pitch);
        for (uint32_t col = x; col < x + width; col++) {
            pixels[col] = color;
        }
    }
}

static srapi_result_t i915_render_tile(
    srapi_device_t *device,
    srapi_framebuffer_t *target,
    const srapi_cmd_buffer_t *cmd,
    int32_t tile_x,
    int32_t tile_y,
    uint32_t tile_width,
    uint32_t tile_height
) {
    int scissor_enabled = 0;
    int32_t scissor_x = 0;
    int32_t scissor_y = 0;
    uint32_t scissor_width = target->width;
    uint32_t scissor_height = target->height;

    for (size_t i = 0; i < cmd->count; i++) {
        const srapi_command_t *op = &cmd->items[i];
        srapi_result_t r;
        uint32_t x;
        uint32_t y;
        uint32_t width;
        uint32_t height;
        int32_t clip_x = tile_x;
        int32_t clip_y = tile_y;
        uint32_t clip_width = tile_width;
        uint32_t clip_height = tile_height;

        if (scissor_enabled) {
            int32_t sx1 = scissor_x + (int32_t)scissor_width;
            int32_t sy1 = scissor_y + (int32_t)scissor_height;
            int32_t tx1 = tile_x + (int32_t)tile_width;
            int32_t ty1 = tile_y + (int32_t)tile_height;

            if (scissor_x > clip_x) clip_x = scissor_x;
            if (scissor_y > clip_y) clip_y = scissor_y;
            if (sx1 < tx1) clip_width = (uint32_t)(sx1 - clip_x);
            else clip_width = (uint32_t)(tx1 - clip_x);
            if (sy1 < ty1) clip_height = (uint32_t)(sy1 - clip_y);
            else clip_height = (uint32_t)(ty1 - clip_y);
            if (clip_width == 0 || clip_height == 0) {
                continue;
            }
        }

        switch (op->kind) {
            case SRAPI_COMMAND_CLEAR:
                if (!clip_to_rect(0, 0, target->width, target->height,
                                  clip_x, clip_y, clip_width, clip_height,
                                  &x, &y, &width, &height)) {
                    break;
                }
                i915_fill_rect_pixels(target, x, y, width, height, op->color);
                break;
            case SRAPI_COMMAND_FILL_RECT:
                if (!clip_to_rect(op->x0, op->y0, op->width, op->height,
                                  clip_x, clip_y, clip_width, clip_height,
                                  &x, &y, &width, &height)) {
                    break;
                }
                i915_fill_rect_pixels(target, x, y, width, height, op->color);
                break;
            case SRAPI_COMMAND_SET_SCISSOR:
                scissor_enabled = 1;
                scissor_x = op->x0;
                scissor_y = op->y0;
                scissor_width = op->width;
                scissor_height = op->height;
                break;
            case SRAPI_COMMAND_SET_VIEWPORT:
                break;
            case SRAPI_COMMAND_SET_BLEND:
                if (op->blend_mode != SRAPI_BLEND_NONE) {
                    srapi_set_error("i915 screen render: alpha blend is not implemented");
                    return SRAPI_ERROR_UNSUPPORTED;
                }
                break;
            case SRAPI_COMMAND_DRAW_LINE:
                r = srapi_i915_render3d_line_framebuffer(
                    device, target, op,
                    1,
                    clip_x, clip_y, clip_width, clip_height
                );
                if (r != SRAPI_OK) return r;
                break;
            case SRAPI_COMMAND_FILL_TRIANGLE:
                r = srapi_i915_render3d_triangle_framebuffer(
                    device, target, op,
                    1,
                    clip_x, clip_y, clip_width, clip_height
                );
                if (r != SRAPI_OK) return r;
                break;
            case SRAPI_COMMAND_SHADE_RECT:
                r = srapi_i915_render3d_shade_framebuffer(
                    device, target, op,
                    1,
                    clip_x, clip_y, clip_width, clip_height
                );
                if (r != SRAPI_OK) return r;
                break;
            default:
                srapi_set_error("i915 screen render: command kind %d is not implemented", op->kind);
                return SRAPI_ERROR_UNSUPPORTED;
        }
    }
    return SRAPI_OK;
}

srapi_result_t srapi_i915_render_image(
    srapi_device_t *device,
    srapi_image_t *target,
    const srapi_cmd_buffer_t *cmd
) {
    int scissor_enabled = 0;
    int32_t scissor_x = 0;
    int32_t scissor_y = 0;
    uint32_t scissor_width;
    uint32_t scissor_height;

    if (device == NULL || target == NULL || cmd == NULL ||
        device->gpu_driver != 915 || target->device != device || target->gpu_memory != 1) {
        return SRAPI_ERROR_BAD_ARG;
    }
    if ((target->usage & SRAPI_IMAGE_COLOR_TARGET) == 0 &&
        (target->usage & SRAPI_IMAGE_TRANSFER_DST) == 0) {
        srapi_set_error("i915 render: image missing color-target or transfer-dst usage");
        return SRAPI_ERROR_BAD_ARG;
    }

    scissor_width = target->width;
    scissor_height = target->height;
    for (size_t i = 0; i < cmd->count; i++) {
        const srapi_command_t *op = &cmd->items[i];
        srapi_result_t r;
        uint32_t x;
        uint32_t y;
        uint32_t width;
        uint32_t height;

        switch (op->kind) {
            case SRAPI_COMMAND_CLEAR:
                if (!clip_to_rect(0, 0, target->width, target->height,
                                  scissor_enabled ? scissor_x : 0,
                                  scissor_enabled ? scissor_y : 0,
                                  scissor_width, scissor_height,
                                  &x, &y, &width, &height)) {
                    break;
                }
                r = srapi_i915_fill_rect_image(device, target, x, y, width, height, op->color);
                if (r != SRAPI_OK) {
                    return r;
                }
                break;
            case SRAPI_COMMAND_FILL_RECT:
                if (!clip_to_rect(op->x0, op->y0, op->width, op->height,
                                  scissor_enabled ? scissor_x : 0,
                                  scissor_enabled ? scissor_y : 0,
                                  scissor_width, scissor_height,
                                  &x, &y, &width, &height)) {
                    break;
                }
                r = srapi_i915_fill_rect_image(device, target, x, y, width, height, op->color);
                if (r != SRAPI_OK) {
                    return r;
                }
                break;
            case SRAPI_COMMAND_SET_SCISSOR:
                scissor_enabled = 1;
                scissor_x = op->x0;
                scissor_y = op->y0;
                scissor_width = op->width;
                scissor_height = op->height;
                break;
            case SRAPI_COMMAND_SET_BLEND:
                if (op->blend_mode != SRAPI_BLEND_NONE) {
                    srapi_set_error("i915 render: alpha blend is not implemented");
                    return SRAPI_ERROR_UNSUPPORTED;
                }
                break;
            case SRAPI_COMMAND_SET_VIEWPORT:
                break;
            case SRAPI_COMMAND_SHADE_RECT:
                r = srapi_i915_render3d_shade_image(
                    device, target, op,
                    scissor_enabled, scissor_x, scissor_y,
                    scissor_width, scissor_height
                );
                if (r != SRAPI_OK) {
                    return r;
                }
                break;
            default:
                srapi_set_error("i915 render: command kind %d is not implemented", op->kind);
                return SRAPI_ERROR_UNSUPPORTED;
        }
    }

    srapi_debugf("i915 render image ok handle=%u commands=%zu %ux%u",
                 target->gpu_handle, cmd->count, target->width, target->height);
    return SRAPI_OK;
}

srapi_result_t srapi_i915_render_framebuffer(
    srapi_device_t *device,
    srapi_framebuffer_t *target,
    const srapi_cmd_buffer_t *cmd
) {
    int scissor_enabled = 0;
    int32_t scissor_x = 0;
    int32_t scissor_y = 0;
    uint32_t scissor_width;
    uint32_t scissor_height;

    if (!i915_device_ok(device) || target == NULL || cmd == NULL ||
        target->gpu_fd < 0 || target->gpu_handle == 0 || target->gpu_size == 0) {
        return SRAPI_ERROR_BAD_ARG;
    }

    if (!device->i915_tile_cache_enabled) {
        scissor_width = target->width;
        scissor_height = target->height;
        for (size_t i = 0; i < cmd->count; i++) {
            const srapi_command_t *op = &cmd->items[i];
            srapi_result_t r;
            uint32_t x;
            uint32_t y;
            uint32_t width;
            uint32_t height;

            switch (op->kind) {
                case SRAPI_COMMAND_CLEAR:
                    if (!clip_to_rect(0, 0, target->width, target->height,
                                      scissor_enabled ? scissor_x : 0,
                                      scissor_enabled ? scissor_y : 0,
                                      scissor_width, scissor_height,
                                      &x, &y, &width, &height)) {
                        break;
                    }
                    r = srapi_i915_fill_framebuffer_rect(device, target, x, y, width, height, op->color);
                    if (r != SRAPI_OK) return r;
                    break;
                case SRAPI_COMMAND_FILL_RECT:
                    if (!clip_to_rect(op->x0, op->y0, op->width, op->height,
                                      scissor_enabled ? scissor_x : 0,
                                      scissor_enabled ? scissor_y : 0,
                                      scissor_width, scissor_height,
                                      &x, &y, &width, &height)) {
                        break;
                    }
                    r = srapi_i915_fill_framebuffer_rect(device, target, x, y, width, height, op->color);
                    if (r != SRAPI_OK) return r;
                    break;
                case SRAPI_COMMAND_SET_SCISSOR:
                    scissor_enabled = 1;
                    scissor_x = op->x0;
                    scissor_y = op->y0;
                    scissor_width = op->width;
                    scissor_height = op->height;
                    break;
                case SRAPI_COMMAND_SET_VIEWPORT:
                    break;
                case SRAPI_COMMAND_SET_BLEND:
                    if (op->blend_mode != SRAPI_BLEND_NONE) {
                        srapi_set_error("i915 screen render: alpha blend is not implemented");
                        return SRAPI_ERROR_UNSUPPORTED;
                    }
                    break;
                case SRAPI_COMMAND_DRAW_LINE:
                    r = srapi_i915_render3d_line_framebuffer(
                        device, target, op,
                        scissor_enabled, scissor_x, scissor_y,
                        scissor_width, scissor_height
                    );
                    if (r != SRAPI_OK) return r;
                    break;
                case SRAPI_COMMAND_FILL_TRIANGLE:
                    r = srapi_i915_render3d_triangle_framebuffer(
                        device, target, op,
                        scissor_enabled, scissor_x, scissor_y,
                        scissor_width, scissor_height
                    );
                    if (r != SRAPI_OK) return r;
                    break;
                case SRAPI_COMMAND_SHADE_RECT:
                    r = srapi_i915_render3d_shade_framebuffer(
                        device, target, op,
                        scissor_enabled, scissor_x, scissor_y,
                        scissor_width, scissor_height
                    );
                    if (r != SRAPI_OK) return r;
                    break;
                default:
                    srapi_set_error("i915 screen render: command kind %d is not implemented", op->kind);
                    return SRAPI_ERROR_UNSUPPORTED;
            }
        }

        srapi_debugf("i915 render framebuffer ok handle=%u commands=%zu %ux%u tile_cache=0",
                     target->gpu_handle, cmd->count, target->width, target->height);
        return SRAPI_OK;
    }

    {
        const uint32_t cols = 4;
        const uint32_t rows = 4;
        uint32_t tile_w = (target->width + cols - 1) / cols;
        uint32_t tile_h = (target->height + rows - 1) / rows;

        if (tile_w == 0) tile_w = target->width;
        if (tile_h == 0) tile_h = target->height;

        for (uint32_t ty = 0; ty < rows; ty++) {
            for (uint32_t tx = 0; tx < cols; tx++) {
                uint32_t tile_index = ty * cols + tx;
                int32_t tile_x = (int32_t)(tx * tile_w);
                int32_t tile_y = (int32_t)(ty * tile_h);
                uint32_t cur_w = tile_w;
                uint32_t cur_h = tile_h;
                uint64_t tile_hash;

                if (tile_x >= (int32_t)target->width || tile_y >= (int32_t)target->height) {
                    continue;
                }
                if (tile_x + cur_w > target->width) cur_w = target->width - (uint32_t)tile_x;
                if (tile_y + cur_h > target->height) cur_h = target->height - (uint32_t)tile_y;
                if (cur_w == 0 || cur_h == 0) {
                    continue;
                }

                tile_hash = i915_tile_hash(cmd, tile_x, tile_y, cur_w, cur_h, target->width, target->height);
                if (device->i915_tile_cache_enabled && device->i915_tile_hashes[tile_index] == tile_hash) {
                    continue;
                }
                device->i915_tile_hashes[tile_index] = tile_hash;
                if (i915_render_tile(device, target, cmd, tile_x, tile_y, cur_w, cur_h) != SRAPI_OK) {
                    return SRAPI_ERROR;
                }
            }
        }
    }

    srapi_debugf("i915 render framebuffer ok handle=%u commands=%zu %ux%u tile_cache=%u",
                 target->gpu_handle, cmd->count, target->width, target->height,
                 device->i915_tile_cache_enabled);
    return SRAPI_OK;
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

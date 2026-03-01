/*
 * Copyright © 2025 Waydroid Project.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "gralloc_handler.h"

#include <cstdint>
#include <memory>

#include <linux/memfd.h>
#include <sys/syscall.h>
#include <drm_fourcc.h>
#include <wayland-client.h>

#include <log/log.h>
#include <ui/Rect.h>
#include <ui/GraphicBufferMapper.h>
#include <gralloc_handle.h>
#include <cros_gralloc/cros_gralloc_handle.h>

#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "wayland-android-client-protocol.h"

#include "egl-tools.h"
#include "wayland-hwc.h"

namespace {
    uint32_t ConvertHalFormatToShm(uint32_t hal_format) {
        uint32_t fmt;

        switch (hal_format) {
            case HAL_PIXEL_FORMAT_RGBX_8888:
                fmt = WL_SHM_FORMAT_XRGB8888;
                break;
            case HAL_PIXEL_FORMAT_BGRA_8888:
            case HAL_PIXEL_FORMAT_RGBA_8888:
                fmt = WL_SHM_FORMAT_ARGB8888;
                break;
            default:
                ALOGE("Cannot convert hal format to shm format %u", hal_format);
                return -EINVAL;
        }
        return fmt;
    }

    const wl_buffer_listener buffer_listener {
        [](void *, struct wl_buffer *) {}
    };
}
std::unique_ptr<buffer> create_shm_wl_buffer(display *display, const buffer_metadata& metadata, buffer_handle_t handle) {
    std::unique_ptr<buffer> buf { new buffer() };

    // Assume 4bpp formats or none of this is going to work
    int shm_stride = metadata.width * 4;
    int size = shm_stride * metadata.height;

    buf->metadata = metadata;
    buf->handle = handle;

    buf->isShm = true;
    buf->size = size;

    auto shm_format = ConvertHalFormatToShm(metadata.format);
    assert(shm_format >= 0);

    int fd = syscall(SYS_memfd_create, "buffer", MFD_ALLOW_SEALING);
    ftruncate(fd, size);
    buf->shm_data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (buf->shm_data == MAP_FAILED) {
        ALOGE("mmap failed");
        close(fd);
        return nullptr;
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(display->shm, fd, size);
    buf->wl_buffer = wl_shm_pool_create_buffer(pool, 0, metadata.width, metadata.height, shm_stride, shm_format);
    wl_buffer_add_listener(buf->wl_buffer, &buffer_listener, nullptr);
    wl_shm_pool_destroy(pool);
    close(fd);

    return buf;
}

namespace {
    //TODO: Implement failure handling for dmabuf
    void create_failed(void *data __unused, zwp_linux_buffer_params_v1 *params __unused)
    {
        ALOGE("%s: zwp_linux_buffer_params.create_immed failed.", __func__);
        abort();
    }

    const struct zwp_linux_buffer_params_v1_listener params_listener = {
            [](void *, zwp_linux_buffer_params_v1 *, wl_buffer *) {},
            create_failed
    };

    bool isFormatSupported(struct display *display, uint32_t format) {
        return display->formats.count(format) == 1;
    }

    uint32_t ConvertHalFormatToDrm(struct display *display, uint32_t hal_format) {
        uint32_t fmt;

        switch (hal_format) {
            case HAL_PIXEL_FORMAT_RGB_888:
                fmt = DRM_FORMAT_BGR888;
                if (!isFormatSupported(display, fmt))
                    fmt = DRM_FORMAT_RGB888;
                break;
            case HAL_PIXEL_FORMAT_BGRA_8888:
                fmt = DRM_FORMAT_ARGB8888;
                if (!isFormatSupported(display, fmt))
                    fmt = DRM_FORMAT_ABGR8888;
                break;
            case HAL_PIXEL_FORMAT_RGBX_8888:
                fmt = DRM_FORMAT_XBGR8888;
                if (!isFormatSupported(display, fmt))
                    fmt = DRM_FORMAT_XRGB8888;
                break;
            case HAL_PIXEL_FORMAT_RGBA_8888:
                fmt = DRM_FORMAT_ABGR8888;
                if (!isFormatSupported(display, fmt))
                    fmt = DRM_FORMAT_ARGB8888;
                break;
            case HAL_PIXEL_FORMAT_RGB_565:
                fmt = DRM_FORMAT_BGR565;
                if (!isFormatSupported(display, fmt))
                    fmt = DRM_FORMAT_RGB565;
                break;
            case HAL_PIXEL_FORMAT_YV12:
                fmt = DRM_FORMAT_YVU420;
                if (!isFormatSupported(display, fmt))
                    fmt = DRM_FORMAT_GR88;
                break;
            default:
                ALOGE("Cannot convert hal format to drm format %u", hal_format);
                return -EINVAL;
        }
        if (!isFormatSupported(display, fmt)) {
            ALOGE("Current wayland display doesn't support hal format %u", hal_format);
            return -EINVAL;
        }
        return fmt;
    }
}
std::unique_ptr<buffer> create_dmabuf_wl_buffer(display *display, const buffer_metadata& metadata,
                                                                int prime_fd, int drm_format, int byte_stride,
                                                                int offset, uint64_t modifier, buffer_handle_t handle)
{
    assert(prime_fd >= 0);

    std::unique_ptr<buffer> buf { new buffer() };

    buf->metadata = metadata;
    buf->handle = handle;

    if (drm_format < 0) {
        drm_format = ConvertHalFormatToDrm(display, metadata.format);
    }
    assert(drm_format >= 0);

    zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(display->dmabuf);
    zwp_linux_buffer_params_v1_add(params, prime_fd, 0, offset, byte_stride, modifier >> 32, modifier & 0xffffffff);
    zwp_linux_buffer_params_v1_add_listener(params, &params_listener, nullptr);

    buf->wl_buffer = zwp_linux_buffer_params_v1_create_immed(params, buf->metadata.width, buf->metadata.height, drm_format, 0);
    wl_buffer_add_listener(buf->wl_buffer, &buffer_listener, nullptr);

    return buf;
}

std::unique_ptr<buffer> create_android_wl_buffer(display *display, const buffer_metadata& metadata, buffer_handle_t handle)
{
    std::unique_ptr<buffer> buf { new buffer() };

    buf->metadata = metadata;
    buf->handle = handle;

    struct wl_array ints;
    wl_array_init(&ints);
    int *the_ints = (int *)wl_array_add(&ints, handle->numInts * sizeof(int));
    memcpy(the_ints, handle->data + handle->numFds, handle->numInts * sizeof(int));
    android_wlegl_handle *wlegl_handle = android_wlegl_create_handle(display->android_wlegl, handle->numFds, &ints);
    wl_array_release(&ints);

    for (int i = 0; i < handle->numFds; i++) {
        android_wlegl_handle_add_fd(wlegl_handle, handle->data[i]);
    }

    buf->wl_buffer = android_wlegl_create_buffer(display->android_wlegl, buf->metadata.width, buf->metadata.height, buf->metadata.pixel_stride, metadata.format, GRALLOC_USAGE_HW_RENDER, wlegl_handle);
    android_wlegl_handle_destroy(wlegl_handle);

    wl_buffer_add_listener(buf->wl_buffer, &buffer_listener, nullptr);

    return buf;
}

namespace {
buffer_metadata get_buffer_metadata_generic(display *display, hwc_layer_1_t *layer, size_t pos) {
    uint32_t format, pixel_stride, width, height;
    if (layer->compositionType == HWC_FRAMEBUFFER_TARGET) {
        format = display->target_layer_handle_ext.format;
        pixel_stride = display->target_layer_handle_ext.stride;
        width = display->target_layer_handle_ext.width;
        height = display->target_layer_handle_ext.height;
    } else {
        format = display->layer_handles_ext[pos].format;
        pixel_stride = display->layer_handles_ext[pos].stride;
        width = display->layer_handles_ext[pos].width;
        height = display->layer_handles_ext[pos].height;
    }

    if (!width)
        width = layer->displayFrame.right - layer->displayFrame.left;
    if (!height)
        height = layer->displayFrame.bottom - layer->displayFrame.top;

    return {height, width, pixel_stride, format};
}
buffer_metadata get_buffer_metadata_gbm(display *, hwc_layer_1_t *layer, size_t) {
    auto handle = reinterpret_cast<const gralloc_handle_t *>(layer->handle);
    return {
            handle->height,
            handle->width,
            handle->stride,
            handle->format
    };
}
buffer_metadata get_buffer_metadata_cros(display *, hwc_layer_1_t *layer, size_t) {
    auto handle = reinterpret_cast<const cros_gralloc_handle *>(layer->handle);
    return {
            handle->height,
            handle->width,
            handle->pixel_stride,
            static_cast<uint32_t>(handle->droid_format)
    };
}

std::unique_ptr<buffer> create_buffer_generic(display *display, const buffer_metadata& metadata, buffer_handle_t handle) {
    return create_shm_wl_buffer(display, metadata, handle);
}
std::unique_ptr<buffer> create_buffer_gbm(display *display, const buffer_metadata& metadata, buffer_handle_t handle) {
    auto *drm_handle = reinterpret_cast<const gralloc_handle_t *>(handle);
    return create_dmabuf_wl_buffer(display, metadata, drm_handle->prime_fd, -1 /* compute drm format */, drm_handle->stride, 0 /* offset */, drm_handle->modifier, handle);
}
std::unique_ptr<buffer> create_buffer_cros(display *display, const buffer_metadata& metadata, buffer_handle_t handle) {
    auto *cros_handle = reinterpret_cast<const cros_gralloc_handle *>(handle);
    return create_dmabuf_wl_buffer(display, metadata, cros_handle->fds[0], cros_handle->format, cros_handle->strides[0], cros_handle->offsets[0], cros_handle->format_modifier, handle);
}
std::unique_ptr<buffer> create_buffer_android(display *display, const buffer_metadata& metadata, buffer_handle_t handle) {
    return create_android_wl_buffer(display, metadata, handle);
}

void update_shm_buffer_generic(display *display, buffer *buffer) {
    display->egl_work_queue.emplace_back(std::bind(egl_render_to_pixels, display, buffer));
    sem_post(&display->egl_go);
    sem_wait(&display->egl_done);
}
void update_shm_buffer_default(display *, buffer *buffer) {
    void *data;
    uint32_t shm_stride, src_stride;
    android::Rect bounds(buffer->metadata.width, buffer->metadata.height);
    if (android::GraphicBufferMapper::get().lock(buffer->handle, GRALLOC_USAGE_SW_READ_OFTEN, bounds, &data) == 0) {
        src_stride = buffer->metadata.pixel_stride;
        shm_stride = buffer->metadata.width;
        for (int i = 0; i < buffer->metadata.height; i++) {
            uint32_t* source = (uint32_t*)data + (i * src_stride);
            uint32_t* dist = (uint32_t*)buffer->shm_data + (i * shm_stride);
            uint32_t* end = dist + shm_stride;

            while (dist < end) {
                uint32_t c = *source;
                *dist = (c & 0xFF00FF00) | ((c & 0xFF0000) >> 16) | ((c & 0xFF) << 16);
                source++;
                dist++;
            }
        }
        android::GraphicBufferMapper::get().unlock(buffer->handle);
    }
}
}

gralloc_handler::get_buffer_metadata_func gralloc_handler::select_get_buffer_metadata_impl(GrallocType gralloc_type) {
    switch (gralloc_type) {
        case GrallocType::GRALLOC_GBM:
            return get_buffer_metadata_gbm;
        case GrallocType::GRALLOC_CROS:
            return get_buffer_metadata_cros;
        case GrallocType::GRALLOC_ANDROID:
        case GrallocType::GRALLOC_DEFAULT:
            return get_buffer_metadata_generic;
        default:
            abort();
    }
}

gralloc_handler::create_buffer_func gralloc_handler::select_create_buffer_impl(display *display, GrallocType gralloc_type) {
    if (gralloc_type == GrallocType::GRALLOC_GBM && display->dmabuf) {
        return create_buffer_gbm;
    } else if (gralloc_type == GrallocType::GRALLOC_CROS && display->dmabuf) {
        return create_buffer_cros;
    } else if (gralloc_type == GrallocType::GRALLOC_ANDROID && display->android_wlegl) {
        return create_buffer_android;
    } else {
        return create_buffer_generic;
    }
}

gralloc_handler::update_shm_buffer_func gralloc_handler::select_update_shm_buffer_impl(GrallocType gralloc_type) {
    if (gralloc_type == GrallocType::GRALLOC_DEFAULT) {
        return update_shm_buffer_default;
    } else {
        return update_shm_buffer_generic;
    }
}

gralloc_handler::gralloc_handler(display *display)
    : get_buffer_metadata_impl(select_get_buffer_metadata_impl(display->gtype))
    , create_buffer_impl(select_create_buffer_impl(display, display->gtype))
    , update_shm_buffer_impl(select_update_shm_buffer_impl(display->gtype)) { }

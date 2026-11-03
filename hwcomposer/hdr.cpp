/*
 * Copyright (C) 2026 The Waydroid Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// HDR video for single-window and full-UI mode.
//
// The composer HAL is 2.1 behind HWC2On1Adapter, so SurfaceFlinger can neither
// output PQ nor pass HDR metadata. Instead, the HWC2 adapter asks this module for
// the HDR capabilities of the host output (wp_color_management_v1) and passes
// each layer's dataspace. With HDR10/HLG reported, SurfaceFlinger offers 10-bit
// HDR video layers for device composition. Those are taken out of the client
// target, which gets a transparent hole for them (HWC_HINT_CLEAR_FB), and drawn
// on a subsurface below it: converted from YCbCr to RGB, still PQ or HLG
// encoded, into a 10-bit wl_shm buffer tagged with a BT.2020 PQ/HLG image
// description. The host compositor maps it to the output.

#include "hdr.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

#include <linux/memfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cutils/properties.h>
#include <log/log.h>
#include <sync/sync.h>
#include <system/graphics.h>
#include <ui/GraphicBufferMapper.h>
#include <ui/Rect.h>

#include "color-management-v1-client-protocol.h"
#include "viewporter-client-protocol.h"

#include "hwcomposer.h"
#include "wayland-hwc.h"

namespace {

// Image description of the host output, as read from wp_image_description_info_v1
struct output_info {
    uint32_t tf = 0;
    uint32_t primaries = 0;
    float min_luminance = 0;
    float max_luminance = 0;
    float target_min_luminance = 0;
    float target_max_luminance = 0;
    float max_cll = 0;
    float max_fall = 0;
};

// Image description for video surfaces; only usable once ready
struct video_description {
    struct wp_image_description_v1 *description = nullptr;
    std::atomic<bool> ready = false;
};

} // namespace

struct hdr_display_state {
    struct display *display;
    struct wp_color_manager_v1 *manager = nullptr;
    struct wp_color_management_output_v1 *output = nullptr;
    bool usable = false;

    // From wp_color_manager_v1 events
    bool parametric = false;
    std::vector<uint32_t> tfs;
    std::vector<uint32_t> primaries;

    // Handlers can run on the Wayland thread and on a thread in wl_display_roundtrip() at once
    std::mutex mutex;
    // Guarded by mutex: the output description being read, and the last one read
    struct wp_image_description_v1 *pending_output_description = nullptr;
    output_info reading;
    output_info current;
    bool current_valid = false;

    // Keyed by transfer function, BT.2020 primaries. Written before the Wayland thread starts.
    std::map<uint32_t, video_description> video_descriptions;

    // Release events of video buffers, dispatched only by hwc_set(), which also destroys them
    struct wl_event_queue *buffer_queue = nullptr;
};

namespace {

// A converted video frame; not rewritten until the compositor releases it
struct frame_buffer {
    std::unique_ptr<buffer> buf;
    bool busy = false;
};

// The source of the frame on the surface, to skip converting it again
struct frame_source {
    buffer_handle_t handle = nullptr;
    int32_t dataspace = 0;
    hwc_frect_t crop {};
    hwc_rect_t frame {};

    bool operator==(const frame_source &o) const {
        return handle == o.handle && dataspace == o.dataspace
            && crop.left == o.crop.left && crop.top == o.crop.top && crop.right == o.crop.right && crop.bottom == o.crop.bottom
            && frame.left == o.frame.left && frame.top == o.frame.top && frame.right == o.frame.right && frame.bottom == o.frame.bottom;
    }
};

} // namespace

struct hdr_window_state {
    window::layer layer;
    struct wp_color_management_surface_v1 *color_surface = nullptr;
    uint32_t tagged_tf = 0;
    bool attached = false;
    bool used = false;
    std::vector<std::unique_ptr<frame_buffer>> buffers;
    frame_source shown;
};

namespace {

bool contains(const std::vector<uint32_t> &values, uint32_t value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

void manager_supported_intent(void *, struct wp_color_manager_v1 *, uint32_t) {}
void manager_supported_feature(void *data, struct wp_color_manager_v1 *, uint32_t feature) {
    auto *hdr = static_cast<hdr_display_state *>(data);
    if (feature == WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC)
        hdr->parametric = true;
}
void manager_supported_tf_named(void *data, struct wp_color_manager_v1 *, uint32_t tf) {
    static_cast<hdr_display_state *>(data)->tfs.push_back(tf);
}
void manager_supported_primaries_named(void *data, struct wp_color_manager_v1 *, uint32_t primaries) {
    static_cast<hdr_display_state *>(data)->primaries.push_back(primaries);
}
void manager_done(void *, struct wp_color_manager_v1 *) {}

const struct wp_color_manager_v1_listener manager_listener = {
    manager_supported_intent,
    manager_supported_feature,
    manager_supported_tf_named,
    manager_supported_primaries_named,
    manager_done,
};

template<typename F>
void update_reading(void *data, F update) {
    auto *hdr = static_cast<hdr_display_state *>(data);
    std::scoped_lock lock(hdr->mutex);
    update(hdr->reading);
}

void info_done(void *data, struct wp_image_description_info_v1 *info) {
    auto *hdr = static_cast<hdr_display_state *>(data);
    output_info read;
    {
        std::scoped_lock lock(hdr->mutex);
        hdr->current = hdr->reading;
        hdr->current_valid = true;
        read = hdr->reading;
    }
    ALOGI("HDR: output tf %u primaries %u, luminance %.4f-%.0f, target %.4f-%.0f, max_cll %.0f max_fall %.0f",
          read.tf, read.primaries, read.min_luminance, read.max_luminance,
          read.target_min_luminance, read.target_max_luminance, read.max_cll, read.max_fall);
    wl_proxy_destroy(reinterpret_cast<struct wl_proxy *>(info));
}
void info_icc_file(void *, struct wp_image_description_info_v1 *, int32_t icc, uint32_t) {
    close(icc);
}
void info_primaries(void *, struct wp_image_description_info_v1 *, int32_t, int32_t, int32_t, int32_t,
                    int32_t, int32_t, int32_t, int32_t) {}
void info_primaries_named(void *data, struct wp_image_description_info_v1 *, uint32_t primaries) {
    update_reading(data, [&](output_info &r) { r.primaries = primaries; });
}
void info_tf_power(void *, struct wp_image_description_info_v1 *, uint32_t) {}
void info_tf_named(void *data, struct wp_image_description_info_v1 *, uint32_t tf) {
    update_reading(data, [&](output_info &r) { r.tf = tf; });
}
void info_luminances(void *data, struct wp_image_description_info_v1 *, uint32_t min_lum, uint32_t max_lum, uint32_t) {
    update_reading(data, [&](output_info &r) {
        r.min_luminance = min_lum / 10000.0f;
        r.max_luminance = max_lum;
    });
}
void info_target_primaries(void *, struct wp_image_description_info_v1 *, int32_t, int32_t, int32_t, int32_t,
                           int32_t, int32_t, int32_t, int32_t) {}
void info_target_luminance(void *data, struct wp_image_description_info_v1 *, uint32_t min_lum, uint32_t max_lum) {
    update_reading(data, [&](output_info &r) {
        r.target_min_luminance = min_lum / 10000.0f;
        r.target_max_luminance = max_lum;
    });
}
void info_target_max_cll(void *data, struct wp_image_description_info_v1 *, uint32_t max_cll) {
    update_reading(data, [&](output_info &r) { r.max_cll = max_cll; });
}
void info_target_max_fall(void *data, struct wp_image_description_info_v1 *, uint32_t max_fall) {
    update_reading(data, [&](output_info &r) { r.max_fall = max_fall; });
}

const struct wp_image_description_info_v1_listener info_listener = {
    info_done,
    info_icc_file,
    info_primaries,
    info_primaries_named,
    info_tf_power,
    info_tf_named,
    info_luminances,
    info_target_primaries,
    info_target_luminance,
    info_target_max_cll,
    info_target_max_fall,
};

void output_description_failed(void *data, struct wp_image_description_v1 *description, uint32_t cause, const char *msg) {
    auto *hdr = static_cast<hdr_display_state *>(data);
    ALOGW("HDR: cannot read the output's image description (%u): %s", cause, msg);
    std::scoped_lock lock(hdr->mutex);
    if (hdr->pending_output_description == description)
        hdr->pending_output_description = nullptr;
    wp_image_description_v1_destroy(description);
}
void output_description_ready(void *data, struct wp_image_description_v1 *description, uint32_t) {
    auto *hdr = static_cast<hdr_display_state *>(data);
    std::scoped_lock lock(hdr->mutex);
    if (hdr->pending_output_description == description) {
        hdr->pending_output_description = nullptr;
        hdr->reading = {};
        auto *info = wp_image_description_v1_get_information(description);
        wp_image_description_info_v1_add_listener(info, &info_listener, hdr);
    }
    wp_image_description_v1_destroy(description);
}
void output_description_ready2(void *, struct wp_image_description_v1 *, uint32_t, uint32_t) {}

const struct wp_image_description_v1_listener output_description_listener = {
    output_description_failed,
    output_description_ready,
    output_description_ready2,
};

void request_output_description(hdr_display_state *hdr) {
    std::scoped_lock lock(hdr->mutex);
    // A superseded request still delivers ready or failed, which destroys it
    hdr->pending_output_description = wp_color_management_output_v1_get_image_description(hdr->output);
    wp_image_description_v1_add_listener(hdr->pending_output_description, &output_description_listener, hdr);
}

void output_image_description_changed(void *data, struct wp_color_management_output_v1 *) {
    // Android reads the capabilities again on the next display hotplug
    request_output_description(static_cast<hdr_display_state *>(data));
}

const struct wp_color_management_output_v1_listener output_listener = {
    output_image_description_changed,
};

void video_description_failed(void *data, struct wp_image_description_v1 *description, uint32_t cause, const char *msg) {
    ALOGW("HDR: compositor refused a video image description (%u): %s", cause, msg);
    // Never ready, so nothing else uses it
    static_cast<video_description *>(data)->description = nullptr;
    wp_image_description_v1_destroy(description);
}
void video_description_ready(void *data, struct wp_image_description_v1 *, uint32_t) {
    static_cast<video_description *>(data)->ready = true;
}
void video_description_ready2(void *, struct wp_image_description_v1 *, uint32_t, uint32_t) {}

const struct wp_image_description_v1_listener video_description_listener = {
    video_description_failed,
    video_description_ready,
    video_description_ready2,
};

void create_video_description(hdr_display_state *hdr, uint32_t tf) {
    if (!contains(hdr->tfs, tf))
        return;
    auto *creator = wp_color_manager_v1_create_parametric_creator(hdr->manager);
    wp_image_description_creator_params_v1_set_tf_named(creator, tf);
    wp_image_description_creator_params_v1_set_primaries_named(creator, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020);
    auto &video = hdr->video_descriptions[tf];
    video.description = wp_image_description_creator_params_v1_create(creator);
    wp_image_description_v1_add_listener(video.description, &video_description_listener, &video);
}

const video_description *ready_video_description(hdr_display_state *hdr, uint32_t tf) {
    auto it = hdr->video_descriptions.find(tf);
    return it != hdr->video_descriptions.end() && it->second.ready ? &it->second : nullptr;
}

uint32_t dataspace_transfer_function(int32_t dataspace) {
    switch (dataspace & HAL_DATASPACE_TRANSFER_MASK) {
        case HAL_DATASPACE_TRANSFER_HLG:
            return WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_HLG;
        case HAL_DATASPACE_TRANSFER_ST2084:
        // 10-bit YUV without a dataspace is HDR10 video in practice
        case HAL_DATASPACE_TRANSFER_UNSPECIFIED:
            return WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ;
        default:
            return 0;
    }
}

int32_t layer_dataspace(waydroid_hwc_composer_device_1 *pdev, size_t index) {
    return index < pdev->layer_dataspaces.size() ? pdev->layer_dataspaces[index] : HAL_DATASPACE_UNKNOWN;
}

// Non-constant luminance Y'CbCr to R'G'B', keeping the transfer function
struct yuv_coefficients {
    float y_offset, y_scale, c_scale;
    float r_cr, g_cb, g_cr, b_cb;
};

yuv_coefficients coefficients_for(int32_t dataspace) {
    // Kr, Kb of BT.2020, or BT.709 when the dataspace says so
    float kr = 0.2627f, kb = 0.0593f;
    if ((dataspace & HAL_DATASPACE_STANDARD_MASK) == HAL_DATASPACE_STANDARD_BT709) {
        kr = 0.2126f;
        kb = 0.0722f;
    }
    const float kg = 1 - kr - kb;

    yuv_coefficients c;
    if ((dataspace & HAL_DATASPACE_RANGE_MASK) == HAL_DATASPACE_RANGE_FULL) {
        c.y_offset = 0;
        c.y_scale = 1 / 1023.0f;
        c.c_scale = 1 / 1023.0f;
    } else {
        c.y_offset = 64;
        c.y_scale = 1 / 876.0f;
        c.c_scale = 1 / 896.0f;
    }
    c.r_cr = 2 * (1 - kr);
    c.g_cb = -2 * kb * (1 - kb) / kg;
    c.g_cr = -2 * kr * (1 - kr) / kg;
    c.b_cb = 2 * (1 - kb);
    return c;
}

// Horizontal bilinear taps of one output column, the same for every row
struct column_taps {
    int x0, x1, cx0, cx1;
    float fx, fcx;
};

struct conversion {
    android_ycbcr src;
    int src_width, src_height;
    hwc_frect_t crop;
    int out_width, out_height;
    uint32_t *dst;
    yuv_coefficients c;
    std::vector<column_taps> columns;
};

// P010 stores each 10-bit sample in the high bits of 16
inline float sample(const uint8_t *row, int x, int step) {
    return reinterpret_cast<const uint16_t *>(row + x * step)[0] >> 6;
}

inline uint32_t to_10bit(float v) {
    return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 1023.0f + 0.5f);
}

// Bilinear sampling, which averages 2x2 texels at the common 2x downscale
void convert_rows(const conversion &cv, int row_begin, int row_end) {
    const auto *y_plane = static_cast<const uint8_t *>(cv.src.y);
    const auto *cb_plane = static_cast<const uint8_t *>(cv.src.cb);
    const auto *cr_plane = static_cast<const uint8_t *>(cv.src.cr);
    const int step = cv.src.chroma_step;
    const int chroma_height = (cv.src_height + 1) / 2;
    const float sy_scale = (cv.crop.bottom - cv.crop.top) / cv.out_height;

    for (int oy = row_begin; oy < row_end; oy++) {
        const float sy = std::clamp(cv.crop.top + (oy + 0.5f) * sy_scale - 0.5f, 0.0f, cv.src_height - 1.0f);
        const int y0 = static_cast<int>(sy);
        const int y1 = std::min(y0 + 1, cv.src_height - 1);
        const float fy = sy - y0;
        // Chroma is sited between the two luma rows it covers
        const float cy = std::clamp(sy * 0.5f - 0.25f, 0.0f, chroma_height - 1.0f);
        const int cy0 = static_cast<int>(cy);
        const int cy1 = std::min(cy0 + 1, chroma_height - 1);
        const float fcy = cy - cy0;

        const uint8_t *yr0 = y_plane + y0 * cv.src.ystride;
        const uint8_t *yr1 = y_plane + y1 * cv.src.ystride;
        const uint8_t *cbr0 = cb_plane + cy0 * cv.src.cstride;
        const uint8_t *cbr1 = cb_plane + cy1 * cv.src.cstride;
        const uint8_t *crr0 = cr_plane + cy0 * cv.src.cstride;
        const uint8_t *crr1 = cr_plane + cy1 * cv.src.cstride;
        uint32_t *out = cv.dst + static_cast<size_t>(oy) * cv.out_width;

        for (int ox = 0; ox < cv.out_width; ox++) {
            const auto [x0, x1, cx0, cx1, fx, fcx] = cv.columns[ox];

            const float luma_top = sample(yr0, x0, 2) + (sample(yr0, x1, 2) - sample(yr0, x0, 2)) * fx;
            const float luma_bottom = sample(yr1, x0, 2) + (sample(yr1, x1, 2) - sample(yr1, x0, 2)) * fx;
            const float luma = luma_top + (luma_bottom - luma_top) * fy;

            const float cb_top = sample(cbr0, cx0, step) + (sample(cbr0, cx1, step) - sample(cbr0, cx0, step)) * fcx;
            const float cb_bottom = sample(cbr1, cx0, step) + (sample(cbr1, cx1, step) - sample(cbr1, cx0, step)) * fcx;
            const float cr_top = sample(crr0, cx0, step) + (sample(crr0, cx1, step) - sample(crr0, cx0, step)) * fcx;
            const float cr_bottom = sample(crr1, cx0, step) + (sample(crr1, cx1, step) - sample(crr1, cx0, step)) * fcx;

            const float y = (luma - cv.c.y_offset) * cv.c.y_scale;
            const float cb = (cb_top + (cb_bottom - cb_top) * fcy - 512) * cv.c.c_scale;
            const float cr = (cr_top + (cr_bottom - cr_top) * fcy - 512) * cv.c.c_scale;

            const uint32_t r = to_10bit(y + cv.c.r_cr * cr);
            const uint32_t g = to_10bit(y + cv.c.g_cb * cb + cv.c.g_cr * cr);
            const uint32_t b = to_10bit(y + cv.c.b_cb * cb);
            // WL_SHM_FORMAT_XBGR2101010
            out[ox] = (b << 20) | (g << 10) | r;
        }
    }
}

void convert(conversion &cv) {
    const int chroma_width = (cv.src_width + 1) / 2;
    const float sx_scale = (cv.crop.right - cv.crop.left) / cv.out_width;
    cv.columns.resize(cv.out_width);
    for (int ox = 0; ox < cv.out_width; ox++) {
        const float sx = std::clamp(cv.crop.left + (ox + 0.5f) * sx_scale - 0.5f, 0.0f, cv.src_width - 1.0f);
        auto &taps = cv.columns[ox];
        taps.x0 = static_cast<int>(sx);
        taps.x1 = std::min(taps.x0 + 1, cv.src_width - 1);
        taps.fx = sx - taps.x0;
        // Chroma is co-sited with the left luma sample
        const float cx = std::clamp(sx * 0.5f, 0.0f, chroma_width - 1.0f);
        taps.cx0 = static_cast<int>(cx);
        taps.cx1 = std::min(taps.cx0 + 1, chroma_width - 1);
        taps.fcx = cx - taps.cx0;
    }

    const int threads = std::clamp(static_cast<int>(std::thread::hardware_concurrency()) / 2, 1, 8);
    const int rows_per_thread = (cv.out_height + threads - 1) / threads;
    std::vector<std::thread> workers;
    for (int t = 1; t < threads; t++) {
        const int begin = t * rows_per_thread;
        const int end = std::min(cv.out_height, begin + rows_per_thread);
        if (begin < end)
            workers.emplace_back(convert_rows, std::cref(cv), begin, end);
    }
    convert_rows(cv, 0, std::min(cv.out_height, rows_per_thread));
    for (auto &worker : workers)
        worker.join();
}

const struct wl_buffer_listener buffer_listener {
    [](void *data, struct wl_buffer *) { static_cast<frame_buffer *>(data)->busy = false; }
};

std::unique_ptr<buffer> create_rgb10_buffer(struct display *display, frame_buffer *frame, int width, int height) {
    const int stride = width * 4;
    const int size = stride * height;
    int fd = syscall(SYS_memfd_create, "hdr-video", MFD_ALLOW_SEALING);
    if (fd < 0 || ftruncate(fd, size) != 0) {
        ALOGE("HDR: cannot allocate a %dx%d buffer", width, height);
        if (fd >= 0)
            close(fd);
        return nullptr;
    }
    void *data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        ALOGE("HDR: cannot map a %dx%d buffer", width, height);
        close(fd);
        return nullptr;
    }

    std::unique_ptr<buffer> buf { new buffer() };
    buf->handle = nullptr;
    buf->metadata = {static_cast<uint32_t>(height), static_cast<uint32_t>(width),
                     static_cast<uint32_t>(width), HAL_PIXEL_FORMAT_RGBA_1010102};
    buf->isShm = true;
    buf->shm_data = data;
    buf->size = size;

    struct wl_shm_pool *pool = wl_shm_create_pool(display->shm, fd, size);
    buf->wl_buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_XBGR2101010);
    wl_proxy_set_queue(reinterpret_cast<struct wl_proxy *>(buf->wl_buffer), display->hdr->buffer_queue);
    wl_buffer_add_listener(buf->wl_buffer, &buffer_listener, frame);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buf;
}

// A released buffer of the size, else a new one, else another released one, else none
frame_buffer *next_frame_buffer(struct display *display, hdr_window_state *state, int width, int height) {
    constexpr size_t kMaxBuffers = 4;
    wl_display_dispatch_queue_pending(display->display, display->hdr->buffer_queue);

    auto fits = [&](const frame_buffer &f) {
        return f.buf->metadata.width == static_cast<uint32_t>(width) && f.buf->metadata.height == static_cast<uint32_t>(height);
    };
    frame_buffer *frame = nullptr;
    for (auto &f : state->buffers) {
        if (!f->busy && fits(*f)) {
            frame = f.get();
            break;
        }
    }
    if (!frame && state->buffers.size() < kMaxBuffers) {
        frame = state->buffers.emplace_back(new frame_buffer()).get();
    }
    if (!frame) {
        for (auto &f : state->buffers) {
            if (!f->busy) {
                frame = f.get();
                break;
            }
        }
    }
    if (!frame)
        return nullptr;

    if (!frame->buf || !fits(*frame)) {
        frame->buf = create_rgb10_buffer(display, frame, width, height);
        frame->busy = false;
        if (!frame->buf) {
            state->buffers.erase(std::find_if(state->buffers.begin(), state->buffers.end(),
                                              [&](const auto &f) { return f.get() == frame; }));
            return nullptr;
        }
    }
    return frame;
}

hdr_window_state *window_state(struct window *window) {
    if (window->hdr)
        return window->hdr;

    auto *display = window->display;
    wl_surface *surface = wl_compositor_create_surface(display->compositor);
    wl_subsurface *subsurface = wl_subcompositor_get_subsurface(display->subcompositor, surface, window->surface);
    // Below the client target, which has a transparent hole for the video
    if (!window->layers.empty() && window->layers[0].surface != window->surface)
        wl_subsurface_place_below(subsurface, window->layers[0].surface);
    wp_viewport *viewport = wp_viewporter_get_viewport(display->viewporter, surface);

    auto *state = new hdr_window_state { window::layer(surface, viewport, subsurface) };
    state->color_surface = wp_color_manager_v1_get_surface(display->hdr->manager, surface);
    window->hdr = state;
    return state;
}

} // namespace

void hdr_bind_color_manager(struct display *display, struct wl_registry *registry, uint32_t id, uint32_t) {
    auto *hdr = new hdr_display_state();
    hdr->display = display;
    hdr->buffer_queue = wl_display_create_queue(display->display);
    hdr->manager = static_cast<struct wp_color_manager_v1 *>(
            wl_registry_bind(registry, id, &wp_color_manager_v1_interface, 1));
    wp_color_manager_v1_add_listener(hdr->manager, &manager_listener, hdr);
    display->hdr = hdr;
}

void hdr_init(struct display *display) {
    auto *hdr = display->hdr;
    if (!hdr || !display->output || !display->shm)
        return;
    if (property_get_bool("persist.waydroid.no_hdr", false))
        return;
    // The video needs a subsurface below the window's content
    hdr->usable = display->subcompositor && display->viewporter
            && !property_get_bool("persist.waydroid.no_background_subsurface", false);

    // Delivers the supported features, transfer functions and primaries
    wl_display_roundtrip(display->display);
    if (!hdr->parametric || !contains(hdr->primaries, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020)) {
        ALOGI("HDR: the compositor cannot take BT.2020 parametric image descriptions");
        hdr->usable = false;
        return;
    }
    if (!display->shm_xbgr2101010) {
        ALOGI("HDR: the compositor cannot take 10-bit wl_shm buffers");
        hdr->usable = false;
        return;
    }

    hdr->output = wp_color_manager_v1_get_output(hdr->manager, display->output);
    wp_color_management_output_v1_add_listener(hdr->output, &output_listener, hdr);
    request_output_description(hdr);
    create_video_description(hdr, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ);
    create_video_description(hdr, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_HLG);

    // The output description takes two round trips: the description, then its information
    for (int i = 0; i < 4; i++) {
        wl_display_roundtrip(display->display);
        std::scoped_lock lock(hdr->mutex);
        if (hdr->current_valid)
            break;
    }
}

hdr_capabilities hdr_get_capabilities(struct display *display) {
    hdr_capabilities caps;
    auto *hdr = display->hdr;
    if (!hdr || !hdr->usable)
        return caps;

    output_info info;
    {
        std::scoped_lock lock(hdr->mutex);
        if (!hdr->current_valid)
            return caps;
        info = hdr->current;
    }
    // Only an output in HDR mode shows more than SDR
    if (info.tf != WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ && info.tf != WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_HLG)
        return caps;

    if (ready_video_description(hdr, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ))
        caps.types.push_back(HAL_HDR_HDR10);
    if (ready_video_description(hdr, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_HLG))
        caps.types.push_back(HAL_HDR_HLG);

    caps.max_luminance = info.max_cll > 0 ? info.max_cll
            : info.target_max_luminance > 0 ? info.target_max_luminance : info.max_luminance;
    caps.max_average_luminance = info.max_fall > 0 ? std::min(info.max_fall, caps.max_luminance) : caps.max_luminance;
    caps.min_luminance = info.target_max_luminance > 0 ? info.target_min_luminance : info.min_luminance;
    return caps;
}

bool hdr_is_video_layer(waydroid_hwc_composer_device_1 *pdev, const hwc_layer_1 &layer, size_t index) {
    if (!layer.handle)
        return false;
    auto it = pdev->display->layer_handles_ext.find(index);
    return it != pdev->display->layer_handles_ext.end() && it->second.format == HAL_PIXEL_FORMAT_YCBCR_P010;
}

int hdr_apply_layer_to_window(waydroid_hwc_composer_device_1 *pdev, hwc_layer_1 *layer, size_t index, struct window *window) {
    constexpr int acquireTimeoutMS = 1000;
    auto *display = pdev->display;
    int res = -1;
    hdr_window_state *state = nullptr;
    frame_buffer *frame = nullptr;
    conversion cv {};
    const video_description *description = nullptr;
    const int32_t dataspace = layer_dataspace(pdev, index);
    const uint32_t tf = dataspace_transfer_function(dataspace);
    const frame_source source {layer->handle, dataspace, layer->sourceCropf, layer->displayFrame};

    layer->releaseFenceFd = -1;
    if (!display->hdr || !display->hdr->usable)
        goto out;
    description = ready_video_description(display->hdr, tf);

    // SurfaceFlinger redraws the UI over a paused video with the same buffer, and a buffer queue
    // never hands out the buffer on screen again, so the same handle is the same frame
    state = window_state(window);
    if (state->attached && state->shown == source) {
        state->used = true;
        res = 0;
        goto out;
    }

    {
        const auto &ext = display->layer_handles_ext[index];
        cv.crop = layer->sourceCropf;
        cv.src_width = ext.width ? ext.width : static_cast<int>(ceil(cv.crop.right));
        cv.src_height = ext.height ? ext.height : static_cast<int>(ceil(cv.crop.bottom));
        const int frame_width = layer->displayFrame.right - layer->displayFrame.left;
        const int frame_height = layer->displayFrame.bottom - layer->displayFrame.top;
        const float crop_width = cv.crop.right - cv.crop.left;
        const float crop_height = cv.crop.bottom - cv.crop.top;
        if (frame_width <= 0 || frame_height <= 0 || crop_width < 1 || crop_height < 1 || cv.src_width <= 0 || cv.src_height <= 0)
            goto out;
        // Never upscale here: the compositor scales from the viewport
        cv.out_width = std::min(frame_width, static_cast<int>(ceil(crop_width)));
        cv.out_height = std::min(frame_height, static_cast<int>(ceil(crop_height)));
    }

    if (layer->acquireFenceFd != -1) {
        if (sync_wait(layer->acquireFenceFd, acquireTimeoutMS) < 0) {
            ALOGE("HDR: video buffer not ready after %d ms", acquireTimeoutMS);
            goto out;
        }
    }

    frame = next_frame_buffer(display, state, cv.out_width, cv.out_height);
    if (!frame) {
        // The compositor holds every buffer, or none could be allocated: drop this frame and
        // keep the one on screen
        state->used = state->attached;
        res = state->attached ? 0 : -1;
        goto out;
    }

    {
        android::Rect bounds(cv.src_width, cv.src_height);
        if (android::GraphicBufferMapper::get().lockYCbCr(layer->handle, GRALLOC_USAGE_SW_READ_OFTEN, bounds, &cv.src) != 0) {
            ALOGE("HDR: cannot lock the video buffer");
            goto out;
        }
        cv.dst = static_cast<uint32_t *>(frame->buf->shm_data);
        cv.c = coefficients_for(dataspace);
        convert(cv);
        android::GraphicBufferMapper::get().unlock(layer->handle);
    }

    if (state->tagged_tf != (description ? tf : 0)) {
        if (description)
            wp_color_management_surface_v1_set_image_description(state->color_surface, description->description,
                                                                 WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
        else
            wp_color_management_surface_v1_unset_image_description(state->color_surface);
        state->tagged_tf = description ? tf : 0;
    }

    state->layer.attach_buffer(*frame->buf);
    frame->busy = true;
    state->shown = source;
    wl_surface_damage(state->layer.surface, 0, 0, INT32_MAX, INT32_MAX);
    state->layer.set_crop({0, 0, static_cast<float>(cv.out_width), static_cast<float>(cv.out_height)});
    state->layer.set_display_frame(layer->displayFrame, display->scale);
    state->layer.set_position(floor(layer->displayFrame.left / display->scale),
                              floor(layer->displayFrame.top / display->scale));
    wl_surface_commit(state->layer.surface);

    display->layers[state->layer.surface] = {
            .x = layer->displayFrame.left,
            .y = layer->displayFrame.top };
    state->attached = true;
    state->used = true;
    res = 0;

out:
    if (layer->acquireFenceFd != -1) {
        close(layer->acquireFenceFd);
        layer->acquireFenceFd = -1;
    }
    return res;
}

void hdr_finish_window_frame(struct window *window) {
    auto *state = window->hdr;
    if (!state)
        return;
    // A window that drew nothing this frame keeps its snapshot, and the video frame under it
    if (!state->used && state->attached && window->lastLayer > 0) {
        wl_surface_attach(state->layer.surface, nullptr, 0, 0);
        wl_surface_commit(state->layer.surface);
        state->attached = false;
        state->shown = {};
    }
    state->used = false;
}

void hdr_destroy_window(struct window *window) {
    auto *state = window->hdr;
    if (!state)
        return;
    window->display->layers.erase(state->layer.surface);
    wp_color_management_surface_v1_destroy(state->color_surface);
    delete state;
    window->hdr = nullptr;
}

extern "C" __attribute__((visibility("default")))
int waydroid_hwc_get_hdr_capabilities(struct hwc_composer_device_1 *dev, int disp, uint32_t *num_types,
        int32_t *types, float *max_luminance, float *max_average_luminance, float *min_luminance) {
    auto *pdev = static_cast<waydroid_hwc_composer_device_1 *>(dev);
    hdr_capabilities caps;
    if (disp == HWC_DISPLAY_PRIMARY)
        caps = hdr_get_capabilities(pdev->display);

    if (types) {
        *num_types = std::min<uint32_t>(*num_types, caps.types.size());
        std::copy_n(caps.types.begin(), *num_types, types);
    } else {
        *num_types = caps.types.size();
    }
    if (max_luminance)
        *max_luminance = caps.max_luminance;
    if (max_average_luminance)
        *max_average_luminance = caps.max_average_luminance;
    if (min_luminance)
        *min_luminance = caps.min_luminance;
    return 0;
}

extern "C" __attribute__((visibility("default")))
void waydroid_hwc_set_layer_dataspaces(struct hwc_composer_device_1 *dev, int disp,
        const int32_t *dataspaces, size_t count) {
    auto *pdev = static_cast<waydroid_hwc_composer_device_1 *>(dev);
    if (disp == HWC_DISPLAY_PRIMARY)
        pdev->layer_dataspaces.assign(dataspaces, dataspaces + count);
}

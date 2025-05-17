/*
 * Copyright (C) 2012 The Android Open Source Project
 * Copyright (C) 2021 The Waydroid Project
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

#include "hwcomposer.h"

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <unistd.h>
#include <wayland-client.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <string>
#include <sstream>
#include <functional>

#include <log/log.h>
#include <cutils/properties.h>
#include <hardware/hwcomposer.h>
#include <libsync/sw_sync.h>
#include <sync/sync.h>
#include <drm_fourcc.h>
#include <presentation-time-client-protocol.h>
#include <viewporter-client-protocol.h>
#include <gralloc_handle.h>
#include <cros_gralloc/cros_gralloc_handle.h>

#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#include <cutils/trace.h>
#include <utils/Trace.h>

#include "WaydroidClipboard.h"
#include "WaydroidWindow.h"
#include "gralloc_handler.h"
#include "egl-tools.h"
#include "extension.h"

#include "modes/closed.h"
#include "modes/full-ui.h"
#include "modes/multi-window.h"
#include "modes/single-window.h"

using ::android::hardware::configureRpcThreadpool;
using ::android::hardware::joinRpcThreadpool;

using ::vendor::waydroid::display::V1_2::IWaydroidDisplay;
using ::vendor::waydroid::display::V1_2::implementation::WaydroidDisplay;
using ::vendor::waydroid::window::V1_1::IWaydroidWindow;
using ::vendor::waydroid::window::implementation::WaydroidWindow;
using ::vendor::waydroid::clipboard::V1_0::IWaydroidClipboard;
using ::vendor::waydroid::clipboard::implementation::WaydroidClipboard;

using ::android::OK;
using ::android::status_t;

#define WINDOW_DECORATION_OUTSET 15

namespace {
    std::pair<int, int> search_first_and_last_skipped_layer(hwc_display_contents_1_t *contents) {
        assert(contents->numHwLayers <= std::numeric_limits<int>::max());

        int first = -1;
        int last = -1;
        for (int i = 0; i < contents->numHwLayers; i++) {
            if (!(contents->hwLayers[i].flags & HWC_SKIP_LAYER))
                continue;

            if (first == -1)
                first = i;
            last = i;
        }
        return {first, last};
    }

    hwc_frect_t rect_apply_transform(hwc_frect_t src, uint32_t transform) {
        /* Transform bits are defined so that for both 90° and 270° this bit is set */
        if (transform & HWC_TRANSFORM_ROT_90) {
            return hwc_frect_t {
                .left = src.top,
                .top = src.left,
                .right = src.bottom,
                .bottom = src.right
            };
        }
        return src;
    }

    buffer *find_cached_buffer(waydroid_hwc_composer_device_1 *pdev, const buffer_metadata &metadata, buffer_handle_t handle) {
        auto it = pdev->display->buffer_map.find(handle);
        if (it != pdev->display->buffer_map.end()) {
            /* FIXME We can't be sure that our cached buffer actually refers to the buffer corresponding to the given handle
             * It's possible that a new buffer got the same handle after the old one was destroyed
             * At least check for the metadata to match. This way this situation is hopefully unlikely */
            if (it->second->metadata != metadata) {
                pdev->display->buffer_map.erase(it);
            } else {
                return it->second.get();
            }
        }
        return nullptr;
    }

    buffer *get_wl_buffer(waydroid_hwc_composer_device_1 *pdev, hwc_layer_1_t *layer, size_t pos) {
        const auto& gralloc_handler = pdev->gralloc_handler;
        auto metadata = gralloc_handler.get_buffer_metadata(pdev->display, layer, pos);
        buffer *buf = find_cached_buffer(pdev, metadata, layer->handle);

        if (!buf) {
            auto result = gralloc_handler.create_buffer(pdev->display, metadata, layer->handle);
            if (!result) {
                ALOGE("failed to create a wayland buffer");
                return nullptr;
            }
            auto emplace_result = pdev->display->buffer_map.emplace(layer->handle, std::move(result));
            assert(emplace_result.second);
            buf = emplace_result.first->second.get();
        }

        if (buf->isShm)
            gralloc_handler.update_shm_buffer(pdev->display, buf);
        return buf;
    }

    std::string property_get_string(const char *key, const char *default_value) {
        char property[PROPERTY_VALUE_MAX];
        int size = property_get(key, property, default_value);
        return std::string(property, size);
    }

    int32_t hwc_transform_to_wayland_transform(uint32_t hwc_transform) {
        switch (hwc_transform) {
            case HWC_TRANSFORM_FLIP_H:
                return WL_OUTPUT_TRANSFORM_FLIPPED_180;
            case HWC_TRANSFORM_FLIP_V:
                return WL_OUTPUT_TRANSFORM_FLIPPED;
            case HWC_TRANSFORM_ROT_90:
                return WL_OUTPUT_TRANSFORM_90;
            case HWC_TRANSFORM_ROT_180:
                return WL_OUTPUT_TRANSFORM_180;
            case HWC_TRANSFORM_ROT_270:
                return WL_OUTPUT_TRANSFORM_270;
            case HWC_TRANSFORM_FLIP_H_ROT_90:
                return WL_OUTPUT_TRANSFORM_FLIPPED_270;
            case HWC_TRANSFORM_FLIP_V_ROT_90:
                return WL_OUTPUT_TRANSFORM_FLIPPED_90;
            default:
                return WL_OUTPUT_TRANSFORM_NORMAL;
        }
    }
}

enum class ShowWindowState {
    NONE,
    BLACKLISTED,
    YES
};

static int hwc_prepare(hwc_composer_device_1_t* dev,
                       size_t numDisplays, hwc_display_contents_1_t** displays) {
    if (HWC_DISPLAY_PRIMARY >= numDisplays || !displays)
        return 0;

    auto *pdev = static_cast<waydroid_hwc_composer_device_1 *>(dev);

    hwc_display_contents_1_t *contents = displays[HWC_DISPLAY_PRIMARY];
    assert(contents);

    std::pair<int, int> skipped = search_first_and_last_skipped_layer(contents);

    for (size_t i = 0; i < contents->numHwLayers; i++) {
        if (contents->hwLayers[i].flags & HWC_IS_CURSOR_LAYER) {
            contents->hwLayers[i].compositionType = HWC_OVERLAY;
            continue;
        }
        if (contents->hwLayers[i].compositionType == HWC_FRAMEBUFFER_TARGET)
            continue;
        if (contents->hwLayers[i].flags & HWC_SKIP_LAYER)
            continue;

        /* skipped layers have to be composited by SurfaceFlinger; so in order
           have correct z-ordering, we must ask SurfaceFlinger to composite
           everything between the first and the last skipped layer. Unfortunately,
           this can't be done in multi windows mode, which relies on layers not
           being composited, so we won't render skipped layers correctly in that mode */
        if (!pdev->multi_windows && skipped.first != -1) {
            if (skipped.first <= i && i <= skipped.second) {
                contents->hwLayers[i].compositionType = HWC_FRAMEBUFFER;
                continue;
            }
        }

        /* If we do composition then request a buffer for every possible layer
         * otherwise instruct SurfaceFlinger to compose everything itself */
        if (pdev->should_compose) {
            if (contents->hwLayers[i].compositionType == HWC_FRAMEBUFFER) {
                contents->hwLayers[i].compositionType = HWC_OVERLAY;
            }
            // TODO: Handle HWC_SIDEBAND
        } else {
            contents->hwLayers[i].compositionType = HWC_FRAMEBUFFER;
        }
    }

    return 0;
}

static void setup_viewport_source(wp_viewport *viewport, hwc_frect_t crop, uint32_t transform)
{
    hwc_frect_t sourceCrop = rect_apply_transform(crop, transform);
    wp_viewport_set_source(viewport,
                           wl_fixed_from_double(fmax(0, sourceCrop.left)),
                           wl_fixed_from_double(fmax(0, sourceCrop.top)),
                           wl_fixed_from_double(fmax(1, sourceCrop.right - sourceCrop.left)),
                           wl_fixed_from_double(fmax(1, sourceCrop.bottom - sourceCrop.top)));
}

static void setup_viewport_destination(wp_viewport *viewport, hwc_rect_t frame, struct display *display)
{
    int width = static_cast<int>(ceil((frame.right - frame.left) / display->scale));
    int height = static_cast<int>(ceil((frame.bottom - frame.top) / display->scale));
    wp_viewport_set_destination(viewport,
                                std::max(1, width),
                                std::max(1, height));
}

static window::layer &get_next_window_layer(struct waydroid_hwc_composer_device_1 *pdev, hwc_layer_1_t *layer, struct window *window)
{
    if (window->lastLayer >= window->layers.size()) {
        assert(window->lastLayer == window->layers.size());
        window->create_new_layer();
    }
    window::layer &requested_layer = window->layers[window->lastLayer];

    if (window->input_region) {
        wl_region_add(window->input_region,
                -WINDOW_DECORATION_OUTSET + floor(layer->displayFrame.left / pdev->display->scale),
                -WINDOW_DECORATION_OUTSET + floor(layer->displayFrame.top / pdev->display->scale),
                2*WINDOW_DECORATION_OUTSET + ceil((layer->displayFrame.right - layer->displayFrame.left) / pdev->display->scale),
                2*WINDOW_DECORATION_OUTSET + ceil((layer->displayFrame.bottom - layer->displayFrame.top) / pdev->display->scale));
    }

    pdev->display->layers[requested_layer.surface] = {
        .x = layer->displayFrame.left,
        .y = layer->displayFrame.top };

    return requested_layer;
}

static long time_to_sleep_to_next_vsync(struct timespec *rt, uint64_t last_vsync_ns, unsigned vsync_period_ns)
{
    uint64_t now = (uint64_t)rt->tv_sec * 1e9 + rt->tv_nsec;
    uint64_t frames_since_last_vsync = (now - last_vsync_ns) / vsync_period_ns + 1;
    uint64_t next_vsync = last_vsync_ns + frames_since_last_vsync * vsync_period_ns;

    return next_vsync - now;
}

static void* hwc_vsync_thread(void* data) {
    auto *pdev = static_cast<waydroid_hwc_composer_device_1 *>(data);
    setpriority(PRIO_PROCESS, 0, HAL_PRIORITY_URGENT_DISPLAY);

    struct timespec rt;
    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in vsync thread clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }
    bool vsync_enabled = false;

    struct timespec wait_time;
    wait_time.tv_sec = 0;
    wait_time.tv_nsec = time_to_sleep_to_next_vsync(&rt, pdev->last_vsync_ns, pdev->vsync_period_ns);

    while (true) {
        ATRACE_BEGIN("hwc_vsync_thread");
        int err = nanosleep(&wait_time, NULL);
        if (err == -1) {
            if (errno == EINTR) {
                break;
            }
            ATRACE_END();
            ALOGE("error in vsync thread: %s", strerror(errno));
            continue;
        }

        vsync_enabled = pdev->vsync_callback_enabled;

        if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
            ALOGE("%s:%d error in vsync thread clock_gettime: %s",
                  __FILE__, __LINE__, strerror(errno));
        }

        wait_time.tv_nsec = time_to_sleep_to_next_vsync(&rt, pdev->last_vsync_ns, pdev->vsync_period_ns);

        if (!vsync_enabled || !pdev->procs || !pdev->procs->vsync) {
            ATRACE_END();
            continue;
        }

        int64_t timestamp = (uint64_t)rt.tv_sec * 1e9 + rt.tv_nsec;
        pdev->procs->vsync(pdev->procs, 0, timestamp);
        ATRACE_END();
    }

    return NULL;
}

static void
feedback_sync_output(void *, struct wp_presentation_feedback *,
             struct wl_output *)
{
}

static void
feedback_presented(void *data,
           struct wp_presentation_feedback *feedback,
           uint32_t tv_sec_hi,
           uint32_t tv_sec_lo,
           uint32_t tv_nsec,
           uint32_t,
           uint32_t,
           uint32_t,
           uint32_t)
{
    auto *pdev = static_cast<waydroid_hwc_composer_device_1 *>(data);
    wp_presentation_feedback_destroy(feedback);

    pdev->last_vsync_ns = (((uint64_t)tv_sec_hi << 32) + tv_sec_lo) * 1e9 + tv_nsec;
}

static void
feedback_discarded(void *, struct wp_presentation_feedback *feedback)
{
    wp_presentation_feedback_destroy(feedback);
}

static const struct wp_presentation_feedback_listener feedback_listener = {
    feedback_sync_output,
    feedback_presented,
    feedback_discarded
};

bool is_blacklisted(struct waydroid_hwc_composer_device_1* pdev, const std::string &app_id, const std::string &component) {
    auto match = pdev->blacklisted_apps.find(app_id);
    if (match == pdev->blacklisted_apps.end())
        return false;
    auto &components = match->second;
    return components.empty() || std::find(components.begin(), components.end(), component) != components.end();
}

static void apply_surface_scale(waydroid_hwc_composer_device_1 *pdev, hwc_layer_1 * hwc_layer, surface_context &surface_context) {
    if (surface_context.viewport) {
        setup_viewport_source(surface_context.viewport, hwc_layer->sourceCropf, hwc_layer->transform);
        setup_viewport_destination(surface_context.viewport, hwc_layer->displayFrame, pdev->display);
    } else {
        // Usually with no viewporter the scale is guaranteed to be integer
        // When supports_cursor_viewport == false, this might not be the case
        // thus use ceil anyway
        int scale = static_cast<int>(ceil(pdev->display->scale));
        wl_surface_set_buffer_scale(surface_context.surface, scale);
    }
}

static int apply_hwc_layer_to_surface_context(waydroid_hwc_composer_device_1 *pdev, hwc_layer_1 *hwc_layer, size_t hwc_layer_index, surface_context &surface_context, buffer *buf = nullptr) {
    constexpr int acquireWarningMS = 100;
    int res = -1;

    if (!buf) {
        buf = get_wl_buffer(pdev, hwc_layer, hwc_layer_index);
        if (!buf) {
            ALOGE("Failed to get wayland buffer");
            goto out;
        }
    }

    // TODO: Implement per-hwc_layer explicit synchronization
    hwc_layer->releaseFenceFd = -1;

    wl_surface_attach(surface_context.surface, buf->wl_buffer, 0, 0);
    wl_surface_damage(surface_context.surface, 0, 0, INT32_MAX, INT32_MAX);
    apply_surface_scale(pdev, hwc_layer, surface_context);
    wl_surface_set_buffer_transform(surface_context.surface, hwc_transform_to_wayland_transform(hwc_layer->transform));

    // TODO: Implement explicit synchronization
    if (hwc_layer->acquireFenceFd != -1) {
        res = sync_wait(hwc_layer->acquireFenceFd, acquireWarningMS);
        if (res < 0 && errno == ETIME) {
            ALOGE("hwcomposer waited on fence %d for %d ms", hwc_layer->acquireFenceFd,
                  acquireWarningMS);
        }
    } else {
        res = 0;
    }

    wl_surface_commit(surface_context.surface);

out:
    if (hwc_layer->acquireFenceFd != -1) {
        close(hwc_layer->acquireFenceFd);
    }
    return res;
}

int apply_hwc_layer_to_window(waydroid_hwc_composer_device_1 *pdev, hwc_layer_1 *hwc_layer, size_t hwc_layer_index, window *window) {
    auto &window_layer = get_next_window_layer(pdev, hwc_layer, window);

    buffer *buf = get_wl_buffer(pdev, hwc_layer, hwc_layer_index);
    if (!buf) {
        ALOGE("Failed to get wayland buffer");
        if (hwc_layer->acquireFenceFd != -1) {
            close(hwc_layer->acquireFenceFd);
        }
        return -1;
    }

    window->lastLayer++;
    window->last_layer_buffer = buf;

    if (apply_hwc_layer_to_surface_context(pdev, hwc_layer, hwc_layer_index, window_layer, buf) != 0) {
        return -1;
    }

    if (window_layer.subsurface) {
        wl_subsurface_set_position(window_layer.subsurface,
                                   floor(hwc_layer->displayFrame.left / pdev->display->scale),
                                   floor(hwc_layer->displayFrame.top / pdev->display->scale));
    }

    if (window->display->presentation) {
        buf->feedback = wp_presentation_feedback(window->display->presentation, window_layer.surface);
        wp_presentation_feedback_add_listener(buf->feedback,
                                              &feedback_listener, pdev);
    }

    // Snapshot buffer should be detached by now, clean up
    window->snapshot_buffer = nullptr;

    return 0;
}

static std::unique_ptr<waydroid_mode> select_mode(waydroid_hwc_composer_device_1 *pdev, const std::string &active_apps) {
    /*
     * In prop "persist.waydroid.multi_windows" we detect HWC let SF render layers
     * And just show the target client layer (single windows mode) or
     * render each layers in wayland surface and subsurfaces.
     * In prop "waydroid.active_apps" we choose what to be shown in window
     * and here if HWC is in single mode we show the screen only if any task are in screen
     * and in multi windows mode we group layers with same task ID in a wayland window.
     *
     * "waydroid.active_apps" prop can be:
     * "none": No windows
     * "Waydroid": Shows android screen in a single window
     * "AppID": Shows apps in related windows as explained above
     */
    waydroid_mode *mode;
    if (active_apps == "none") {
        mode = new closed_mode();
    } else if (active_apps == "Waydroid") {
        if (pdev->should_compose) {
            mode = new compositing_full_ui_mode();
        } else {
            mode = new non_compositing_full_ui_mode();
        }
    } else if (!pdev->multi_windows) {
        if (pdev->should_compose) {
            mode = new compositing_single_window_mode();
        } else {
            mode = new non_compositing_single_window_mode();
        }
    } else {
        assert(pdev->should_compose);
        mode = new multi_window_mode();
    }
    return std::unique_ptr<waydroid_mode>(mode);
}

static void reset_per_commit_state_window(waydroid_hwc_composer_device_1 *pdev) {
    for (auto& [id, window] : pdev->display->windows) {
        window->lastLayer = 0;
        window->last_layer_buffer = nullptr;
        if (window->input_region) {
            wl_region_subtract(window->input_region, 0, 0, INT_MAX, INT_MAX);
        }
    }
}

static int hwc_set(struct hwc_composer_device_1* dev,size_t numDisplays,
                   hwc_display_contents_1_t** displays) {
    if (HWC_DISPLAY_PRIMARY >= numDisplays || !displays)
        return 0;

    auto *pdev = static_cast<waydroid_hwc_composer_device_1 *>(dev);

    hwc_display_contents_1_t* contents = displays[HWC_DISPLAY_PRIMARY];
    assert(contents);

    std::string active_apps = property_get_string("waydroid.active_apps", "none");
    if (active_apps != "Waydroid" && !property_get_bool("waydroid.background_start", true)) {
        for (size_t l = 0; l < contents->numHwLayers; l++) {
            const auto &layer_name = pdev->display->layer_names[l];
            if (layer_name.rfind("BootAnimation#", 0) == 0) {
                // force single window mode during boot animation
                active_apps = "Waydroid";
                break;
            }
        }
    }
    std::unique_ptr<waydroid_mode> mode = select_mode(pdev, active_apps);

    mode->setup(pdev, contents);

    std::scoped_lock lock(pdev->display->windowsMutex);
    mode->cleanup_stale_windows(pdev, contents);

    reset_per_commit_state_window(pdev);

    bool found_cursor = false;
    for (size_t l = 0; l < contents->numHwLayers; l++) {
        auto *layer = &contents->hwLayers[l];
        if (layer->flags & HWC_IS_CURSOR_LAYER) {
            found_cursor = true;
            pdev->display->cursor_handler->apply_cursor(pdev, layer, l);
        } else {
            mode->handle_layer(pdev, layer, l);
        }
    }
    if (!found_cursor) {
        pdev->display->cursor_handler->reset_cursor(pdev);
    }

    mode->post_processing(pdev, contents);

    for (auto& [key, window] : pdev->display->windows) {
        if (window->input_region) {
            wl_surface_set_input_region(window->surface, window->input_region);
        }
        wl_surface_commit(window->surface);
    }
    wl_display_flush(pdev->display->display);

    sw_sync_timeline_inc(pdev->timeline_fd, 1);
    contents->retireFenceFd = sw_sync_fence_create(pdev->timeline_fd, "hwc_contents_release", ++pdev->next_sync_point);
    return 0;
}

static int hwc_query(struct hwc_composer_device_1* dev, int what, int* value) {
    auto *pdev = static_cast<waydroid_hwc_composer_device_1 *>(dev);
    switch (what) {
        case HWC_VSYNC_PERIOD:
            value[0] = pdev->vsync_period_ns;
            break;
        default:
            // unsupported query
            ALOGE("%s badness unsupported query what=%d", __FUNCTION__, what);
            return -EINVAL;
    }
    return 0;
}

static int hwc_event_control(struct hwc_composer_device_1* dev, int dpy __unused,
                             int event, int enabled) {
    auto *pdev = static_cast<waydroid_hwc_composer_device_1 *>(dev);
    int ret = -EINVAL;

    // enabled can only be 0 or 1
    if (!(enabled & ~1)) {
        if (event == HWC_EVENT_VSYNC) {
            pdev->vsync_callback_enabled = enabled;
            ret = 0;
        }
    }
    return ret;
}

static int hwc_blank(struct hwc_composer_device_1* dev __unused, int disp __unused,
                     int blank __unused) {
    return 0;
}

static void hwc_dump(hwc_composer_device_1* dev __unused, char* buff __unused,
                     int buff_len __unused) {
    // This is run when running dumpsys.
    // No-op for now.
}


static int hwc_get_display_configs(struct hwc_composer_device_1* dev __unused,
                                   int disp, uint32_t* configs, size_t* numConfigs) {
    if (*numConfigs == 0) {
        return 0;
    }

    if (disp == HWC_DISPLAY_PRIMARY) {
        configs[0] = 0;
        *numConfigs = 1;
        return 0;
    }

    return -EINVAL;
}

static int hwc_get_active_config(struct hwc_composer_device_1* dev __unused, int disp) {
    if (disp == HWC_DISPLAY_PRIMARY)
        return 0;
    return -EINVAL;
}

static int hwc_set_active_config(struct hwc_composer_device_1* dev __unused, int disp, int index) {
    if (disp == HWC_DISPLAY_PRIMARY && index == 0)
        return 0;
    return -EINVAL;
}

static int hwc_set_cursor_position_async(struct hwc_composer_device_1 *, int, int, int) {
    // Ignored: Wayland compositor is managing the cursor position
    return 0;
}

static int32_t hwc_attribute(struct waydroid_hwc_composer_device_1* pdev,
                             const uint32_t attribute) {
    char property[PROPERTY_VALUE_MAX];
    int width = floor(pdev->display->width * pdev->display->scale);
    int height = floor(pdev->display->height * pdev->display->scale);
    int density = 180;

    switch(attribute) {
        case HWC_DISPLAY_VSYNC_PERIOD:
            return pdev->vsync_period_ns;
        case HWC_DISPLAY_WIDTH: {
            if (property_get("persist.waydroid.width_padding", property, nullptr) > 0)
                width -= atoi(property);
            std::string width_str = std::to_string(width);
            property_set("waydroid.display_width", width_str.c_str());
            return width;
        }
        case HWC_DISPLAY_HEIGHT: {
            if (property_get("persist.waydroid.height_padding", property, nullptr) > 0)
                height -= atoi(property);
            std::string height_str = std::to_string(height);
            property_set("waydroid.display_height", height_str.c_str());
            return height;
        }
        case HWC_DISPLAY_DPI_X:
        case HWC_DISPLAY_DPI_Y:
            if (property_get("ro.sf.lcd_density", property, nullptr) > 0)
                density = atoi(property);
            return density * 1000;
        case HWC_DISPLAY_COLOR_TRANSFORM:
            return HAL_COLOR_TRANSFORM_IDENTITY;
        default:
            ALOGE("unknown display attribute %u", attribute);
            return -EINVAL;
    }
}

static int hwc_get_display_attributes(struct hwc_composer_device_1* dev __unused,
                                      int disp, uint32_t config __unused,
                                      const uint32_t* attributes, int32_t* values) {
    auto *pdev = static_cast<waydroid_hwc_composer_device_1 *>(dev);
    for (int i = 0; attributes[i] != HWC_DISPLAY_NO_ATTRIBUTE; i++) {
        if (disp == HWC_DISPLAY_PRIMARY) {
            values[i] = hwc_attribute(pdev, attributes[i]);
            if (values[i] == -EINVAL) {
                return -EINVAL;
            }
        } else {
            ALOGE("unknown display type %u", disp);
            return -EINVAL;
        }
    }

    return 0;
}

static int hwc_close(hw_device_t* dev) {
    auto *pdev = reinterpret_cast<waydroid_hwc_composer_device_1 *>(dev);

    pdev->display->buffer_map.clear();

    destroy_display(pdev->display);

    pthread_kill(pdev->wayland_thread, SIGTERM);
    pthread_join(pdev->wayland_thread, NULL);

    delete dev;
    return 0;
}

static void* hwc_wayland_thread(void* data) {
    auto *pdev = static_cast<waydroid_hwc_composer_device_1 *>(data);
    int ret = 0;

    setpriority(PRIO_PROCESS, 0, HAL_PRIORITY_URGENT_DISPLAY);

    while (ret != -1)
        ret = wl_display_dispatch(pdev->display->display);

    ALOGE("*** %s: Wayland client was disconnected: %s", __PRETTY_FUNCTION__, strerror(ret));

    return NULL;
}

static void* hwc_binder_thread(void* data) {
    auto *pdev = static_cast<waydroid_hwc_composer_device_1 *>(data);
    status_t status;

    sp<IWaydroidDisplay> waydroidDisplay;
    sp<IWaydroidWindow> waydroidWindow;
    sp<IWaydroidClipboard> waydroidClipboard;

    setpriority(PRIO_PROCESS, 0, HAL_PRIORITY_URGENT_DISPLAY);
    configureRpcThreadpool(1, true /*callerWillJoin*/);

    waydroidDisplay = new WaydroidDisplay(pdev->display);
    if (waydroidDisplay == nullptr) {
        ALOGE("Can not create an instance of Waydroid Display HAL, exiting.");
        goto shutdown;
    }
    status = waydroidDisplay->registerAsService();
    if (status != OK) {
        ALOGE("Could not register service for Waydroid Display HAL (%d).", status);
        goto shutdown;
    }

    waydroidWindow = new WaydroidWindow(pdev->display);
    if (waydroidWindow == nullptr) {
        ALOGE("Can not create an instance of Waydroid Window HAL, exiting.");
        goto shutdown;
    }
    status = waydroidWindow->registerAsService();
    if (status != OK) {
        ALOGE("Could not register service for Waydroid Window HAL (%d).", status);
        goto shutdown;
    }

    waydroidClipboard = new WaydroidClipboard(pdev->display);
    if (waydroidClipboard == nullptr) {
        ALOGE("Can not create an instance of Waydroid Clipboard HAL, exiting.");
        goto shutdown;
    }
    status = waydroidClipboard->registerAsService();
    if (status != OK) {
        ALOGE("Could not register service for Waydroid Clipboard HAL (%d).", status);
        goto shutdown;
    }

    ALOGI("Waydroid hwcomposer services are ready.");
    joinRpcThreadpool();
    // Should not pass this line

shutdown:
    // In normal operation, we don't expect the thread pool to shutdown
    ALOGE("Waydroid hwcomposer services shutting down.");
    return NULL;
}

static void hwc_register_procs(struct hwc_composer_device_1* dev,
                               hwc_procs_t const* procs) {
    auto *pdev = static_cast<waydroid_hwc_composer_device_1 *>(dev);
    pdev->procs = procs;
}

static int hwc_open(const struct hw_module_t* module, const char* name,
                    struct hw_device_t** device) {
    int ret = 0;
    char property[PROPERTY_VALUE_MAX];

    if (strcmp(name, HWC_HARDWARE_COMPOSER)) {
        ALOGE("%s called with bad name %s", __FUNCTION__, name);
        return -EINVAL;
    }

    waydroid_hwc_composer_device_1 *pdev = new waydroid_hwc_composer_device_1();
    if (!pdev) {
        ALOGE("%s failed to allocate dev", __FUNCTION__);
        return -ENOMEM;
    }

    pdev->common.tag = HARDWARE_DEVICE_TAG;
    pdev->common.version = HWC_DEVICE_API_VERSION_1_4;
    pdev->common.module = const_cast<hw_module_t *>(module);
    pdev->common.close = hwc_close;

    pdev->prepare = hwc_prepare;
    pdev->set = hwc_set;
    pdev->eventControl = hwc_event_control;
    pdev->blank = hwc_blank;
    pdev->query = hwc_query;
    pdev->registerProcs = hwc_register_procs;
    pdev->dump = hwc_dump;
    pdev->getDisplayConfigs = hwc_get_display_configs;
    pdev->getDisplayAttributes = hwc_get_display_attributes;
    pdev->getActiveConfig = hwc_get_active_config;
    pdev->setActiveConfig = hwc_set_active_config;
    pdev->setCursorPositionAsync = hwc_set_cursor_position_async;

    pdev->vsync_period_ns = 1000*1000*1000/60; // vsync is 60 hz

    pdev->multi_windows = property_get_bool("persist.waydroid.multi_windows", false);
    pdev->should_compose = property_get_bool("persist.waydroid.use_subsurface", false) || pdev->multi_windows;
    pdev->timeline_fd = sw_sync_timeline_create();
    pdev->next_sync_point = 1;

    pdev->blacklisted_apps["com.android.launcher3"] = {};
    pdev->blacklisted_apps["com.android.settings"] = {"com.android.settings.FallbackHome"};
    if (property_get("waydroid.blacklist_apps", property, nullptr) > 0) {
        std::string blacklist_apps = std::string(property);
        std::istringstream iss(blacklist_apps);
        std::string app;
        while (std::getline(iss, app, ':')) {
            pdev->blacklisted_apps[app] = {};
        }
    }

    if (property_get("waydroid.xdg_runtime_dir", property, "/run/user/1000") > 0) {
        setenv("XDG_RUNTIME_DIR", property, 1);
    }
    if (property_get("waydroid.wayland_display", property, "wayland-0") > 0) {
        setenv("WAYLAND_DISPLAY", property, 1);
    }
    if (property_get("ro.hardware.gralloc", property, "default") > 0) {
        pdev->display = create_display(property);
    }
    pdev->gralloc_handler = gralloc_handler(pdev->display);
    if (!pdev->display) {
        ALOGE("failed to open wayland connection");
        return -ENODEV;
    }
    ALOGE("wayland display %p", pdev->display);

    pdev->vsync_callback_enabled = true;

    auto first_window = window::create(pdev->display, pdev->should_compose, "Waydroid", "0", {0, 0, 0, 255});
    if (!property_get_bool("waydroid.background_start", true)) {
        pdev->display->windows["Waydroid"] = std::move(first_window);
        property_set("waydroid.active_apps", "Waydroid");
        property_set("waydroid.open_windows", "1");
    } else {
        first_window.reset();
    }

    if (pdev->display->refresh > 1000 && pdev->display->refresh < 1000000)
        pdev->vsync_period_ns = 1000 * 1000 * 1000 / (pdev->display->refresh / 1000);

    if (!property_get_bool("persist.waydroid.cursor_on_subsurface", false)) {
        pdev->display->cursor_handler.reset(new wl_cursor_cursor_handler(pdev));
    } else {
        pdev->display->cursor_handler.reset(new subsurface_cursor_handler());
    }


    struct timespec rt;
    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
       ALOGE("%s:%d error in vsync thread clock_gettime: %s",
            __FILE__, __LINE__, strerror(errno));
    }

    pdev->last_vsync_ns = int64_t(rt.tv_sec) * 1e9 + rt.tv_nsec;

    if (!pdev->vsync_thread) {
        ret = pthread_create (&pdev->vsync_thread, NULL, hwc_vsync_thread, pdev);
        if (ret) {
            ALOGE("waydroid_hw_composer could not start vsync_thread\n");
        }
    }

    ret = pthread_create (&pdev->wayland_thread, NULL, hwc_wayland_thread, pdev);
    if (ret) {
        ALOGE("waydroid_hw_composer could not start wayland_thread\n");
    }

    ret = pthread_create (&pdev->binder_thread, NULL, hwc_binder_thread, pdev);
    if (ret) {
        ALOGE("waydroid_hw_composer could not start binder thread");
    }

    ret = pthread_create(&pdev->egl_worker_thread, NULL, egl_loop, pdev->display);
    if (ret) {
        ALOGE("waydroid_hw_composer could not start egl_worker_thread");
    }

    *device = &pdev->common;

    return ret;
}

int subsurface_cursor_handler::apply_cursor(waydroid_hwc_composer_device_1* pdev, hwc_layer_1* hwc_layer, size_t hwc_layer_index) {
    if (!pdev->display->pointer_surface) {
        if (hwc_layer->acquireFenceFd != -1) {
            close(hwc_layer->acquireFenceFd);
        }
        return 0;
    }

    auto window_it = std::find_if(pdev->display->windows.begin(), pdev->display->windows.end(), [&](const auto &it){
        auto &window = it.second;
        return window->surface == pdev->display->pointer_surface
               || std::any_of(window->layers.begin(), window->layers.end(), [&](const auto &layer) {
                      return layer.surface == pdev->display->pointer_surface;
                  });
    });
    if (window_it == pdev->display->windows.end()) {
        if (hwc_layer->acquireFenceFd != -1) {
            close(hwc_layer->acquireFenceFd);
        }
        return 0;
    }

    return apply_hwc_layer_to_window(pdev, hwc_layer, hwc_layer_index, window_it->second.get());
}

wl_cursor_cursor_handler::wl_cursor_cursor_handler(waydroid_hwc_composer_device_1* pdev) {
    cursor_surface_context.surface = wl_compositor_create_surface(pdev->display->compositor);
    if (pdev->display->viewporter && pdev->display->supports_cursor_viewport) {
        cursor_surface_context.viewport =
                wp_viewporter_get_viewport(pdev->display->viewporter, cursor_surface_context.surface);
    }
}

void wl_cursor_cursor_handler::set_cursor(display* display) const {
    assert(display->pointer);
    wl_pointer_set_cursor (display->pointer, display->pointer_enter_serial,
                          cursor_surface_context.surface,
                          round(display->cursor_hotspot.x / display->scale),
                          round(display->cursor_hotspot.y / display->scale));
}

int wl_cursor_cursor_handler::apply_cursor(waydroid_hwc_composer_device_1* pdev, hwc_layer_1* hwc_layer, size_t hwc_layer_index) {
    if (pdev->display->pointer) {
        if (apply_hwc_layer_to_surface_context(pdev, hwc_layer, hwc_layer_index, cursor_surface_context) != 0) {
            ALOGE("Failed to prepare cursur surface");
            return -1;
        }
        set_cursor(pdev->display);
    }
    return 0;
}

int wl_cursor_cursor_handler::reset_cursor(waydroid_hwc_composer_device_1* pdev) {
    if (pdev->display->pointer) {
        wl_pointer_set_cursor(pdev->display->pointer, pdev->display->pointer_enter_serial,
                              nullptr,
                              0,
                              0);
    }
    return 0;
}

int wl_cursor_cursor_handler::cursor_enter(display* display) {
    set_cursor(display);
    return 0;
}


static struct hw_module_methods_t hwc_module_methods = {
    .open = hwc_open,
};

hwc_module_t HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = HWC_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = HWC_HARDWARE_MODULE_ID,
        .name = "Waydroid hwcomposer module",
        .author = "The Android Open Source Project",
        .methods = &hwc_module_methods,
    }
};

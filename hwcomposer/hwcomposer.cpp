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
        if (!metadata.format) {
            // hwc_set can run before the setLayerHandleInfo HIDL call arrives.
            ALOGW("get_wl_buffer: skipping pos=%zu, metadata not ready (map size=%zu)",
                  pos, pdev->display->layer_handles_ext.size());
            return nullptr;
        }
        buffer *buf = find_cached_buffer(pdev, metadata, layer->handle);

        if (!buf) {
            std::unique_ptr<buffer> result;
            if (layer->flags & HWC_IS_CURSOR_LAYER)
                result = pdev->display->cursor_handler->create_buffer(pdev, metadata, layer);
            else
                result = gralloc_handler.create_buffer(pdev->display, metadata, layer->handle);
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

    std::unique_ptr<waydroid_mode> select_mode(waydroid_hwc_composer_device_1 *pdev, hwc_display_contents_1_t *contents) {
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
}

static int hwc_prepare(hwc_composer_device_1_t* dev,
                       size_t numDisplays, hwc_display_contents_1_t** displays) {
    if (HWC_DISPLAY_PRIMARY >= numDisplays || !displays)
        return 0;

    auto *pdev = static_cast<waydroid_hwc_composer_device_1 *>(dev);

    hwc_display_contents_1_t *contents = displays[HWC_DISPLAY_PRIMARY];
    assert(contents);

    pdev->selected_mode = select_mode(pdev, contents);
    if (pdev->selected_mode->setup_prepare(pdev, contents) != 0) {
        return -1;
    }

    for (size_t i = 0; i < contents->numHwLayers; i++) {
        if (contents->hwLayers[i].flags & HWC_IS_CURSOR_LAYER) {
            contents->hwLayers[i].compositionType = HWC_OVERLAY;
            continue;
        }
        if (contents->hwLayers[i].compositionType == HWC_FRAMEBUFFER_TARGET)
            continue;
        if (contents->hwLayers[i].flags & HWC_SKIP_LAYER)
            continue;

        if (pdev->selected_mode->prepare(&contents->hwLayers[i], i) != 0) {
            return -1;
        }
    }

    return 0;
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

static void apply_surface_damage(hwc_layer_1 *hwc_layer, surface_context &surface_context) {
    auto &surface_damage = hwc_layer->surfaceDamage;

    if (surface_damage.numRects == 0
        || wl_surface_get_version(surface_context.surface) < WL_SURFACE_DAMAGE_BUFFER_SINCE_VERSION) {
        surface_context.damage_surface(0, 0, INT32_MAX, INT32_MAX);
    }

    std::for_each(surface_damage.rects, surface_damage.rects + surface_damage.numRects, [&](const auto &rect){
        surface_context.damage_surface(
            rect.left,
            rect.top,
            rect.right - rect.left,
            rect.bottom - rect.top
        );
    });
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

    surface_context.attach_buffer(*buf);
    apply_surface_damage(hwc_layer, surface_context);
    surface_context.set_buffer_transform(hwc_transform_to_buffer_transform(hwc_layer->transform));
    // Scaling can only be supported correctly with wp_viewport
    if (surface_context.viewport) {
        surface_context.set_crop(rect_apply_transform(hwc_layer->sourceCropf, hwc_layer->transform));
        surface_context.set_display_frame(hwc_layer->displayFrame, pdev->display->scale);
    } else {
        surface_context.set_buffer_scale(pdev->display->scale);
    }

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
    buffer *buf = get_wl_buffer(pdev, hwc_layer, hwc_layer_index);
    if (!buf) {
        ALOGE("Failed to get wayland buffer");
        if (hwc_layer->acquireFenceFd != -1) {
            close(hwc_layer->acquireFenceFd);
        }
        return -1;
    }

    auto &window_layer = window->get_next_layer();

    if (apply_hwc_layer_to_surface_context(pdev, hwc_layer, hwc_layer_index, window_layer, buf) != 0) {
        return -1;
    }

    window_layer.set_position(
        floor(hwc_layer->displayFrame.left / pdev->display->scale),
        floor(hwc_layer->displayFrame.top / pdev->display->scale)
    );

    if (window->input_region) {
        wl_region_add(window->input_region,
                      -WINDOW_DECORATION_OUTSET + floor(hwc_layer->displayFrame.left / pdev->display->scale),
                      -WINDOW_DECORATION_OUTSET + floor(hwc_layer->displayFrame.top / pdev->display->scale),
                      2*WINDOW_DECORATION_OUTSET + ceil((hwc_layer->displayFrame.right - hwc_layer->displayFrame.left) / pdev->display->scale),
                      2*WINDOW_DECORATION_OUTSET + ceil((hwc_layer->displayFrame.bottom - hwc_layer->displayFrame.top) / pdev->display->scale));
    }

    pdev->display->layers[window_layer.surface] = {
            .x = hwc_layer->displayFrame.left,
            .y = hwc_layer->displayFrame.top };

    if (window->display->presentation) {
        auto feedback = wp_presentation_feedback(window->display->presentation, window_layer.surface);
        wp_presentation_feedback_add_listener(feedback,&feedback_listener, pdev);
    }

    window->last_layer_buffer = buf;

    // Snapshot buffer should be detached by now, clean up
    window->snapshot_buffer = nullptr;

    return 0;
}

static void reset_per_commit_state_window(waydroid_hwc_composer_device_1 *pdev) {
    for (auto& [id, window] : pdev->display->windows) {
        window->reset_per_set_state();
    }
}

static int hwc_set(struct hwc_composer_device_1* dev,size_t numDisplays,
                   hwc_display_contents_1_t** displays) {
    if (HWC_DISPLAY_PRIMARY >= numDisplays || !displays)
        return 0;

    auto *pdev = static_cast<waydroid_hwc_composer_device_1 *>(dev);

    hwc_display_contents_1_t* contents = displays[HWC_DISPLAY_PRIMARY];
    assert(contents);

    if (pdev->should_compose && contents->flags & HWC_GEOMETRY_CHANGED) {
        pdev->display->buffer_map.clear();
    }

    auto& mode = pdev->selected_mode;
    mode->setup_set(pdev, contents);

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

    if (pdev->display->needHotplug && pdev->procs && pdev->procs->hotplug) {
        pdev->procs->hotplug(pdev->procs, 0, 1);
        pdev->display->buffer_map.clear();
        pdev->display->needHotplug = false;
    }
    return 0;
}

static int hwc_event_control(struct hwc_composer_device_1* dev, int disp,
                             int event, int enabled) {
    auto *pdev = static_cast<waydroid_hwc_composer_device_1 *>(dev);

    // enabled can only be 0 or 1
    if (enabled & ~1) {
        return -EINVAL;
    }

    if (disp != HWC_DISPLAY_PRIMARY) {
        return 0;
    }

    switch (event) {
        case HWC_EVENT_VSYNC:
            pdev->vsync_callback_enabled = enabled;
            return 0;
        default:
            // unsupported event
            ALOGE("%s badness unsupported event event=%d", __FUNCTION__, event);
            return -EINVAL;
    }
}

static int hwc_set_power_move(struct hwc_composer_device_1 *dev __unused, int disp __unused, int mode __unused) {
    return 0;
}

static int hwc_query(struct hwc_composer_device_1 *, int what, int *value) {
    switch (what) {
        case HWC_BACKGROUND_LAYER_SUPPORTED:
            // TODO: Support background layer
            *value = 0;
            break;
        case HWC_DISPLAY_TYPES_SUPPORTED:
            *value = HWC_DISPLAY_PRIMARY;
            break;
        default:
            // unsupported query
            ALOGE("%s badness unsupported query what=%d", __FUNCTION__, what);
            return -EINVAL;
    }
    return 0;
}

static void hwc_register_procs(struct hwc_composer_device_1* dev,
                               hwc_procs_t const* procs) {
    auto *pdev = static_cast<waydroid_hwc_composer_device_1 *>(dev);
    pdev->procs = procs;

    pdev->display->procs = procs;
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

static int hwc_get_display_attributes(struct hwc_composer_device_1* dev,
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

static int hwc_close(hw_device_t* dev) {
    auto *pdev = reinterpret_cast<waydroid_hwc_composer_device_1 *>(dev);

    pdev->display->buffer_map.clear();

    destroy_display(pdev->display);

    delete pdev;
    return 0;
}

static void* hwc_binder_thread(void* data) {
    auto *pdev = static_cast<waydroid_hwc_composer_device_1 *>(data);
    status_t status;

    sp<IWaydroidDisplay> waydroidDisplay;
    sp<IWaydroidWindow> waydroidWindow;
    sp<IWaydroidClipboard> waydroidClipboard;

    setpriority(PRIO_PROCESS, 0, HAL_PRIORITY_URGENT_DISPLAY);
    // Don't configure the threadpool here: composer@2.1-service main() already
    // set it to 4 threads, and shrinking it to 1 aborts the process.

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
    pdev->common.version = HWC_DEVICE_API_VERSION_1_5;
    pdev->common.module = const_cast<hw_module_t *>(module);
    pdev->common.close = hwc_close;

    pdev->prepare = hwc_prepare;
    pdev->set = hwc_set;
    pdev->eventControl = hwc_event_control;
    pdev->setPowerMode = hwc_set_power_move;
    pdev->query = hwc_query;
    pdev->registerProcs = hwc_register_procs;
    pdev->dump = nullptr;
    pdev->getDisplayConfigs = hwc_get_display_configs;
    pdev->getDisplayAttributes = hwc_get_display_attributes;
    pdev->getActiveConfig = hwc_get_active_config;
    pdev->setActiveConfig = hwc_set_active_config;
    pdev->setCursorPositionAsync = hwc_set_cursor_position_async;

    pdev->vsync_period_ns = 1000*1000*1000/60; // vsync is 60 hz

    pdev->timeline_fd = sw_sync_timeline_create();
    pdev->next_sync_point = 1;

    pdev->blacklisted_apps["com.android.launcher3"] = {};
    pdev->blacklisted_apps["com.android.settings"] = {"com.android.settings.FallbackHome"};
    pdev->blacklisted_apps["com.android.tv.settings"] = {"com.android.tv.settings.system.FallbackHome"};
    pdev->blacklisted_apps["com.google.android.tvlauncher"] = {};
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
    if (!pdev->display) {
        ALOGE("failed to open wayland connection");
        return -ENODEV;
    }
    ALOGE("wayland display %p", pdev->display);

    pdev->gralloc_handler = gralloc_handler(pdev->display);
    pdev->multi_windows = property_get_bool("persist.waydroid.multi_windows", false);
    if (pdev->multi_windows && !pdev->display->subcompositor) {
        ALOGW("multi window mode requested but wl_subcompositor is not supported. Disabling it.");
        pdev->multi_windows = false;
    }
    pdev->should_compose = property_get_bool("persist.waydroid.use_subsurface", false) || pdev->multi_windows;
    if (pdev->should_compose && !pdev->display->subcompositor) {
        ALOGW("usage of subsurfaces requested but wl_subcompositor is not supported. Disabling it.");
        pdev->should_compose = false;
    }

    if (!property_get_bool("persist.waydroid.cursor_on_subsurface", false)) {
        pdev->display->cursor_handler.reset(new wl_cursor_cursor_handler(pdev));
    } else {
        pdev->display->cursor_handler.reset(new subsurface_cursor_handler());
    }

    auto first_window = window::create(pdev->display, pdev->should_compose, "Waydroid", "0", {0, 0, 0, 255});
    if (!property_get_bool("waydroid.background_start", true)) {
        pdev->display->windows.add("Waydroid", std::move(first_window));
        property_set("waydroid.active_apps", "Waydroid");
    } else {
        first_window.reset();
    }

    pdev->vsync_callback_enabled = true;
    if (pdev->display->refresh > 1000 && pdev->display->refresh < 1000000)
        pdev->vsync_period_ns = 1000 * 1000 * 1000 / (pdev->display->refresh / 1000);

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

std::unique_ptr<buffer> cursor_handler::create_buffer(waydroid_hwc_composer_device_1 *pdev, const buffer_metadata& metadata, hwc_layer_1 *hwc_layer) {
    return pdev->gralloc_handler.create_buffer(pdev->display, metadata, hwc_layer->handle);
};

void subsurface_cursor_handler::clear_previous_subsurface_if_needed(waydroid_hwc_composer_device_1 *pdev) {
    /* If should_compose is set, remaining subsurface are cleared in post_processing.
     * In this case we can skip it here */
    if (!pdev->should_compose && !window_key.empty()) {
        auto window_it = pdev->display->windows.find(window_key);
        if (window_it == pdev->display->windows.end()) {
            // Window was closed this hwc_set
            return;
        }

        assert(window_it->second->layers.size() == 2);
        auto &last_layer = window_it->second->layers[window_it->second->layers.size() - 1];
        wl_surface_attach(last_layer.surface, nullptr, 0, 0);
        wl_surface_commit(last_layer.surface);
    }
    window_key = {};
}

int subsurface_cursor_handler::apply_cursor(waydroid_hwc_composer_device_1* pdev, hwc_layer_1* hwc_layer, size_t hwc_layer_index) {
    if (!pdev->display->pointer_surface) {
        if (hwc_layer->acquireFenceFd != -1) {
            close(hwc_layer->acquireFenceFd);
        }
        clear_previous_subsurface_if_needed(pdev);
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
        clear_previous_subsurface_if_needed(pdev);
        return 0;
    }


    int res = apply_hwc_layer_to_window(pdev, hwc_layer, hwc_layer_index, window_it->second.get());
    if (res == 0) {
        window_key = window_it->first;
        return 0;
    } else {
        window_key = {};
        return res;
    }
}

int subsurface_cursor_handler::reset_cursor(waydroid_hwc_composer_device_1* pdev) {
    clear_previous_subsurface_if_needed(pdev);
    return 0;
}

int subsurface_cursor_handler::on_cursor_enter(display* display) {
    if (display->pointer) {
        wl_pointer_set_cursor(display->pointer, display->pointer_enter_serial,
                              nullptr,
                              0,
                              0);
    }
    return 0;
}

wl_cursor_cursor_handler::wl_cursor_cursor_handler(waydroid_hwc_composer_device_1* pdev) {
    cursor_surface_context.surface = wl_compositor_create_surface(pdev->display->compositor);
    if (pdev->display->viewporter && pdev->display->supports_cursor_viewport) {
        cursor_surface_context.viewport =
                wp_viewporter_get_viewport(pdev->display->viewporter, cursor_surface_context.surface);
    }
}

std::unique_ptr<buffer> wl_cursor_cursor_handler::create_buffer(waydroid_hwc_composer_device_1* pdev, const buffer_metadata& metadata, hwc_layer_1 *hwc_layer) {
    if (pdev->display->supports_cursor_hw_buffer)
        return cursor_handler::create_buffer(pdev, metadata, hwc_layer);
    else
        return create_shm_wl_buffer (pdev->display, metadata, hwc_layer->handle);
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

int wl_cursor_cursor_handler::on_cursor_enter(display* display) {
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

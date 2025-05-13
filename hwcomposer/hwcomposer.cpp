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

#include "extension.h"
#include "WaydroidClipboard.h"
#include "WaydroidWindow.h"
#include "egl-tools.h"
#include "gralloc_handler.h"

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

struct waydroid_hwc_composer_device_1 : hwc_composer_device_1_t {
    const hwc_procs_t *procs;        // constant after init
    pthread_t wayland_thread;        // constant after init
    pthread_t vsync_thread;          // constant after init
    pthread_t binder_thread;         // constant after init
    pthread_t egl_worker_thread;     // constant after init
    int32_t vsync_period_ns;         // constant after init
    struct display *display;         // constant after init
    gralloc_handler gralloc_handler; // constant after init
    std::map<std::string, struct window *> windows;
    std::map<std::string, std::vector<std::string>> blacklisted_apps;

    pthread_mutex_t vsync_lock;
    bool vsync_callback_enabled; // protected by this->vsync_lock
    uint64_t last_vsync_ns;

    int timeline_fd;
    int next_sync_point;
    bool should_compose;
    bool multi_windows;
};

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

static struct wl_surface *get_surface(struct waydroid_hwc_composer_device_1 *pdev, hwc_layer_1_t *layer, struct window *window)
{
    if (window->lastLayer >= window->layers.size()) {
        assert(window->lastLayer == window->layers.size());
        window->create_new_layer();
    }
    window::layer &requested_layer = window->layers[window->lastLayer];

    if (requested_layer.viewport) {
        setup_viewport_source(requested_layer.viewport, layer->sourceCropf, layer->transform);
        setup_viewport_destination(requested_layer.viewport, layer->displayFrame, pdev->display);
    }

    if (requested_layer.subsurface) {
        wl_subsurface_set_position(requested_layer.subsurface,
                                   floor(layer->displayFrame.left / pdev->display->scale),
                                   floor(layer->displayFrame.top / pdev->display->scale));
    }

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
    return requested_layer.surface;
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

    pthread_mutex_lock(&pdev->vsync_lock);
    wait_time.tv_nsec = time_to_sleep_to_next_vsync(&rt, pdev->last_vsync_ns, pdev->vsync_period_ns);
    pthread_mutex_unlock(&pdev->vsync_lock);

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

        pthread_mutex_lock(&pdev->vsync_lock);
        vsync_enabled = pdev->vsync_callback_enabled;
        pthread_mutex_unlock(&pdev->vsync_lock);

        if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
            ALOGE("%s:%d error in vsync thread clock_gettime: %s",
                  __FILE__, __LINE__, strerror(errno));
        }

        pthread_mutex_lock(&pdev->vsync_lock);
        wait_time.tv_nsec = time_to_sleep_to_next_vsync(&rt, pdev->last_vsync_ns, pdev->vsync_period_ns);
        pthread_mutex_unlock(&pdev->vsync_lock);

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

    pthread_mutex_lock(&pdev->vsync_lock);
    pdev->last_vsync_ns = (((uint64_t)tv_sec_hi << 32) + tv_sec_lo) * 1e9 + tv_nsec;
    pthread_mutex_unlock(&pdev->vsync_lock);
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

static bool is_blacklisted(struct waydroid_hwc_composer_device_1* pdev, std::string &app_id, std::string &component) {
    auto match = pdev->blacklisted_apps.find(app_id);
    if (match == pdev->blacklisted_apps.end())
        return false;
    auto &components = match->second;
    return components.empty() || std::find(components.begin(), components.end(), component) != components.end();
}

static int hwc_set(struct hwc_composer_device_1* dev,size_t numDisplays,
                   hwc_display_contents_1_t** displays) {
    char property[PROPERTY_VALUE_MAX];

    if (HWC_DISPLAY_PRIMARY >= numDisplays || !displays)
        return 0;

    auto *pdev = static_cast<waydroid_hwc_composer_device_1 *>(dev);

    hwc_display_contents_1_t* contents = displays[HWC_DISPLAY_PRIMARY];
    assert(contents);

    size_t fb_target = -1;
    int err = 0;
    bool found_cursor = false;

    std::pair<int, int> skipped(-1, -1);
    if (pdev->should_compose && !pdev->multi_windows) {
        skipped = search_first_and_last_skipped_layer(contents);
    }

    /*
     * In prop "persist.waydroid.multi_windows" we detect HWC let SF rander layers 
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
    property_get("waydroid.active_apps", property, "none");
    std::string active_apps = std::string(property);
    std::string single_layer_tid;
    std::string single_layer_aid;

    if (active_apps != "Waydroid" && !property_get_bool("waydroid.background_start", true)) {
        for (size_t l = 0; l < contents->numHwLayers; l++) {
            std::string layer_name = pdev->display->layer_names[l];
            if (layer_name.rfind("BootAnimation#", 0) == 0) {
                // force single window mode during boot animation
                active_apps = "Waydroid";
                break;
            }
        }
    }

    std::scoped_lock lock(pdev->display->windowsMutex);
    if (active_apps == "none") {
        // Clear all open windows
        for (auto it = pdev->windows.begin(); it != pdev->windows.end(); it++) {
            if (it->second)
                destroy_window(it->second);
        }
        pdev->windows.clear();
        for (size_t layer = 0; layer < contents->numHwLayers; layer++) {
            hwc_layer_1_t* fb_layer = &contents->hwLayers[layer];
            if (fb_layer->acquireFenceFd != -1)
                close(fb_layer->acquireFenceFd);
        }

        property_set("waydroid.open_windows", "0");
        goto sync;
    } else if (active_apps == "Waydroid") {
        // Clear all open windows if there's any and just keep "Waydroid"
        if (pdev->windows.find(active_apps) == pdev->windows.end() || !pdev->windows[active_apps]->isActive) {
            for (auto it = pdev->windows.begin(); it != pdev->windows.end(); it++) {
                if (it->second) {
                    destroy_window(it->second);
                }
            }
            pdev->windows.clear();
        } else {
            pdev->windows[active_apps]->lastLayer = 0;
            pdev->windows[active_apps]->last_layer_buffer = nullptr;
        }
    } else if (!pdev->multi_windows) {
        // Single window mode, detecting if any unblacklisted app is on screen
        ShowWindowState showWindow = ShowWindowState::NONE;
        for (size_t l = 0; l < contents->numHwLayers; l++) {
            std::string layer_name = pdev->display->layer_names[l];
            if (layer_name.substr(0, 4) == "TID:") {
                std::string layer_tid = layer_name.substr(4, layer_name.find('#') - 4);
                std::string layer_aid = layer_name.substr(layer_name.find('#') + 1, layer_name.find('/') - layer_name.find('#') - 1);
                size_t c = layer_name.find('/');
                std::string component = layer_name.substr(c + 1, layer_name.find('#', c) - c - 1);

                if (is_blacklisted(pdev, layer_aid, component)) {
                    if (showWindow == ShowWindowState::NONE)
                        showWindow = ShowWindowState::BLACKLISTED;
                } else {
                    showWindow = ShowWindowState::YES;
                    if (!single_layer_tid.length()) {
                        single_layer_tid = layer_tid;
                        single_layer_aid = layer_aid;
                    }
                    if (pdev->windows.find(single_layer_tid) != pdev->windows.end()) {
                        pdev->windows[single_layer_tid]->lastLayer = 0;
                        pdev->windows[single_layer_tid]->last_layer_buffer = nullptr;
                    }
                }
            }
        }
        // Nothing to show on screen, so clear all open windows
        if (showWindow == ShowWindowState::BLACKLISTED) {
            for (auto it = pdev->windows.begin(); it != pdev->windows.end(); it++) {
                if (it->second)
                    destroy_window(it->second);
            }
            pdev->windows.clear();
            for (size_t layer = 0; layer < contents->numHwLayers; layer++) {
                hwc_layer_1_t* fb_layer = &contents->hwLayers[layer];
                if (fb_layer->acquireFenceFd != -1)
                    close(fb_layer->acquireFenceFd);
            }

            property_set("waydroid.open_windows", "0");
            goto sync;
        }
        bool shouldCloseLeftover = true;
        for (auto it = pdev->windows.cbegin(); it != pdev->windows.cend();) {
            if (it->second) {
                // This window is closed, but android is still showing leftover layers, we detect it here
                if (!it->second->isActive || it->first == "Waydroid") {
                    for (size_t l = 0; l < contents->numHwLayers; l++) {
                        std::string layer_name = pdev->display->layer_names[l];
                        if (layer_name.substr(0, 4) == "TID:") {
                            std::string layer_tid = layer_name.substr(4, layer_name.find('#') - 4);
                            if (layer_tid == it->first) {
                                shouldCloseLeftover = false;
                                break;
                            }
                        }
                    }
                    if (shouldCloseLeftover) {
                        destroy_window(it->second);
                        pdev->windows.erase(it++);
                        shouldCloseLeftover = true;
                        std::string windows_size_str = std::to_string(pdev->windows.size());
                        property_set("waydroid.open_windows", windows_size_str.c_str());
                    } else
                        ++it;
                } else
                    ++it;
            } else
                ++it;
        }
    } else {
        // Multi window mode
        // Checking current open windows to detect and kill obsolete ones
        for (auto it = pdev->windows.cbegin(); it != pdev->windows.cend();) {
            bool foundApp = false;
            for (size_t l = 0; l < contents->numHwLayers; l++) {
                if (contents->hwLayers[l].compositionType != HWC_OVERLAY)
                    continue;

                std::string layer_name = pdev->display->layer_names[l];
                if (layer_name.substr(0, 4) == "TID:") {
                    std::string layer_tid = layer_name.substr(4, layer_name.find('#') - 4);
                    if (layer_tid == it->first) {
                        it->second->lastLayer = 0;
                        it->second->last_layer_buffer = nullptr;
                        foundApp = true;
                        break;
                    }
                } else {
                    std::string LayerRawName;
                    std::istringstream issLayer(layer_name);
                    std::getline(issLayer, LayerRawName, '#');
                    if (LayerRawName == it->first) {
                        it->second->lastLayer = 0;
                        it->second->last_layer_buffer = nullptr;
                        foundApp = true;
                        break;
                    }
                }
            }
            // This window ID doesn't match with any selected app IDs from prop, so kill it
            if (!foundApp || (it->second && !it->second->isActive)) {
                if (it->second)
                    destroy_window(it->second);
                pdev->windows.erase(it++);
                std::string windows_size_str = std::to_string(pdev->windows.size());
                property_set("waydroid.open_windows", windows_size_str.c_str());
            } else {
                ++it;
            }
        }
    }

    // Set Wayland cursor
    if (pdev->display->pointer) {
        for (size_t l = 0; l < contents->numHwLayers; l++) {
            hwc_layer_1_t* fb_layer = &contents->hwLayers[l];
            if ((fb_layer->flags & HWC_IS_CURSOR_LAYER) && pdev->display->cursor_surface) {
                struct buffer *buf = get_wl_buffer(pdev, fb_layer, l);
                if (!buf) {
                    ALOGE("Failed to get wayland buffer");
                    if (fb_layer->acquireFenceFd != -1) {
                        close(fb_layer->acquireFenceFd);
                    }
                    break;
                }

                wl_surface_attach(pdev->display->cursor_surface, buf->wl_buffer, 0, 0);
                if (wl_surface_get_version(pdev->display->cursor_surface) >= WL_SURFACE_DAMAGE_BUFFER_SINCE_VERSION)
                    wl_surface_damage_buffer(pdev->display->cursor_surface, 0, 0, buf->metadata.width, buf->metadata.height);
                else
                    wl_surface_damage(pdev->display->cursor_surface, 0, 0, buf->metadata.width, buf->metadata.height);
                if (!pdev->display->viewporter && pdev->display->scale > 1) {
                    // With no viewporter the scale is guaranteed to be integer
                    wl_surface_set_buffer_scale(pdev->display->cursor_surface, (int)pdev->display->scale);
                }
                if (pdev->display->cursor_viewport) {
                    setup_viewport_source(pdev->display->cursor_viewport, fb_layer->sourceCropf, fb_layer->transform);
                    setup_viewport_destination(pdev->display->cursor_viewport, fb_layer->displayFrame, pdev->display);
                } else {
                    wl_surface_set_buffer_scale(pdev->display->cursor_surface, (int)ceil(pdev->display->scale));
                }

                wl_pointer_set_cursor (pdev->display->pointer, pdev->display->pointer_enter_serial,
                                       pdev->display->cursor_surface,
                                       roundf(pdev->display->cursor_hotspot.x / pdev->display->scale),
                                       roundf(pdev->display->cursor_hotspot.y / pdev->display->scale));

                const int kAcquireWarningMS = 100;
                err = sync_wait(fb_layer->acquireFenceFd, kAcquireWarningMS);
                if (err < 0 && errno == ETIME) {
                    ALOGE("hwcomposer waited on fence %d for %d ms",
                        fb_layer->acquireFenceFd, kAcquireWarningMS);
                }
                close(fb_layer->acquireFenceFd);

                wl_surface_commit(pdev->display->cursor_surface);
                found_cursor = true;
                break;
            }
        }
        if (!found_cursor) {
            wl_pointer_set_cursor (pdev->display->pointer, pdev->display->pointer_enter_serial, NULL, 0, 0);
        }
    }

    for (size_t l = 0; l < contents->numHwLayers; l++) {
        hwc_layer_1_t* fb_layer = &contents->hwLayers[l];
        if (fb_layer->compositionType == HWC_FRAMEBUFFER_TARGET) {
            fb_target = l;
            break;
        }
    }

    for (auto it = pdev->windows.begin(); it != pdev->windows.end(); it++)
        if (it->second->input_region)
            wl_region_subtract(it->second->input_region, 0, 0, INT_MAX, INT_MAX);

    for (size_t l = 0; l < contents->numHwLayers; l++) {
        size_t layer = l;
        if (l == skipped.first && fb_target >= 0) {
            // draw framebuffer target instead of skipped layers
            if (contents->hwLayers[layer].acquireFenceFd != -1) {
                close(contents->hwLayers[layer].acquireFenceFd);
            }
            layer = fb_target;
        }
        if (skipped.first >= 0 && l == fb_target) {
            // don't handle fb_target twice
            continue;
        }

        hwc_layer_1_t* fb_layer = &contents->hwLayers[layer];

        if (fb_layer->flags & HWC_SKIP_LAYER) {
            if (fb_layer->acquireFenceFd != -1) {
                close(fb_layer->acquireFenceFd);
            }
            continue;
        }

        if ((fb_layer->flags & HWC_IS_CURSOR_LAYER) && pdev->display->cursor_surface) {
            // Cursor was already handled separately
            if (fb_layer->acquireFenceFd != -1) {
                close(fb_layer->acquireFenceFd);
            }
            continue;
        }

        if (fb_layer->compositionType != 
            (pdev->should_compose ? HWC_OVERLAY : HWC_FRAMEBUFFER_TARGET) && layer == l) {
            if (fb_layer->acquireFenceFd != -1) {
                close(fb_layer->acquireFenceFd);
            }
            continue;
        }

        if (!fb_layer->handle) {
            if (fb_layer->acquireFenceFd != -1) {
                close(fb_layer->acquireFenceFd);
            }
            continue;
        }

        struct window *window = NULL;
        std::string layer_name = pdev->display->layer_names[layer];

        if (active_apps == "Waydroid") {
            // Show everything in a single window
            if (pdev->windows.find(active_apps) == pdev->windows.end()) {
                pdev->windows[active_apps] = create_window(pdev->display, pdev->should_compose, active_apps, "0", {0, 0, 0, 255});
                std::string windows_size_str = std::to_string(pdev->windows.size());
                property_set("waydroid.open_windows", windows_size_str.c_str());
            }
            window = pdev->windows[active_apps];
        } else if (!pdev->multi_windows) {
            if (single_layer_tid.length()) {
                if (pdev->windows.find(single_layer_tid) == pdev->windows.end()) {
                    pdev->windows[single_layer_tid] = create_window(pdev->display, pdev->should_compose, single_layer_aid, single_layer_tid, {0, 0, 0, 255});
                    std::string windows_size_str = std::to_string(pdev->windows.size());
                    property_set("waydroid.open_windows", windows_size_str.c_str());
                }
                window = pdev->windows[single_layer_tid];
            }
        } else {
            // Create windows based on Task ID in layer name
            if (layer_name.substr(0, 4) == "TID:") {
                std::string layer_tid = layer_name.substr(4, layer_name.find('#') - 4);
                std::string layer_aid = layer_name.substr(layer_name.find('#') + 1, layer_name.find('/') - layer_name.find('#') - 1);
                size_t c = layer_name.find('/');
                std::string component = layer_name.substr(c + 1, layer_name.find('#', c) - c - 1);

                if (!is_blacklisted(pdev, layer_aid, component)) {
                    if (pdev->windows.find(layer_tid) == pdev->windows.end()) {
                        pdev->windows[layer_tid] = create_window(pdev->display, pdev->should_compose, layer_aid, layer_tid, {0, 0, 0, 0});
                        std::string windows_size_str = std::to_string(pdev->windows.size());
                        property_set("waydroid.open_windows", windows_size_str.c_str());
                    }
                    if (pdev->windows.find(layer_tid) != pdev->windows.end())
                        window = pdev->windows[layer_tid];
                }
            }
        }

        // Detecting special layers (like cursor and IME)
        if (!window) {
            std::string LayerRawName;
            std::istringstream issLayer(layer_name);
            std::getline(issLayer, LayerRawName, '#');
            if ((fb_layer->flags & HWC_IS_CURSOR_LAYER) && !pdev->display->cursor_surface && pdev->display->pointer_surface) {
                // Cursor layer. Without cursor_surface we draw it as a subsurface
                for (auto it = pdev->windows.begin(); it != pdev->windows.end(); it++) {
                    if (it->second) {
                        if (it->second->surface == pdev->display->pointer_surface) {
                            window = it->second;
                            break;
                        }
                        for (auto &window_layer : it->second->layers) {
                            if (window_layer.surface == pdev->display->pointer_surface) {
                                window = it->second;
                                break;
                            }
                        }
                    }
                }
            } else if (LayerRawName == "InputMethod") {
                // IME layer
                if (pdev->windows.find(LayerRawName) == pdev->windows.end()) {
                    pdev->windows[LayerRawName] = create_window(pdev->display, pdev->should_compose, LayerRawName, "none", {0, 0, 0, 0});
                    std::string windows_size_str = std::to_string(pdev->windows.size());
                    property_set("waydroid.open_windows", windows_size_str.c_str());
                }
                if (pdev->windows.find(LayerRawName) != pdev->windows.end())
                    window = pdev->windows[LayerRawName];
            }
        }

        if (!window || !window->isActive) {
            if (fb_layer->acquireFenceFd != -1) {
                close(fb_layer->acquireFenceFd);
            }
            continue;
        }

        struct buffer *buf = get_wl_buffer(pdev, fb_layer, layer);
        if (!buf) {
            ALOGE("Failed to get wayland buffer");
            if (fb_layer->acquireFenceFd != -1) {
               close(fb_layer->acquireFenceFd);
            }
            continue;
        }

        // TODO: Implement per-layer explicit synchronization
        fb_layer->releaseFenceFd = -1;

        struct wl_surface *surface = get_surface(pdev, fb_layer, window);
        if (!surface) {
            ALOGE("Failed to get surface");
            continue;
        }
        window->last_layer_buffer = buf;
        window->lastLayer++;

        wl_surface_attach(surface, buf->wl_buffer, 0, 0);
        if (wl_surface_get_version(surface) >= WL_SURFACE_DAMAGE_BUFFER_SINCE_VERSION)
            wl_surface_damage_buffer(surface, 0, 0, buf->metadata.width, buf->metadata.height);
        else
            wl_surface_damage(surface, 0, 0, buf->metadata.width, buf->metadata.height);
        if (!pdev->display->viewporter && pdev->display->scale > 1) {
            // With no viewporter the scale is guaranteed to be integer
            wl_surface_set_buffer_scale(surface, (int)pdev->display->scale);
        }
        switch (fb_layer->transform) {
            case HWC_TRANSFORM_FLIP_H:
                wl_surface_set_buffer_transform(surface, WL_OUTPUT_TRANSFORM_FLIPPED_180);
                break;
            case HWC_TRANSFORM_FLIP_V:
                wl_surface_set_buffer_transform(surface, WL_OUTPUT_TRANSFORM_FLIPPED);
                break;
            case HWC_TRANSFORM_ROT_90:
                wl_surface_set_buffer_transform(surface, WL_OUTPUT_TRANSFORM_90);
                break;
            case HWC_TRANSFORM_ROT_180:
                wl_surface_set_buffer_transform(surface, WL_OUTPUT_TRANSFORM_180);
                break;
            case HWC_TRANSFORM_ROT_270:
                wl_surface_set_buffer_transform(surface, WL_OUTPUT_TRANSFORM_270);
                break;
            case HWC_TRANSFORM_FLIP_H_ROT_90:
                wl_surface_set_buffer_transform(surface, WL_OUTPUT_TRANSFORM_FLIPPED_270);
                break;
            case HWC_TRANSFORM_FLIP_V_ROT_90:
                wl_surface_set_buffer_transform(surface, WL_OUTPUT_TRANSFORM_FLIPPED_90);
                break;
            default:
                wl_surface_set_buffer_transform(surface, WL_OUTPUT_TRANSFORM_NORMAL);
                break;
        }

        struct wp_presentation *pres = window->display->presentation;
        if (pres) {
            buf->feedback = wp_presentation_feedback(pres, surface);
            wp_presentation_feedback_add_listener(buf->feedback,
                              &feedback_listener, pdev);
        }

        const int kAcquireWarningMS = 100;
        err = sync_wait(fb_layer->acquireFenceFd, kAcquireWarningMS);
        if (err < 0 && errno == ETIME) {
            ALOGE("hwcomposer waited on fence %d for %d ms",
                fb_layer->acquireFenceFd, kAcquireWarningMS);
        }
        close(fb_layer->acquireFenceFd);

        wl_surface_commit(surface);

        // Snapshot buffer should be detached by now, clean up
        window->snapshot_buffer = nullptr;
    }
    // Layers order is changed from SF so we rearrange wayland surfaces
    if (pdev->should_compose && (contents->flags & HWC_GEOMETRY_CHANGED)) {
        for (auto it = pdev->windows.begin(); it != pdev->windows.end(); it++) {
            if (it->second) {
                // This window has no changes in layers, leaving it
                if (!it->second->lastLayer)
                    continue;
                // Neutralize unused surfaces
                for (size_t l = it->second->lastLayer; l < it->second->layers.size(); l++) {
                    wl_surface_attach(it->second->layers[l].surface, NULL, 0, 0);
                    wl_surface_commit(it->second->layers[l].surface);
                }
            }
        }
    }

    for (auto it = pdev->windows.begin(); it != pdev->windows.end(); it++)
        if (it->second->input_region)
            wl_surface_set_input_region(it->second->surface, it->second->input_region);

    if (!pdev->multi_windows && single_layer_tid.length() && active_apps != "Waydroid") {
        for (auto const& [layer_tid, window] : pdev->windows) {
            // Replace inactive app window buffer with snapshot in staged mode
            if (layer_tid != single_layer_tid && !window->snapshot_buffer) {
                pdev->display->egl_work_queue.push_back(std::bind(snapshot_inactive_app_window, pdev->display, window));
            }
        }
        if (!pdev->display->egl_work_queue.empty()) {
            sem_post(&pdev->display->egl_go);
            sem_wait(&pdev->display->egl_done);
        }
    }

    if (pdev->should_compose)
        for (auto it = pdev->windows.begin(); it != pdev->windows.end(); it++)
            if (it->second)
                wl_surface_commit(it->second->surface);
    wl_display_flush(pdev->display->display);

sync:
    sw_sync_timeline_inc(pdev->timeline_fd, 1);
    contents->retireFenceFd = sw_sync_fence_create(pdev->timeline_fd, "hwc_contents_release", ++pdev->next_sync_point);
    return err;
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
            pthread_mutex_lock(&pdev->vsync_lock);
            pdev->vsync_callback_enabled = enabled;
            pthread_mutex_unlock(&pdev->vsync_lock);
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

    pthread_mutex_init(&pdev->vsync_lock, NULL);
    pdev->vsync_callback_enabled = true;

    auto first_window = create_window(pdev->display, pdev->should_compose, "Waydroid", "0", {0, 0, 0, 255});
    if (!property_get_bool("waydroid.background_start", true)) {
        pdev->windows["Waydroid"] = first_window;
        property_set("waydroid.active_apps", "Waydroid");
        property_set("waydroid.open_windows", "1");
    } else {
        destroy_window(first_window);
    }

    if (pdev->display->refresh > 1000 && pdev->display->refresh < 1000000)
        pdev->vsync_period_ns = 1000 * 1000 * 1000 / (pdev->display->refresh / 1000);

    if (!property_get_bool("persist.waydroid.cursor_on_subsurface", false)) {
        pdev->display->cursor_surface =
            wl_compositor_create_surface(pdev->display->compositor);
        if (pdev->display->viewporter && pdev->display->supports_cursor_viewport) {
            pdev->display->cursor_viewport =
                wp_viewporter_get_viewport(pdev->display->viewporter, pdev->display->cursor_surface);
        }
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

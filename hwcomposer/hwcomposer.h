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

#pragma once

#include <hardware/hwcomposer.h>
#include <atomic>
#include <string>
#include <map>

#include "wayland-hwc.h"
#include "gralloc_handler.h"

struct subsurface_cursor_handler : cursor_handler {
    int apply_cursor(waydroid_hwc_composer_device_1 *pdev, hwc_layer_1 *hwc_layer, size_t hwc_layer_index) override;
};

struct wl_cursor_cursor_handler : cursor_handler {
    surface_context cursor_surface_context {};

    wl_cursor_cursor_handler(waydroid_hwc_composer_device_1 *pdev);

    void set_cursor(display *display) const;
    int apply_cursor(waydroid_hwc_composer_device_1 *pdev, hwc_layer_1 *hwc_layer, size_t hwc_layer_index) override;
    int reset_cursor(waydroid_hwc_composer_device_1 *pdev) override;
    int cursor_enter(display *display) override;
};

struct waydroid_hwc_composer_device_1 : hwc_composer_device_1_t {
    const hwc_procs_t *procs;        // constant after init
    pthread_t wayland_thread;        // constant after init
    pthread_t vsync_thread;          // constant after init
    pthread_t binder_thread;         // constant after init
    pthread_t egl_worker_thread;     // constant after init
    int32_t vsync_period_ns;         // constant after init
    struct display *display;         // constant after init
    gralloc_handler gralloc_handler; // constant after init

    std::map<std::string, std::vector<std::string>> blacklisted_apps;

    std::atomic<bool> vsync_callback_enabled;
    std::atomic<uint64_t> last_vsync_ns;

    int timeline_fd;
    int next_sync_point;
    bool should_compose;
    bool multi_windows;
};
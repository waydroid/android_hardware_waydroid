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

#include "single-window.h"

#include <log/log.h>
#include <cutils/properties.h>
#include <sys/system_properties.h>

bool single_window_mode_base::should_show() const {
    return !target_layer_tid.empty();
}

// Longest wait for Android to bring the focused window's task to the front
static constexpr std::chrono::seconds kFocusRequestTimeout{2};

// After focus moves to another window, or a launched app's window is not focused yet, the app in
// front renders at the focused window's size. Keep its window on the last frame at its own size
// meanwhile, but not for longer than the timeout, so a failed setFocusedTask() cannot freeze it.
bool single_window_mode_base::target_has_focus(waydroid_hwc_composer_device_1 *pdev) {
    auto *display = pdev->display;
    const window *focused = display->size_window;
    if (!focused)
        return true;
    auto it = display->windows.find(target_layer_tid);
    if (it == display->windows.end() || it->second.get() == focused)
        return true;

    const auto now = std::chrono::steady_clock::now();
    auto &unfocused = display->unfocused_target;
    if (unfocused.target != it->second.get() || unfocused.focused != focused)
        unfocused = {it->second.get(), focused, now};
    return now - unfocused.since > kFocusRequestTimeout;
}

// Longest time from a Back to Android leaving the app's task
static constexpr std::chrono::seconds kBackTimeout{2};

// Android returned to a task that already has a window soon after a Back, without a launch or a
// focus change from us: the focused app finished, so close its window. Without a Back, the app
// brought another task to the front, as a link to an app with a window does, and a switch to a
// task without a window is the app opening another one; both keep the windows.
void single_window_mode_base::close_left_task_window(waydroid_hwc_composer_device_1 *pdev) {
    auto *display = pdev->display;
    // `waydroid app launch` sets waydroid.active_apps before Android switches tasks. The serial
    // changes on every set, also when the same package is launched again.
    uint32_t active_apps_serial = 0;
    if (const prop_info *active_apps = __system_property_find("waydroid.active_apps")) {
        __system_property_read_callback(active_apps, [](void *cookie, const char *, const char *, uint32_t serial) {
            *static_cast<uint32_t *>(cookie) = serial;
        }, &active_apps_serial);
    }

    // A setFocusedTask() request is fulfilled once its task is in front, so it cannot later excuse
    // a Back that returns to the same task. Several can be pending while focus moves across windows.
    bool focus_requested;
    {
        const auto now = std::chrono::steady_clock::now();
        std::scoped_lock lock(display->requested_tasks_mutex);
        auto &requests = display->requested_tasks;
        for (auto it = requests.begin(); it != requests.end();) {
            if (now - it->second > kFocusRequestTimeout)
                it = requests.erase(it);
            else
                ++it;
        }
        focus_requested = requests.erase(target_layer_tid) > 0;
    }

    if (display->last_target_tid == target_layer_tid)
        return;

    const bool launched = active_apps_serial != display->last_active_apps_serial;
    const std::string left_tid = display->last_target_tid;
    display->last_target_tid = target_layer_tid;
    display->last_active_apps_serial = active_apps_serial;
    const std::chrono::steady_clock::time_point last_back{std::chrono::steady_clock::duration{display->last_back.load()}};
    const bool after_back = std::chrono::steady_clock::now() - last_back < kBackTimeout;

    if (left_tid.empty() || launched || focus_requested || !after_back
            || display->windows.find(target_layer_tid) == display->windows.end())
        return;
    auto it = display->windows.find(left_tid);
    if (it != display->windows.end())
        display->windows.erase(it);
}

window *single_window_mode_base::get_window(waydroid_hwc_composer_device_1 *pdev) const {
    assert(should_show());

    auto &windows = pdev->display->windows;
    auto it = windows.find(target_layer_tid);
    if (it != windows.end()) {
        return it->second.get();
    } else {
        return windows.add(pdev, target_layer_tid, target_layer_aid, target_layer_tid);
    }
}

int single_window_mode_base::setup_set(waydroid_hwc_composer_device_1* pdev, hwc_display_contents_1_t* contents) {
    layer_infos.setup(pdev, contents->hwLayers, contents->numHwLayers);
    return 0;
}

int single_window_mode_base::cleanup_stale_windows(waydroid_hwc_composer_device_1* pdev,
                                                   hwc_display_contents_1_t*) {
    auto first_tid_layer = std::find_if(layer_infos.container().cbegin(), layer_infos.container().cend(), [&](const auto& layer_info){
        return layer_info.type == LayerSplitType::TID;
    });
    if (first_tid_layer != layer_infos.container().cend()) {
        if (is_blacklisted(pdev, first_tid_layer->aid, first_tid_layer->component)) {
            clear_open_windows(pdev);
            return 0;
        } else if (pdev->display->ignored_apps.count(first_tid_layer->tid) == 0) {
            target_layer_tid = first_tid_layer->tid;
            target_layer_aid = first_tid_layer->aid;
            close_left_task_window(pdev);
        }
    }

    // Close leftover window from full-ui mode
    pdev->display->windows.erase("Waydroid");

    // Snapshot non-active windows
    if (should_show()) {
        target_shown = target_has_focus(pdev);
        for (auto const& [layer_tid, window] : pdev->display->windows) {
            // Replace inactive app window buffer with snapshot
            if ((layer_tid != target_layer_tid || !target_shown) && !window->snapshot_buffer) {
                pdev->display->egl_work_queue.push_back(std::bind(snapshot_inactive_app_window, pdev->display, window.get()));
            }
        }
        if (!pdev->display->egl_work_queue.empty()) {
            sem_post(&pdev->display->egl_go);
            sem_wait(&pdev->display->egl_done);
        }
    }

    // Clear ignored apps that are closed now
    auto &ignored_apps = pdev->display->ignored_apps;
    auto it = ignored_apps.begin();
    while (it != ignored_apps.end()) {
        bool still_shown = std::any_of(layer_infos.container().begin(), layer_infos.container().end(), [&it](const auto &layer_info){
            return layer_info.type == LayerSplitType::TID && layer_info.tid == *it;
        });

        if (still_shown) {
            ++it;
        } else {
            it = pdev->display->ignored_apps.erase(it);
        }
    }

    return 0;
}


int non_compositing_single_window_mode::setup_set(waydroid_hwc_composer_device_1* pdev, hwc_display_contents_1_t* contents) {
    int res = Base::setup_set(pdev, contents);
    if (res != 0)
        return res;
    return single_window_mode_base::setup_set(pdev, contents);
}

int non_compositing_single_window_mode::handle_layer(waydroid_hwc_composer_device_1* pdev, hwc_layer_1* hwc_layer, size_t i) {
    if (!should_show() || !target_shown) {
        if (hwc_layer->acquireFenceFd != -1) {
            close(hwc_layer->acquireFenceFd);
        }
        return 0;
    }

    return Base::handle_layer(pdev, hwc_layer, i);
}


int compositing_single_window_mode::setup_set(waydroid_hwc_composer_device_1* pdev, hwc_display_contents_1_t* contents) {
    int res = Base::setup_set(pdev, contents);
    if (res != 0)
        return res;
    return single_window_mode_base::setup_set(pdev, contents);
}

int compositing_single_window_mode::handle_layer(waydroid_hwc_composer_device_1* pdev, hwc_layer_1* hwc_layer, size_t i) {
    if (!should_show() || !target_shown) {
        if (hwc_layer->acquireFenceFd != -1) {
            close(hwc_layer->acquireFenceFd);
        }
        return 0;
    }

    return Base::handle_layer(pdev, hwc_layer, i);
}
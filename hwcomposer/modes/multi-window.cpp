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

#include "multi-window.h"

#include <log/log.h>
#include <cutils/properties.h>

constexpr hwc_color_t color_transparent = {0, 0, 0, 0};

/* The card the keyboard is drawn into: the focused task's, and only while it
 * is drawing this frame. A card that has no layers here is about to be frozen
 * to a snapshot, and the keyboard would land on the frozen surface alone. */
window *multi_window_mode::keyboard_host(waydroid_hwc_composer_device_1 *pdev) {
    for (const auto &[tid, task] : pdev->display->tasks) {
        if (!task.focused || task.closing)
            continue;
        bool drawing = false;
        for (const auto &info : layer_infos.container())
            if (info.type == LayerSplitType::TID && info.tid == tid) {
                drawing = true;
                break;
            }
        if (!drawing)
            break;
        auto card = pdev->display->windows.find(tid);
        return card == pdev->display->windows.end() ? nullptr : card->second.get();
    }
    return nullptr;
}

window *multi_window_mode::get_window(waydroid_hwc_composer_device_1 *pdev, layer_info &layer_info) {
    auto &windows = pdev->display->windows;
    if (layer_info.type == LayerSplitType::TID) {
        // Create windows based on Task ID in layer name
        if (is_blacklisted(pdev, layer_info.aid, layer_info.component))
            return nullptr;

        /* Swipe-closed tasks keep drawing for a few frames while they tear
         * down; recreating their window here would flash the card back. */
        if (pdev->display->task_closing(layer_info.tid))
            return nullptr;

        auto it = windows.find(layer_info.tid);
        if (it != windows.end()) {
            return it->second.get();
        } else {
            pdev->display->note_task_from_layer(layer_info.tid, layer_info.aid);
            return windows.add(pdev, layer_info.tid, layer_info.aid, layer_info.tid, color_transparent);
        }
    } else if (layer_info.type == LayerSplitType::RawName) {
        if (layer_info.aid == "InputMethod") {
            /* The keyboard belongs over the app it is typing into, not beside
             * it: on its own toplevel the host tiles it, moves it and lets it
             * resize the display, and a staged shell shows it as a card of its
             * own. As a layer of the focused card it lands at its display
             * frame, and input already translates surface-local offsets. */
            if (window *card = keyboard_host(pdev)) {
                /* A toplevel from an earlier frame would otherwise stay mapped
                 * showing that frame's keyboard: it is still named in the
                 * frame, so cleanup_stale_windows keeps it, and it gets no
                 * layer, so nothing neutralizes its surfaces either. */
                windows.erase("InputMethod");
                return card;
            }
            /* Nothing to put it on: a toplevel of its own is still better than
             * dropping the keyboard. */
            auto it = windows.find("InputMethod");
            if (it != windows.end()) {
                return it->second.get();
            } else {
                return windows.add(pdev, "InputMethod", "InputMethod", "none", color_transparent);
            }
        }
    }
    return nullptr;
}

bool multi_window_mode::can_handle_layer(const hwc_layer_1& layer) {
    return layer.compositionType == HWC_OVERLAY && !(layer.flags & HWC_SKIP_LAYER);
}

int multi_window_mode::prepare(hwc_layer_1* hwc_layer, size_t) {
    if (hwc_layer->compositionType == HWC_FRAMEBUFFER) {
        hwc_layer->compositionType = HWC_OVERLAY;
    }
    // TODO: Handle HWC_SIDEBAND
    return 0;
}

int multi_window_mode::setup_set(waydroid_hwc_composer_device_1* pdev, hwc_display_contents_1_t* contents) {
    layer_infos.setup(pdev, contents->hwLayers, contents->numHwLayers);
    return 0;
}

int multi_window_mode::cleanup_stale_windows(waydroid_hwc_composer_device_1* pdev,
                                             hwc_display_contents_1_t* contents) {
    close_windows_for_dead_tasks(pdev);

    /* A skipped layer still names its task: animation frames mark app layers
     * HWC_SKIP_LAYER, so requiring can_handle_layer() here destroyed and
     * recreated the toplevel across every animation. */
    auto named_in_frame = [&](const std::string &key) {
        for (size_t i = 0; i < contents->numHwLayers; ++i) {
            const auto &layer_info = layer_infos[i];
            if (layer_info.type != LayerSplitType::Empty && layer_info.key() == key)
                return true;
        }
        return false;
    };

    /* Keep app cards for backgrounded tasks alive as snapshots; closing them
     * closed the host card, and dropping to zero windows forces a wayland
     * reconnect (splash flicker, qtmir session churn). Helper windows
     * (InputMethod, full-ui leftovers) still close with their layers. */
    pdev->display->windows.erase_if([&](const auto& it){
        if (named_in_frame(it.first))
            return false;
        const auto& taskID = it.second->taskID;
        return taskID == "none" || taskID == "0";
    });

    for (auto const& [key, window] : pdev->display->windows) {
        if (!named_in_frame(key) && !window->snapshot_buffer && !window->snapshot_unavailable) {
            pdev->display->egl_work_queue.push_back(std::bind(snapshot_inactive_app_window, pdev->display, window.get()));
        }
    }
    if (!pdev->display->egl_work_queue.empty()) {
        sem_post(&pdev->display->egl_go);
        sem_wait(&pdev->display->egl_done);
    }

    /* Without WMS events, drop close-pending marks only once the task's
     * layers are gone (clearing every frame recreated the card mid-teardown).
     * With events, taskRemoved is the authoritative clear. */
    if (!pdev->display->task_events_seen) {
        for (auto &[tid, task] : pdev->display->tasks) {
            if (task.closing && !named_in_frame(tid))
                task.closing = false;
        }
    }

    return 0;
}

int multi_window_mode::handle_layer(waydroid_hwc_composer_device_1* pdev, hwc_layer_1* hwc_layer, size_t i) {
    if (!can_handle_layer(*hwc_layer)) {
        if (!(hwc_layer->flags & HWC_SKIP_LAYER) && hwc_layer->compositionType != HWC_FRAMEBUFFER_TARGET) {
            // TODO: Support HWC_BACKGROUND
            ALOGW("Encountered unsupported compositionType: %" PRId32, hwc_layer->compositionType);
        }
        if (hwc_layer->acquireFenceFd != -1) {
            close(hwc_layer->acquireFenceFd);
        }
        return 0;
    }

    assert(hwc_layer->handle);
    window *window = get_window(pdev, layer_infos[i]);

    if (window) {
        return apply_hwc_layer_to_window(pdev, hwc_layer, i, window);
    } else {
        if (hwc_layer->acquireFenceFd != -1) {
            close(hwc_layer->acquireFenceFd);
        }
        return 0;
    }
}

int multi_window_mode::post_processing(waydroid_hwc_composer_device_1 *pdev, hwc_display_contents_1_t *contents) {
    if (contents->flags & HWC_GEOMETRY_CHANGED) {
        neutralize_remaining_subsurfaces(pdev);
    }
    return 0;
}
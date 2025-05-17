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

window *multi_window_mode::get_window(waydroid_hwc_composer_device_1 *pdev, layer_info &layer_info) {
    auto &windows = pdev->display->windows;
    if (layer_info.type == LayerSplitType::TID) {
        // Create windows based on Task ID in layer name
        if (is_blacklisted(pdev, layer_info.aid, layer_info.component))
            return nullptr;

        auto it = windows.find(layer_info.tid);
        if (it != windows.end()) {
            return it->second.get();
        } else {
            // TODO: Reduce code duplication
            auto res = windows.emplace(
                layer_info.tid,
                window::create(pdev->display, pdev->should_compose, layer_info.aid, layer_info.tid, color_transparent)
            );
            assert(res.second);
            std::string windows_size_str = std::to_string(pdev->display->windows.size());
            property_set("waydroid.open_windows", windows_size_str.c_str());
            return res.first->second.get();
        }
    } else if (layer_info.type == LayerSplitType::RawName) {
        if (layer_info.aid == "InputMethod") {
            auto it = windows.find("InputMethod");
            if (it != windows.end()) {
                return it->second.get();
            } else {
                // TODO: Reduce code duplication
                auto res = windows.emplace(
                    "InputMethod",
                    window::create(pdev->display, pdev->should_compose, "InputMethod", "none", color_transparent)
                );
                assert(res.second);
                std::string windows_size_str = std::to_string(pdev->display->windows.size());
                property_set("waydroid.open_windows", windows_size_str.c_str());
                return res.first->second.get();
            }
        }
    }
    return nullptr;
}

bool multi_window_mode::can_handle_layer(const hwc_layer_1& layer) {
    return layer.compositionType == HWC_OVERLAY && !(layer.flags & HWC_SKIP_LAYER);
}

int multi_window_mode::setup(waydroid_hwc_composer_device_1* pdev, hwc_display_contents_1_t* contents) {
    layer_infos.setup(pdev, contents->hwLayers, contents->numHwLayers);
    return 0;
}

int multi_window_mode::cleanup_stale_windows(waydroid_hwc_composer_device_1* pdev,
                                             hwc_display_contents_1_t* contents) {
    auto &windows = pdev->display->windows;
    for (auto it = windows.begin(); it != windows.end();) {
        bool found_app = false;
        for (size_t i = 0; i < contents->numHwLayers; ++i) {
            const auto &layer_info = layer_infos[i];
            const auto &layer = contents->hwLayers[i];
            if (!can_handle_layer(layer))
                continue;
            if ((layer_info.type == LayerSplitType::TID && layer_info.tid == it->first)
                || (layer_info.type == LayerSplitType::RawName && layer_info.aid == it->first)) {
                found_app = true;
                break;
            }
        }
        if (found_app) {
            ++it;
        } else {
            it = windows.erase(it);
        }
    }

    std::string windows_size_str = std::to_string(windows.size());
    property_set("waydroid.open_windows", windows_size_str.c_str());

    pdev->display->ignored_apps.clear();

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
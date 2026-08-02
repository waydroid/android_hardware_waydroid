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

#include "waydroid_mode.h"

#include <cutils/properties.h>

void clear_open_windows(waydroid_hwc_composer_device_1 *pdev) {
    pdev->display->windows.clear();
}

void neutralize_remaining_subsurfaces(waydroid_hwc_composer_device_1 *pdev) {
    auto& windows = pdev->display->windows;
    for (const auto& [key, window] : windows) {
        // Nothing was rendered. Skip
        if (window->lastLayer == 0)
            continue;
        // Neutralize unused surfaces
        for (size_t l = window->lastLayer; l < window->layers.size(); ++l) {
            window->layers[l].attach_buffer(nullptr);
            wl_surface_commit(window->layers[l].surface);
        }
    }
}


void skipped_layers_helper::setup_for_each_layer(size_t i, const hwc_layer_1 &layer) {
    if (layer.flags & HWC_SKIP_LAYER) {
        if (m_first_skipped == UNSET_VALUE) {
            m_first_skipped = i;
        }
        m_last_skipped = i;
    }
    if (layer.compositionType == HWC_FRAMEBUFFER_TARGET) {
        m_framebuffer_target_index = i;
    }
}

void skipped_layers_helper::setup_set(hwc_layer_1* layers, size_t size) {
    if (m_framebuffer_target_index >= size) {
        ALOGE("layers changed between prepare and set");
        abort();
    }
    m_framebuffer_target = &layers[m_framebuffer_target_index];
}

size_t skipped_layers_helper::first_skipped() const {
    return m_first_skipped;
}
size_t skipped_layers_helper::last_skipped() const {
    return m_last_skipped;
}

hwc_layer_1 *skipped_layers_helper::framebuffer_target_layer() const {
    return m_framebuffer_target;
}
size_t skipped_layers_helper::framebuffer_target_index() const {
    return m_framebuffer_target_index;
}

bool skipped_layers_helper::has_skipped_layers() const {
    return m_first_skipped != UNSET_VALUE;
}


layer_info split_layer_names_helper::split_layer_name(const std::string &layer_name) {
    using namespace std::string_literals;
    if (layer_name.empty()) {
        return {
                LayerSplitType::Empty,
                std::string(),
                std::string()
        };
    } else if (layer_name.compare(0, 4, "TID:") == 0) {
        const auto hash_pos = layer_name.find('#');
        const auto slash_pos = layer_name.find('/');
        const auto hash_after_slash_pos = layer_name.find('#', slash_pos);

        return {
                LayerSplitType::TID,
                layer_name.substr(hash_pos + 1, slash_pos - hash_pos - 1),
                layer_name.substr(4, hash_pos - 4),
                layer_name.substr(slash_pos + 1, hash_after_slash_pos - slash_pos - 1)
        };
    } else {
        const auto hash_pos = layer_name.find('#');

        return {
                LayerSplitType::RawName,
                layer_name.substr(0, hash_pos),
                "none"s
        };
    }
}

void split_layer_names_helper::setup(waydroid_hwc_composer_device_1 *pdev, const hwc_layer_1 *layers, size_t count_of_layers) {
    layer_infos.reserve(count_of_layers);
    for (size_t l = 0; l < count_of_layers; l++) {
        const auto& name = [&](){
            if (layers[l].compositionType == HWC_FRAMEBUFFER_TARGET) {
                // SurfaceFlinger does not write a layer name for the FRAMEBUFFER_TARGET layer.
                // Make sure we don't use the stale layer name entry
                return std::string();
            } else {
                return pdev->display->layer_names[l];
            }
        }();
        layer_infos.push_back(split_layer_name(name));
    }
}

const std::string& layer_info::key() const {
    if (type == LayerSplitType::TID) {
        return tid;
    } else if (type == LayerSplitType::RawName) {
        return aid;
    } else {
        abort();
    }
}

layer_info &split_layer_names_helper::operator[](size_t i) {
    return layer_infos.at(i);
}
const std::vector<layer_info> &split_layer_names_helper::container() const {
    return layer_infos;
}
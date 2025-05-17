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

#include "waydroid_mode.h"

template<class T>
class compositing_window_mode : public virtual waydroid_mode {
    skipped_layers_helper skipped_layers;

    T *derived() {
        return static_cast<T *>(this);
    }

  public:
    int setup(waydroid_hwc_composer_device_1 *, hwc_display_contents_1_t *contents) override {
        for (size_t i = 0; i < contents->numHwLayers; ++i) {
            skipped_layers.setup_for_each_layer(i, contents->hwLayers[i]);
        }
        return 0;
    }
    int handle_layer(waydroid_hwc_composer_device_1 *pdev, hwc_layer_1 *hwc_layer, size_t i) override {
        if (i == skipped_layers.first_skipped()) {
            // Draw the content composited by SurfaceFlinger here
            if (hwc_layer->acquireFenceFd != -1) {
                close(hwc_layer->acquireFenceFd);
            }

            hwc_layer = skipped_layers.framebuffer_target_layer();
            assert(hwc_layer);
            i = skipped_layers.framebuffer_target_index();
        } else if (skipped_layers.first_skipped() < i && i <= skipped_layers.last_skipped()) {
            if (hwc_layer->acquireFenceFd != -1) {
                close(hwc_layer->acquireFenceFd);
            }
            return 0;
        } else if (hwc_layer == skipped_layers.framebuffer_target_layer()) {
            /*
             * There are two cases:
             * 1. We have skipped layers: In this case the framebuffer_target layer will be drawn at i = skipped_layers.first_skipped(),
             *    thus we can skip it here
             * 2. We have no skipped layers: In this case SurfaceFlinger has not done any compositing
             *    thus we can also skip the framebuffer_target layer.
             *    In this case we have to close acquireFenceFd though
             */
            if (!skipped_layers.has_skipped_layers() && hwc_layer->acquireFenceFd != -1) {
                close(hwc_layer->acquireFenceFd);
            }
            return 0;
        } else if (hwc_layer->compositionType != HWC_OVERLAY) {
            ALOGW("Encountered unsupported compositionType: %" PRId32, hwc_layer->compositionType);
            // TODO: Support HWC_BACKGROUND
            if (hwc_layer->acquireFenceFd != -1) {
                close(hwc_layer->acquireFenceFd);
            }
            return 0;
        } else if (!hwc_layer->handle) {
            return 0;
        }

        window *window = derived()->get_window(pdev);
        return apply_hwc_layer_to_window(pdev, hwc_layer, i, window);
    }

    int post_processing(waydroid_hwc_composer_device_1 *pdev, hwc_display_contents_1_t *contents) override {
        if (contents->flags & HWC_GEOMETRY_CHANGED) {
            neutralize_remaining_subsurfaces(pdev);
        }
        return 0;
    }
};
template<class T>
class non_compositing_window_mode : public virtual waydroid_mode {
    T *derived() {
        return static_cast<T *>(this);
    }

  public:
    int handle_layer(waydroid_hwc_composer_device_1 *pdev, hwc_layer_1 *hwc_layer, size_t i) override {
        if (hwc_layer->compositionType == HWC_FRAMEBUFFER_TARGET) {
            assert(hwc_layer->handle);
            window *window = derived()->get_window(pdev);

            return apply_hwc_layer_to_window(pdev, hwc_layer, i, window);
        } else {
            if (hwc_layer->acquireFenceFd != -1) {
                close(hwc_layer->acquireFenceFd);
            }
            return 0;
        }
    }
};
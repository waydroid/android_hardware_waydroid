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

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <hardware/hwcomposer.h>

struct display;
struct window;
struct wl_output;
struct wl_registry;
struct waydroid_hwc_composer_device_1;

// HDR capabilities of the host output, as reported through HWC2 getHdrCapabilities
struct hdr_capabilities {
    std::vector<int32_t> types; // android_hdr_t
    float max_luminance = 0;
    float max_average_luminance = 0;
    float min_luminance = 0;
};

void hdr_bind_color_manager(struct display *display, struct wl_registry *registry, uint32_t id, uint32_t version);
void hdr_output_added(struct display *display, uint32_t name, struct wl_output *output);
void hdr_output_removed(struct display *display, uint32_t name);
// Reads the outputs' image descriptions; call before the Wayland thread starts
void hdr_init(struct display *display);
// Handles the outputs' image description events; only the Wayland thread calls this
void hdr_dispatch_outputs(struct display *display);
void hdr_destroy_display(struct display *display);
hdr_capabilities hdr_get_capabilities(struct display *display);

// Whether the layer is a 10-bit YUV video layer
bool hdr_is_video_layer(waydroid_hwc_composer_device_1 *pdev, const hwc_layer_1 &layer, size_t index);
// Whether the HDR path draws the layer itself: BT.2020 PQ or HLG video of a type Android was told about
bool hdr_takes_layer(waydroid_hwc_composer_device_1 *pdev, const hwc_layer_1 &layer, size_t index);
// Draws the layer on the window's HDR subsurface, below every other layer of the window
int hdr_apply_layer_to_window(waydroid_hwc_composer_device_1 *pdev, hwc_layer_1 *layer, size_t index, struct window *window);
// Clears the HDR subsurface of a window that had no HDR layer this frame
void hdr_finish_window_frame(struct window *window);
void hdr_destroy_window(struct window *window);

// Called by the HWC2 adapter (libhwc2on1adapter) through dlsym on the module
extern "C" {
int waydroid_hwc_get_hdr_capabilities(struct hwc_composer_device_1 *dev, int disp, uint32_t *num_types,
        int32_t *types, float *max_luminance, float *max_average_luminance, float *min_luminance);
void waydroid_hwc_set_layer_dataspaces(struct hwc_composer_device_1 *dev, int disp,
        const int32_t *dataspaces, size_t count);
}

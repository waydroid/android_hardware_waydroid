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

class multi_window_mode : public waydroid_mode {
    split_layer_names_helper layer_infos;

    static window *get_window(waydroid_hwc_composer_device_1 *pdev, layer_info &layer_info);
    static bool can_handle_layer(const hwc_layer_1& layer);

  public:
    int setup(waydroid_hwc_composer_device_1 *pdev, hwc_display_contents_1_t *contents) final;
    int cleanup_stale_windows(waydroid_hwc_composer_device_1* pdev,
                              hwc_display_contents_1_t* contents) final;
    int handle_layer(waydroid_hwc_composer_device_1 *pdev, hwc_layer_1 *hwc_layer, size_t hwc_layer_index) final;
    int post_processing(waydroid_hwc_composer_device_1 *pdev, hwc_display_contents_1_t *contents) final;
};
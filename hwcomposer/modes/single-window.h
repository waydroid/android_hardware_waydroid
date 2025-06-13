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

#include "waydroid_mode_impl.h"

class single_window_mode_base : public virtual waydroid_mode {
  protected:
    split_layer_names_helper layer_infos;

    std::string target_layer_tid;
    std::string target_layer_aid;

    bool should_show() const;
    window *get_window(waydroid_hwc_composer_device_1 *pdev) const;

    int setup_set(waydroid_hwc_composer_device_1 *pdev, hwc_display_contents_1_t *contents) override;
    int cleanup_stale_windows(waydroid_hwc_composer_device_1* pdev,
                              hwc_display_contents_1_t* contents) final;
};

class non_compositing_single_window_mode : public single_window_mode_base, public non_compositing_window_mode<non_compositing_single_window_mode> {
    using Base = non_compositing_window_mode<non_compositing_single_window_mode>;
    friend Base;
    using single_window_mode_base::get_window;

    int setup_set(waydroid_hwc_composer_device_1 *pdev, hwc_display_contents_1_t *contents) final;
    int handle_layer(waydroid_hwc_composer_device_1 *pdev, hwc_layer_1 *hwc_layer, size_t hwc_layer_index) final;
};

class compositing_single_window_mode : public single_window_mode_base, public compositing_window_mode<compositing_single_window_mode> {
    using Base = compositing_window_mode<compositing_single_window_mode>;
    friend Base;
    using single_window_mode_base::get_window;

    int setup_set(waydroid_hwc_composer_device_1 *pdev, hwc_display_contents_1_t *contents) final;
    int handle_layer(waydroid_hwc_composer_device_1 *pdev, hwc_layer_1 *hwc_layer, size_t hwc_layer_index) final;
};
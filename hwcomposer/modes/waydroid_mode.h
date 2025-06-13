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

#include "hwcomposer.h"

#include <hardware/hwcomposer.h>
#include <log/log.h>

#include <wayland-client.h>

#include <utility>

struct waydroid_mode {
    virtual ~waydroid_mode() = default;

    // Called during hwc_prepare
    virtual int setup_prepare(waydroid_hwc_composer_device_1 *, hwc_display_contents_1_t *) { return 0; }
    virtual int prepare(hwc_layer_1 *, size_t) { return 0; }

    // Called during hwc_set
    virtual int setup_set(waydroid_hwc_composer_device_1 *, hwc_display_contents_1_t *) { return 0; }
    virtual int cleanup_stale_windows(waydroid_hwc_composer_device_1* pdev,
                                      hwc_display_contents_1_t* contents) = 0;
    virtual int handle_layer(waydroid_hwc_composer_device_1 *pdev, hwc_layer_1 *hwc_layer, size_t hwc_layer_index) = 0;
    virtual int post_processing(waydroid_hwc_composer_device_1 *, hwc_display_contents_1_t *) { return 0; }
};


void clear_open_windows(waydroid_hwc_composer_device_1 *pdev);
void neutralize_remaining_subsurfaces(waydroid_hwc_composer_device_1 *pdev);

class skipped_layers_helper {
    constexpr static size_t UNSET_VALUE = std::numeric_limits<size_t>::max();

    // Initialized during prepare
    size_t m_first_skipped {UNSET_VALUE};
    size_t m_last_skipped {UNSET_VALUE};
    size_t m_framebuffer_target_index {UNSET_VALUE};

    // Initialized during set
    hwc_layer_1 *m_framebuffer_target {};

  public:
    void setup_for_each_layer(size_t i, const hwc_layer_1 &layer);
    void setup_set(hwc_layer_1 *layers, size_t size);

    // Safe to call after setup_for_each_layer
    size_t first_skipped() const;
    size_t last_skipped() const;

    size_t framebuffer_target_index() const;

    bool has_skipped_layers() const;

    // Safe to call after setup_set
    hwc_layer_1 *framebuffer_target_layer() const;
};

enum class LayerSplitType {
    TID,
    RawName,
    Empty
};
struct layer_info {
    LayerSplitType type;
    std::string aid;
    std::string tid;
    std::string component {};

    const std::string &key() const;
};
class split_layer_names_helper {
    std::vector<layer_info> layer_infos;

    static layer_info split_layer_name(const std::string &layer_name);

  public:
    void setup(waydroid_hwc_composer_device_1 *pdev, const hwc_layer_1 *layers, size_t count_of_layers);

    layer_info &operator[](size_t i);
    const std::vector<layer_info> &container() const;
};
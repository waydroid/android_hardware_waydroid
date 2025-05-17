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

#include "full-ui.h"

#include <cinttypes>

#include <log/log.h>
#include <cutils/properties.h>

window *full_ui_mode_base::get_window(waydroid_hwc_composer_device_1 *pdev) {
    auto& windows = pdev->display->windows;
    assert(windows.size() <= 1);
    if (windows.size() == 1) {
        // The full-ui windows should be the only one after cleanup_stale_windows
        auto it = windows.begin();
        assert(it != windows.end() && it->first == "Waydroid");
        return it->second.get();
    } else {
        auto res = windows.emplace(
            "Waydroid",
            window::create(pdev->display, pdev->should_compose, "Waydroid", "0", {0, 0, 0, 255})
        );
        assert(res.second);
        assert(windows.size() == 1);
        property_set("waydroid.open_windows", "1");
        return res.first->second.get();
    }
}

int full_ui_mode_base::cleanup_stale_windows(waydroid_hwc_composer_device_1* pdev,
                                             hwc_display_contents_1_t*) {
    // Clear all open windows if there's any and just keep "Waydroid"
    auto &windows = pdev->display->windows;
    auto it = windows.begin();
    while (it != windows.end()) {
        if (it->first == "Waydroid") {
            ++it;
            continue;
        }

        windows.erase(it++);
    }

    std::string windows_size_str = std::to_string(windows.size());
    property_set("waydroid.open_windows", windows_size_str.c_str());

    return 0;
}
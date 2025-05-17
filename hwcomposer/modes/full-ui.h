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

class full_ui_mode_base : public virtual waydroid_mode {
  public:
    int cleanup_stale_windows(waydroid_hwc_composer_device_1* pdev,
                              hwc_display_contents_1_t* contents) final;

  protected:
    static window *get_window(waydroid_hwc_composer_device_1 *pdev);
};

class non_compositing_full_ui_mode : public full_ui_mode_base, public non_compositing_window_mode<non_compositing_full_ui_mode> {
    friend non_compositing_window_mode<non_compositing_full_ui_mode>;
    using full_ui_mode_base::get_window;
};

class compositing_full_ui_mode : public full_ui_mode_base, public compositing_window_mode<compositing_full_ui_mode> {
    friend compositing_window_mode<compositing_full_ui_mode>;
    using full_ui_mode_base::get_window;
};
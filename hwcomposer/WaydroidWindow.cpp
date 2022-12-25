/*
 * Copyright (C) 2022 The Waydroid Project
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

#include "WaydroidWindow.h"

#include <cutils/properties.h>
#include "xdg-shell-client-protocol.h"

namespace vendor::waydroid::window::implementation {

WaydroidWindow::WaydroidWindow(struct display *display)
    : mDisplay(display)
{
}

// Methods from ::vendor::waydroid::window::V1_0::IWaydroidWindow follow.
Return<bool> WaydroidWindow::minimize(const hidl_string& packageName) {
    char property[PROPERTY_VALUE_MAX];

    if (!mDisplay->wm_base)
        return false;

    property_get("waydroid.active_apps", property, "Waydroid");
    if (!strcmp(property, "Waydroid"))
        return false;

    for (auto it = mDisplay->windows.begin(); it != mDisplay->windows.end(); it++) {
        struct window* window = it->second;
        if (window && window->appID == packageName) {
            xdg_toplevel_set_minimized(window->xdg_toplevel);
            return true;
        }
    }
    return false;
}

}  // namespace vendor::waydroid::window::implementation

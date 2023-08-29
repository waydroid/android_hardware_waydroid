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
#include <log/log.h>

#include "xdg-shell-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "idle-inhibit-unstable-v1-client-protocol.h"

namespace vendor::waydroid::window::implementation {

WaydroidWindow::WaydroidWindow(struct display *display)
    : mDisplay(display)
{
}

static const struct zwp_relative_pointer_v1_listener relative_pointer_listener = {
    handle_relative_motion,
};

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

// Methods from ::vendor::waydroid::window::V1_1::IWaydroidWindow follow.
Return<void> WaydroidWindow::setPointerCapture(const hidl_string& packageName, bool enabled) {
    char property[PROPERTY_VALUE_MAX];
    std::string windowName = packageName;

    if (!mDisplay->pointer_constraints)
        return Void();

    property_get("waydroid.active_apps", property, "Waydroid");
    if (!strcmp(property, "Waydroid"))
        windowName = "Waydroid";

    for (auto it = mDisplay->windows.begin(); it != mDisplay->windows.end(); it++) {
        struct window* window = it->second;
        if (window && window->appID == windowName) {
            if (enabled && window->locked_pointer == nullptr) {
                window->locked_pointer = zwp_pointer_constraints_v1_lock_pointer(
                        mDisplay->pointer_constraints,
                        window->surface, mDisplay->pointer, nullptr,
                        ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
                if (mDisplay->relative_pointer == nullptr) {
                    mDisplay->relative_pointer = zwp_relative_pointer_manager_v1_get_relative_pointer(
                            mDisplay->relative_pointer_manager, mDisplay->pointer);
                    zwp_relative_pointer_v1_add_listener(mDisplay->relative_pointer, &relative_pointer_listener, mDisplay);
                }
            } else if (!enabled && window->locked_pointer != nullptr) {
                zwp_locked_pointer_v1_destroy(window->locked_pointer);
                window->locked_pointer = nullptr;
                bool anyLocks = false;
                for (auto jt = mDisplay->windows.begin(); jt != mDisplay->windows.end(); jt++) {
                    struct window* window = jt->second;
                    if (window->locked_pointer) {
                        anyLocks = true;
                        break;
                    }
                }
                if (!anyLocks) {
                    zwp_relative_pointer_v1_destroy(mDisplay->relative_pointer);
                    mDisplay->relative_pointer = nullptr;
                }
            }
            break;
        }
    }
    return Void();
}

// Methods from ::vendor::waydroid::window::V1_2::IWaydroidWindow follow.
Return<void> WaydroidWindow::setIdleInhibit(const hidl_string& task, bool enabled) {
    char property[PROPERTY_VALUE_MAX];
    std::string taskID = task;

    if (!mDisplay->idle_manager)
        return Void();

    property_get("waydroid.active_apps", property, "Waydroid");
    if (!strcmp(property, "Waydroid"))
        taskID = "0";

    for (auto it = mDisplay->windows.begin(); it != mDisplay->windows.end(); it++) {
        struct window* window = it->second;
        if (window && window->isActive && (window->taskID == taskID || taskID == "*")) {
            ALOGI("%sinhibiting sleep from %s#%s", enabled ? "" : "not ", window->appID.c_str(), window->taskID.c_str());
            if (enabled && window->idle_inhibitor == nullptr) {
                window->idle_inhibitor = zwp_idle_inhibit_manager_v1_create_inhibitor(
                        mDisplay->idle_manager,
                        window->surface);
            } else if (!enabled && window->idle_inhibitor != nullptr) {
                zwp_idle_inhibitor_v1_destroy(window->idle_inhibitor);
                window->idle_inhibitor = nullptr;
            }
            break;
        }
    }
    return Void();
}

}  // namespace vendor::waydroid::window::implementation

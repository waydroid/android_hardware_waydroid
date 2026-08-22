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

    if (!mDisplay->ctl->wm_base)
        return false;

    property_get("waydroid.active_apps", property, "Waydroid");
    if (!strcmp(property, "Waydroid"))
        return false;

    std::scoped_lock lock(mDisplay->windowsMutex);
    for (auto& [id, window] : mDisplay->windows){
        if (window->appID == packageName) {
            window->minimize();
            return true;
        }
    }
    return false;
}

// Methods from ::vendor::waydroid::window::V1_1::IWaydroidWindow follow.
Return<void> WaydroidWindow::setPointerCapture(const hidl_string& packageName, bool enabled) {
    char property[PROPERTY_VALUE_MAX];
    std::string windowName = packageName;

    if (!mDisplay->ctl->pointer_constraints)
        return Void();

    if (!mDisplay->ctl->pointer)
        return Void();

    property_get("waydroid.active_apps", property, "Waydroid");
    if (!strcmp(property, "Waydroid"))
        windowName = "Waydroid";

    std::scoped_lock lock(mDisplay->windowsMutex);
    for (auto& [id, window] : mDisplay->windows) {
        if (window->appID == windowName) {
            ALOGI("%slocking pointer for %s#%s", enabled ? "" : "un", window->appID.c_str(), window->taskID.c_str());
            /* Lock both the toplevel and all subsurfaces:
             * https://gitlab.freedesktop.org/wayland/wayland-protocols/-/issues/287
             */
            for (auto& layer : window->layers) {
                if (enabled && !layer.locked_pointer) {
                    layer.locked_pointer = zwp_pointer_constraints_v1_lock_pointer(
                            mDisplay->ctl->pointer_constraints,
                            layer.surface, mDisplay->ctl->pointer, nullptr,
                            ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
                } else if (!enabled && layer.locked_pointer) {
                    zwp_locked_pointer_v1_destroy(layer.locked_pointer);
                    layer.locked_pointer = nullptr;
                }
            }
            if (enabled && window->dedicated_background_surface && !window->locked_pointer) {
                window->locked_pointer = zwp_pointer_constraints_v1_lock_pointer(
                        mDisplay->ctl->pointer_constraints,
                        window->surface, mDisplay->ctl->pointer, nullptr,
                        ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
            } else if (!enabled && window->dedicated_background_surface && window->locked_pointer) {
                zwp_locked_pointer_v1_destroy(window->locked_pointer);
                window->locked_pointer = nullptr;
            }

            if (enabled && !mDisplay->ctl->relative_pointer) {
                mDisplay->ctl->relative_pointer = zwp_relative_pointer_manager_v1_get_relative_pointer(
                        mDisplay->ctl->relative_pointer_manager, mDisplay->ctl->pointer);
                zwp_relative_pointer_v1_add_listener(mDisplay->ctl->relative_pointer, &relative_pointer_listener, mDisplay);
            } else if (!enabled && mDisplay->ctl->relative_pointer) {
                bool any_locks =
                    window->locked_pointer ||
                    std::any_of(mDisplay->windows.begin(), mDisplay->windows.end(), [](auto& pair) {
                        return std::any_of(pair.second->layers.begin(), pair.second->layers.end(), [](auto& layer) {
                            return layer.locked_pointer;
                        });
                    });
                if (!any_locks) {
                    zwp_relative_pointer_v1_destroy(mDisplay->ctl->relative_pointer);
                    mDisplay->ctl->relative_pointer = nullptr;
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

    if (!mDisplay->ctl->idle_manager)
        return Void();

    property_get("waydroid.active_apps", property, "Waydroid");
    if (!strcmp(property, "Waydroid"))
        taskID = "0";

    std::scoped_lock lock(mDisplay->windowsMutex);
    for (auto& [id, window] : mDisplay->windows) {
        if (window && (window->taskID == taskID || taskID == "*")) {
            ALOGI("%sinhibiting sleep for %s#%s", enabled ? "" : "un", window->appID.c_str(), window->taskID.c_str());
            if (enabled && window->idle_inhibitor == nullptr) {
                window->idle_inhibitor = zwp_idle_inhibit_manager_v1_create_inhibitor(
                        mDisplay->ctl->idle_manager,
                        window->surface);
            } else if (!enabled && window->idle_inhibitor != nullptr) {
                zwp_idle_inhibitor_v1_destroy(window->idle_inhibitor);
                window->idle_inhibitor = nullptr;
            }
        }
    }
    return Void();
}

// Methods from ::vendor::waydroid::window::V1_3::IWaydroidWindow follow.
Return<void> WaydroidWindow::taskCreated(uint32_t taskID, const hidl_string& packageName,
                                         const hidl_string& componentName) {
    std::string tid = std::to_string(taskID);
    std::scoped_lock lock(mDisplay->windowsMutex);
    mDisplay->task_events_seen = true;

    auto it = mDisplay->tasks.find(tid);
    if (it != mDisplay->tasks.end() && it->second.closing) {
        /* Android only hands out a task ID once the old task is gone, so this
         * is the taskRemoved we never got: retire the old card rather than
         * refuse the new task's. Snapshots resync the table now, so this can
         * no longer be a resync replaying taskCreated mid-close. */
        ALOGI("taskCreated %s (%s): ID reused while close pending, retiring the old card",
              tid.c_str(), packageName.c_str());
        if (mDisplay->forget_task(tid) && mDisplay->ctl->wl_alive.load())
            wl_display_flush(mDisplay->ctl->display);
    }
    auto &task = mDisplay->tasks[tid];
    task.appID = packageName;
    task.component = componentName;
    task.from_layer = false;
    ALOGI("taskCreated %s %s/%s", tid.c_str(), packageName.c_str(), componentName.c_str());
    return Void();
}

Return<void> WaydroidWindow::taskRemoved(uint32_t taskID) {
    std::string tid = std::to_string(taskID);
    std::scoped_lock lock(mDisplay->windowsMutex);
    mDisplay->task_events_seen = true;

    bool had_window = mDisplay->forget_task(tid);
    ALOGI("taskRemoved %s%s", tid.c_str(), had_window ? ": closed its card" : "");
    /* hwc_set flushes each frame, but with the display asleep SF posts no
     * frames and the surface destruction would sit in the send buffer. */
    if (had_window && mDisplay->ctl->wl_alive.load())
        wl_display_flush(mDisplay->ctl->display);
    return Void();
}

Return<void> WaydroidWindow::taskFocusChanged(uint32_t taskID, bool focused) {
    std::string tid = std::to_string(taskID);
    std::scoped_lock lock(mDisplay->windowsMutex);
    mDisplay->task_events_seen = true;

    auto it = mDisplay->tasks.find(tid);
    if (it == mDisplay->tasks.end()) {
        if (!focused)
            return Void();
        ALOGW("taskFocusChanged %s: unknown task, self-healing an entry", tid.c_str());
        it = mDisplay->tasks.emplace(tid, task_info{}).first;
    }
    if (focused) {
        /* WMS brought the task to front, so it is not going away: a stale
         * close-pending mark (lost taskRemoved) would refuse its posts and
         * the relaunched app would never get a card. */
        if (it->second.closing) {
            ALOGW("taskFocusChanged %s: focused while close pending, dropping the mark", tid.c_str());
            it->second.closing = false;
        }
        for (auto &[other_tid, task] : mDisplay->tasks)
            task.focused = (other_tid == tid);
    } else {
        it->second.focused = false;
    }
    ALOGI("taskFocusChanged %s focused=%d", tid.c_str(), focused);
    return Void();
}

Return<void> WaydroidWindow::taskListSnapshot(
        uint32_t generation,
        const hidl_vec<V1_3::IWaydroidWindow::TaskInfo>& tasks) {
    std::scoped_lock lock(mDisplay->windowsMutex);
    mDisplay->task_events_seen = true;
    mDisplay->task_generation = generation;

    std::set<std::string> live;
    for (const auto &t : tasks)
        live.insert(std::to_string(t.taskID));

    /* The snapshot is the whole truth, so anything missing from it is gone.
     * This earns its keep on a framework restart under a surviving composer:
     * every entry left by the old system_server is stale, and their frozen
     * cards are invisible to close_windows_for_dead_tasks, which only closes
     * windows whose task is absent from the table. */
    std::vector<std::string> dead;
    for (const auto &[tid, task] : mDisplay->tasks)
        if (!live.count(tid))
            dead.push_back(tid);

    size_t closed = 0;
    for (const auto &tid : dead)
        closed += mDisplay->forget_task(tid);

    /* Only let the snapshot move focus if it actually reports some: clearing
     * every flag on a snapshot that simply carries no focus would leave the
     * task gate refusing content for every card. */
    bool carries_focus = false;
    for (const auto &t : tasks)
        carries_focus |= t.focused;

    for (const auto &t : tasks) {
        auto &task = mDisplay->tasks[std::to_string(t.taskID)];
        /* WMS reports no component for identity-less system tasks; don't let
         * that erase an identity a layer name already gave us. */
        if (!t.packageName.empty()) {
            task.appID = t.packageName;
            task.component = t.componentName;
            task.from_layer = false;
        }
        if (carries_focus)
            task.focused = t.focused;
        /* closing is deliberately kept: the task still being listed means
         * Android has not finished removing it yet. */
    }

    ALOGI("taskListSnapshot #%u: %zu tasks, retired %zu (%zu cards closed)",
          generation, (size_t)tasks.size(), dead.size(), closed);
    /* See taskRemoved: a sleeping display posts no frames to flush behind. */
    if (closed && mDisplay->ctl->wl_alive.load())
        wl_display_flush(mDisplay->ctl->display);
    return Void();
}

}  // namespace vendor::waydroid::window::implementation

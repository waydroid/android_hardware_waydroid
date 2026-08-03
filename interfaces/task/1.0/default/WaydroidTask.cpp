/*
 * Copyright (C) 2021 The Waydroid Project
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

#include "WaydroidTask.h"

#include <android/api-level.h>
#include <log/log.h>
#include <utils/String16.h>
#include <utils/String8.h>

namespace vendor {
namespace waydroid {
namespace task {
namespace V1_0 {
namespace implementation {

/* This service outlives system_server (framework soft-reboots restart
 * system_server but not vendor HALs), so a cached proxy can point at a dead
 * binder and every call silently vanishes. Re-resolve when that happens. */
sp<IActivityTaskManager> WaydroidTask::getActivityTaskManager() {
    if (mActivityTaskManager != nullptr
        && !::android::IInterface::asBinder(mActivityTaskManager)->isBinderAlive()) {
        ALOGW("activity_task binder died (system_server restart?), re-resolving");
        mActivityTaskManager = nullptr;
    }
    if (mActivityTaskManager == nullptr) {
        sp<IBinder> binderTask = android::defaultServiceManager()->getService(android::String16("activity_task"));
        if (binderTask != nullptr)
            mActivityTaskManager = android::interface_cast<IActivityTaskManager>(binderTask);
    }
    return mActivityTaskManager;
}

sp<IPlatform> WaydroidTask::getPlatform() {
    if (mPlatform != nullptr
        && !::android::IInterface::asBinder(mPlatform)->isBinderAlive()) {
        ALOGW("waydroidplatform binder died (system_server restart?), re-resolving");
        mPlatform = nullptr;
    }
    if (mPlatform == nullptr) {
        sp<IBinder> binderPlatform = android::defaultServiceManager()->getService(android::String16("waydroidplatform"));
        if (binderPlatform != nullptr)
            mPlatform = android::interface_cast<IPlatform>(binderPlatform);
    }
    return mPlatform;
}

// Methods from ::vendor::waydroid::task::V1_0::IWaydroidTask follow.
Return<void> WaydroidTask::setFocusedTask(uint32_t taskID) {
    sp<IActivityTaskManager> atm = getActivityTaskManager();
    if (atm != nullptr) {
        auto status = atm->setFocusedTask(taskID);
        if (!status.isOk())
            ALOGE("setFocusedTask(%u) failed: %s", taskID, status.toString8().c_str());
    } else {
        ALOGE("setFocusedTask(%u): activity_task service unavailable", taskID);
    }
    return Void();
}

Return<void> WaydroidTask::removeTask(uint32_t taskID) {
    bool ret = false;
    sp<IActivityTaskManager> atm = getActivityTaskManager();
    if (atm != nullptr) {
        auto status = atm->removeTask(taskID, &ret);
        if (!status.isOk())
            ALOGE("removeTask(%u) failed: %s", taskID, status.toString8().c_str());
        else if (!ret)
            ALOGW("removeTask(%u): refused by ActivityTaskManager", taskID);
    } else {
        ALOGE("removeTask(%u): activity_task service unavailable", taskID);
    }
    return Void();
}

Return<void> WaydroidTask::removeAllVisibleRecentTasks() {
    sp<IActivityTaskManager> atm = getActivityTaskManager();
    if (atm != nullptr)
        atm->removeAllVisibleRecentTasks();
    return Void();
}

Return<void> WaydroidTask::getAppName(const hidl_string& packageName, getAppName_cb _hidl_cb) {
    android::String16 AppName;
    sp<IPlatform> platform = getPlatform();
    if (platform != nullptr)
        platform->getAppName(android::String16(packageName.c_str()), &AppName);
    android::String8 AppName8(AppName);
#if __ANDROID_API__ >= 34
    const char* OutAppName = AppName8.c_str();
#else
    const char* OutAppName = AppName8.string();
#endif
    if (strlen(OutAppName) == 0)
        OutAppName = packageName.c_str();
    _hidl_cb(OutAppName);
    return Void();
}

}  // namespace implementation
}  // namespace V1_0
}  // namespace task
}  // namespace waydroid
}  // namespace vendor

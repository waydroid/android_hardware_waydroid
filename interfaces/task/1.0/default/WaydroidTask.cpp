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

#include <thread>
#include <unistd.h>

#include <android/api-level.h>
#include <utils/String16.h>
#include <utils/String8.h>

namespace vendor {
namespace waydroid {
namespace task {
namespace V1_0 {
namespace implementation {

// Methods from ::vendor::waydroid::task::V1_0::IWaydroidTask follow.
Return<void> WaydroidTask::setFocusedTask(uint32_t taskID) {
    if (mActivityTaskManager == nullptr) {
        sp<IBinder> binderTask = android::defaultServiceManager()->getService(android::String16("activity_task"));
        if (binderTask != nullptr)
            mActivityTaskManager = android::interface_cast<IActivityTaskManager>(binderTask);
    }
    if (mActivityTaskManager != nullptr)
        mActivityTaskManager->setFocusedTask(taskID);
    return Void();
}

Return<void> WaydroidTask::removeTask(uint32_t taskID) {
    bool ret;
    if (mActivityTaskManager == nullptr) {
        sp<IBinder> binderTask = android::defaultServiceManager()->getService(android::String16("activity_task"));
        if (binderTask != nullptr)
            mActivityTaskManager = android::interface_cast<IActivityTaskManager>(binderTask);
    }
    if (mActivityTaskManager != nullptr)
        mActivityTaskManager->removeTask(taskID, &ret);
    return Void();
}

Return<void> WaydroidTask::removeAllVisibleRecentTasks() {
    if (mActivityTaskManager == nullptr) {
        sp<IBinder> binderTask = android::defaultServiceManager()->getService(android::String16("activity_task"));
        if (binderTask != nullptr)
            mActivityTaskManager = android::interface_cast<IActivityTaskManager>(binderTask);
    }
    if (mActivityTaskManager != nullptr)
        mActivityTaskManager->removeAllVisibleRecentTasks();
    return Void();
}

Return<void> WaydroidTask::getAppName(const hidl_string& packageName, getAppName_cb _hidl_cb) {
    android::String16 AppName;
    if (mPlatform == nullptr) {
        sp<IBinder> binderPlatform = android::defaultServiceManager()->getService(android::String16("waydroidplatform"));
        if (binderPlatform != nullptr)
            mPlatform = android::interface_cast<IPlatform>(binderPlatform);
    }
    if (mPlatform != nullptr)
        mPlatform->getAppName(android::String16(packageName.c_str()), &AppName);
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

// Methods from ::vendor::waydroid::task::V1_1::IWaydroidTask follow.
// The composer sends the first rate while Android boots, before system_server publishes
// waydroidplatform, and doesn't send it again, so a thread keeps the latest rate until it is set.
Return<void> WaydroidTask::setRefreshRate(uint32_t millihertz) {
    std::scoped_lock lock(mRefreshMutex);
    mRefreshRate = millihertz;
    mRefreshChanged.notify_one();
    if (!mRefreshThreadStarted) {
        mRefreshThreadStarted = true;
        std::thread(&WaydroidTask::applyRefreshRates, this).detach();
    }
    return Void();
}

void WaydroidTask::applyRefreshRates() {
    constexpr int32_t kSettingsSystem = 1;
    sp<IPlatform> platform;
    uint32_t applied = 0;
    for (;;) {
        uint32_t millihertz;
        {
            std::unique_lock lock(mRefreshMutex);
            mRefreshChanged.wait(lock, [&] { return mRefreshRate != applied; });
            millihertz = mRefreshRate;
        }
        if (platform == nullptr) {
            sp<IBinder> binderPlatform = android::defaultServiceManager()->getService(android::String16("waydroidplatform"));
            if (binderPlatform == nullptr) {
                sleep(1);
                continue;
            }
            platform = android::interface_cast<IPlatform>(binderPlatform);
        }
        android::String16 rate(android::String8::format("%u.%03u", millihertz / 1000, millihertz % 1000));
        // Lowering the minimum first keeps the range valid whichever way the rate moves
        if (platform->settingsPutString(kSettingsSystem, android::String16("min_refresh_rate"), android::String16("0")).isOk() &&
            platform->settingsPutString(kSettingsSystem, android::String16("peak_refresh_rate"), rate).isOk() &&
            platform->settingsPutString(kSettingsSystem, android::String16("min_refresh_rate"), rate).isOk()) {
            applied = millihertz;
        } else {
            // system_server restarted, or isn't ready yet
            platform = nullptr;
            sleep(1);
        }
    }
}

}  // namespace implementation
}  // namespace V1_0
}  // namespace task
}  // namespace waydroid
}  // namespace vendor

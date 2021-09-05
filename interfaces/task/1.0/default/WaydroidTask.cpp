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
    const char* OutAppName = android::String8(AppName).string();
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

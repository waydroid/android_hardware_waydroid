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

#pragma once

#include <binder/IBinder.h>
#include <binder/IServiceManager.h>
#include <vendor/waydroid/task/1.0/IWaydroidTask.h>
#include <hidl/MQDescriptor.h>
#include <hidl/Status.h>

#include <android/app/IActivityTaskManager.h>
#include <lineageos/waydroid/IPlatform.h>

namespace vendor {
namespace waydroid {
namespace task {
namespace V1_0 {
namespace implementation {

using ::android::hardware::hidl_array;
using ::android::hardware::hidl_memory;
using ::android::hardware::hidl_string;
using ::android::hardware::hidl_vec;
using ::android::hardware::Return;
using ::android::hardware::Void;
using ::android::sp;
using ::android::IBinder;

using ::android::app::IActivityTaskManager;
using ::lineageos::waydroid::IPlatform;

struct WaydroidTask : public IWaydroidTask {
    // Methods from ::vendor::waydroid::task::V1_0::IWaydroidTask follow.
    Return<void> removeAllVisibleRecentTasks() override;
    Return<void> removeTask(uint32_t taskID) override;
    Return<void> getAppName(const hidl_string& packageName, getAppName_cb _hidl_cb) override;
  private:
    sp<IActivityTaskManager> mActivityTaskManager;
    sp<IPlatform> mPlatform;
};

}  // namespace implementation
}  // namespace V1_0
}  // namespace task
}  // namespace waydroid
}  // namespace vendor

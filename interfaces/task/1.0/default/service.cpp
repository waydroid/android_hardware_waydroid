/*
 * Copyright 2021 The Waydroid Project
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

#define LOG_TAG "vendor.waydroid.task@1.0-service"

#include <android-base/logging.h>
#include <binder/ProcessState.h>
#include <hidl/HidlTransportSupport.h>

#include "WaydroidTask.h"

using android::hardware::configureRpcThreadpool;
using android::hardware::joinRpcThreadpool;

using android::hardware::hidl_string;
using android::hardware::Return;
using vendor::waydroid::task::V1_0::implementation::WaydroidTask;

using android::OK;
using android::status_t;

// A vendor manifest that declares only task@1.0 refuses the 1.1 registration, and the composer
// would wait for the service forever. This registers the same service as 1.0 there.
struct WaydroidTask1_0 : public vendor::waydroid::task::V1_0::IWaydroidTask {
    explicit WaydroidTask1_0(const android::sp<WaydroidTask>& task) : mTask(task) {}
    Return<void> setFocusedTask(uint32_t taskID) override { return mTask->setFocusedTask(taskID); }
    Return<void> removeTask(uint32_t taskID) override { return mTask->removeTask(taskID); }
    Return<void> removeAllVisibleRecentTasks() override { return mTask->removeAllVisibleRecentTasks(); }
    Return<void> getAppName(const hidl_string& packageName, getAppName_cb _hidl_cb) override {
        return mTask->getAppName(packageName, _hidl_cb);
    }
  private:
    android::sp<WaydroidTask> mTask;
};

int main() {
    // the conventional HAL might start binder services
    android::ProcessState::self()->setThreadPoolMaxThreadCount(4);
    android::ProcessState::self()->startThreadPool();
    android::sp<WaydroidTask> service = new WaydroidTask();

    configureRpcThreadpool(4, true);

    status_t status = service->registerAsService();
    if (status != OK) {
        LOG(WARNING) << "Cannot register task@1.1, registering task@1.0.";
        android::sp<vendor::waydroid::task::V1_0::IWaydroidTask> service1_0 = new WaydroidTask1_0(service);
        status = service1_0->registerAsService();
    }
    if (status != OK) {
        LOG(ERROR) << "Cannot register WaydroidTask HAL service.";
        return 1;
    }

    LOG(INFO) << "Waydroid Task HAL service ready.";

    joinRpcThreadpool();

    LOG(ERROR) << "WaydroidTask HAL service failed to join thread pool.";
    return 1;
}

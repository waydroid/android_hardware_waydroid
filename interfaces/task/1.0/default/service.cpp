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

using vendor::waydroid::task::V1_0::IWaydroidTask;
using vendor::waydroid::task::V1_0::implementation::WaydroidTask;

using android::OK;
using android::status_t;

int main() {
    // the conventional HAL might start binder services
    android::ProcessState::self()->setThreadPoolMaxThreadCount(4);
    android::ProcessState::self()->startThreadPool();
    android::sp<IWaydroidTask> service = new WaydroidTask();

    configureRpcThreadpool(4, true);

    status_t status = service->registerAsService();
    if (status != OK) {
        LOG(ERROR) << "Cannot register WaydroidTask HAL service.";
        return 1;
    }

    LOG(INFO) << "Waydroid Task HAL service ready.";

    joinRpcThreadpool();

    LOG(ERROR) << "WaydroidTask HAL service failed to join thread pool.";
    return 1;
}

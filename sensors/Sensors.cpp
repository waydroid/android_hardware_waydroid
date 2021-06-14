/*
 * Copyright (C) 2014, 2017-2018 The  Linux Foundation. All rights reserved.
 * Not a contribution
 * Copyright (C) 2008 The Android Open Source Project
 * Copyright (C) 2018 The LineageOS Project
 * Copyright (C) 2021 Anbox Project
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

#include "Sensors.h"

namespace android {
namespace hardware {
namespace sensors {
namespace V1_0 {
namespace implementation {

Sensors::Sensors() {
}

Return<void> Sensors::getSensorsList(getSensorsList_cb _hidl_cb) {
    hidl_vec<SensorInfo> out;

    _hidl_cb(out);
    return Void();
}

Return<Result> Sensors::setOperationMode(OperationMode) {
    return Result::INVALID_OPERATION;
}

Return<Result> Sensors::activate(
        int32_t, bool) {
    return Result::OK;
}

Return<void> Sensors::poll(int32_t, poll_cb _hidl_cb) {
    hidl_vec<Event> out;
    hidl_vec<SensorInfo> dynamicSensorsAdded;

    _hidl_cb(Result::OK, out, dynamicSensorsAdded);
    return Void();
}

Return<Result> Sensors::batch(int32_t, int64_t, int64_t) {
    return Result::OK;
}

Return<Result> Sensors::flush(int32_t) {
    return Result::OK;
}

Return<Result> Sensors::injectSensorData(const Event&) {
    // HAL does not support
    return Result::INVALID_OPERATION;
}

Return<void> Sensors::registerDirectChannel(
        const SharedMemInfo&, registerDirectChannel_cb _hidl_cb) {
    // HAL does not support
    _hidl_cb(Result::INVALID_OPERATION, -1);
    return Void();
}

Return<Result> Sensors::unregisterDirectChannel(int32_t) {
    // HAL does not support
    return Result::INVALID_OPERATION;
}

Return<void> Sensors::configDirectReport(
        int32_t, int32_t, RateLevel, configDirectReport_cb _hidl_cb) {
    // HAL does not support
    _hidl_cb(Result::INVALID_OPERATION, -1);
    return Void();
}

}  // namespace implementation
}  // namespace V1_0
}  // namespace sensors
}  // namespace hardware
}  // namespace android

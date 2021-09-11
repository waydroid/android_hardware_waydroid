/*
 * Copyright (C) 2020 The LineageOS Project
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

#define LOG_TAG "android.hardware.vibrator@1.0-service.waydroid"

#include <log/log.h>

#include <android-base/stringprintf.h>

#include <hardware/hardware.h>
#include <hardware/vibrator.h>

#include "Vibrator.h"

#include <cinttypes>
#include <cmath>
#include <fstream>
#include <iostream>

namespace android {
namespace hardware {
namespace vibrator {
namespace V1_0 {
namespace implementation {

/*
 * Write value to path and close file.
 */
template <typename T>
static void set(const std::string& path, const T& value) {
    std::ofstream file(path);
    if (file) {
        file << value << std::endl;
    }
}

Vibrator::Vibrator() {}

// Methods from ::android::hardware::vibrator::V1_0::IVibrator follow.
Return<Status> Vibrator::on(uint32_t timeout_ms) {
    set("/sys/devices/virtual/timed_output/vibrator/enable", timeout_ms);
    set("/sys/class/leds/vibrator/state", 1);
    set("/sys/class/leds/vibrator/duration", timeout_ms);
    set("/sys/class/leds/vibrator/activate", 1);

    return Status::OK;
}

Return<Status> Vibrator::off() {
    set("/sys/devices/virtual/timed_output/vibrator/enable", 0);
    set("/sys/class/leds/vibrator/activate", 0);

    return Status::OK;
}

Return<bool> Vibrator::supportsAmplitudeControl() {
    return false;
}

Return<Status> Vibrator::setAmplitude(uint8_t) {
    return Status::UNSUPPORTED_OPERATION;
}

Return<void> Vibrator::perform(Effect, EffectStrength, perform_cb _hidl_cb) {
    _hidl_cb(Status::UNSUPPORTED_OPERATION, 0);
    return Void();
}

}  // namespace implementation
}  // namespace V1_0
}  // namespace vibrator
}  // namespace hardware
}  // namespace android

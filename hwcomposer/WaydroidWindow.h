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

#pragma once

#include <vendor/waydroid/window/1.1/IWaydroidWindow.h>
#include <hidl/MQDescriptor.h>
#include <hidl/Status.h>

#include "wayland-hwc.h"

namespace vendor {
namespace waydroid {
namespace window {
namespace implementation {

using ::android::hardware::hidl_array;
using ::android::hardware::hidl_memory;
using ::android::hardware::hidl_string;
using ::android::hardware::hidl_vec;
using ::android::hardware::Return;
using ::android::hardware::Void;
using ::android::sp;

struct WaydroidWindow : public V1_1::IWaydroidWindow {
  public:
    WaydroidWindow(struct display *display);
    // Methods from ::vendor::waydroid::window::V1_0::IWaydroidWindow follow.
    Return<bool> minimize(const hidl_string& packageName) override;

    // Methods from ::vendor::waydroid::window::V1_1::IWaydroidWindow follow.
    Return<void> setPointerCapture(const hidl_string& packageName, bool enabled) override;
  private:
    struct display *mDisplay;
};

}  // namespace implementation
}  // namespace window
}  // namespace waydroid
}  // namespace vendor

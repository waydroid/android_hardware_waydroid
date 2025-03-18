/*
 * Copyright (C) 2025 The Waydroid Project
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

#include <vendor/waydroid/clipboard/1.0/IWaydroidClipboard.h>
#include <hidl/MQDescriptor.h>
#include <hidl/Status.h>

#include "wayland-hwc.h"

namespace vendor {
namespace waydroid {
namespace clipboard {
namespace implementation {

using ::android::hardware::hidl_array;
using ::android::hardware::hidl_memory;
using ::android::hardware::hidl_string;
using ::android::hardware::hidl_vec;
using ::android::hardware::Return;
using ::android::hardware::Void;
using ::android::sp;

struct WaydroidClipboard : public V1_0::IWaydroidClipboard {
  public:
    WaydroidClipboard(struct display *display);
    // Methods from ::vendor::waydroid::clipboard::V1_0::IWaydroidClipboard follow.
    Return<void> sendClipboardData(const hidl_string& value) override;
    Return<void> getClipboardData(getClipboardData_cb _hidl_cb) override;
  private:
    struct display *mDisplay;
};

}  // namespace implementation
}  // namespace clipboard
}  // namespace waydroid
}  // namespace vendor

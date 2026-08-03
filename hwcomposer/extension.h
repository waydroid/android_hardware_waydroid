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
#ifndef VENDOR_WAYDROID_DISPLAY_V1_3_WAYDROIDDISPLAY_H
#define VENDOR_WAYDROID_DISPLAY_V1_3_WAYDROIDDISPLAY_H

#include <android/hardware/graphics/composer/2.1/IComposer.h>
#include <vendor/waydroid/display/1.3/IWaydroidDisplay.h>
#include <hidl/HidlTransportSupport.h>
#include <hidl/MQDescriptor.h>
#include <hidl/Status.h>

#include "wayland-hwc.h"

namespace vendor {
namespace waydroid {
namespace display {
namespace V1_3 {
namespace implementation {

using ::android::hardware::hidl_handle;
using ::android::hardware::hidl_string;
using ::android::hardware::hidl_vec;
using ::android::hardware::Return;
using ::android::hardware::Void;
using ::android::hardware::graphics::composer::V2_1::Error;
using ::android::sp;
using ::vendor::waydroid::display::V1_3::IWaydroidDisplay;

class WaydroidDisplay : public IWaydroidDisplay {
  public:
    WaydroidDisplay(struct waydroid_hwc_composer_device_1 *pdev);

    // Methods from ::vendor::waydroid::display::V1_0::IWaydroidDisplay follow.
    Return<Error> setLayerName(uint32_t layer, const hidl_string &name) override;
    Return<Error> setLayerHandleInfo(uint32_t layer, uint32_t format, uint32_t stride) override;
    Return<Error> setTargetLayerHandleInfo(uint32_t format, uint32_t stride) override;

    // Methods from ::vendor::waydroid::display::V1_1::IWaydroidDisplay follow.
    Return<Error> setLayerSize(uint32_t layer, uint32_t width, uint32_t height) override;
    Return<Error> setTargetLayerSize(uint32_t width, uint32_t height) override;

    // Methods from ::vendor::waydroid::display::V1_2::IWaydroidDisplay follow.
    Return<Error> setMouseMetadata(uint32_t layer, int32_t style, float hotspotX, float hotspotY) override;

    // Methods from ::vendor::waydroid::display::V1_3::IWaydroidDisplay follow.
    Return<void> postTaskBuffer(uint32_t taskId, uint32_t slot, const hidl_handle &buffer,
                                uint32_t width, uint32_t height, uint32_t stride,
                                int32_t format, const hidl_handle &acquireFence,
                                postTaskBuffer_cb _hidl_cb) override;
    Return<void> updateTaskList(const hidl_vec<uint32_t> &tasks,
                                updateTaskList_cb _hidl_cb) override;

  private:
    struct waydroid_hwc_composer_device_1 *mPdev;
    struct display *mDisplay;
};

}  // namespace implementation
}  // namespace V1_3
}  // namespace display
}  // namespace waydroid
}  // namespace vendor

#endif  // VENDOR_WAYDROID_DISPLAY_V1_3_WAYDROIDDISPLAY_H

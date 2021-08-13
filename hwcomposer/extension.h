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
#ifndef VENDOR_WAYDROID_DISPLAY_V1_0_WAYDROIDDISPLAY_H
#define VENDOR_WAYDROID_DISPLAY_V1_0_WAYDROIDDISPLAY_H

#include <android/hardware/graphics/composer/2.1/IComposer.h>
#include <vendor/waydroid/display/1.0/IWaydroidDisplay.h>
#include <hidl/HidlTransportSupport.h>
#include <hidl/MQDescriptor.h>
#include <hidl/Status.h>

#include "wayland-hwc.h"

namespace vendor {
namespace waydroid {
namespace display {
namespace V1_0 {
namespace implementation {

using ::android::hardware::hidl_string;
using ::android::hardware::Return;
using ::android::hardware::graphics::composer::V2_1::Error;
using ::android::sp;
using ::vendor::waydroid::display::V1_0::IWaydroidDisplay;

class WaydroidDisplay : public IWaydroidDisplay {
  public:
    WaydroidDisplay(struct display *display);

    // Methods from ::vendor::waydroid::display::V1_0::IWaydroidDisplay follow.
    Return<Error> setLayerName(uint32_t layer, const hidl_string &name) override;
    Return<Error> setLayerHandleInfo(uint32_t layer, uint32_t format, uint32_t stride) override;
    Return<Error> setTargetLayerHandleInfo(uint32_t format, uint32_t stride) override;
  private:
    struct display *mDisplay;
};

}  // namespace implementation
}  // namespace V1_0
}  // namespace display
}  // namespace waydroid
}  // namespace vendor

#endif  // VENDOR_WAYDROID_DISPLAY_V1_0_WAYDROIDDISPLAY_H

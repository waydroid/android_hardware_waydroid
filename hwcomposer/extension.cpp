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

#include "extension.h"
#include "hwcomposer.h"

#include <errno.h>

namespace vendor {
namespace waydroid {
namespace display {
namespace V1_3 {
namespace implementation {

WaydroidDisplay::WaydroidDisplay(struct waydroid_hwc_composer_device_1 *pdev)
    : mPdev(pdev), mDisplay(pdev->display)
{
}

// Methods from ::vendor::waydroid::display::V1_0::IWaydroidDisplay follow.
Return<Error> WaydroidDisplay::setLayerName(uint32_t layer, const hidl_string &name) {
    mDisplay->layer_names[layer] = std::string(name);
    return Error::NONE;
}
Return<Error> WaydroidDisplay::setLayerHandleInfo(uint32_t layer, uint32_t format, uint32_t stride) {
    mDisplay->layer_handles_ext[layer] = 
    {
        .format = format,
        .stride = stride
    };
    return Error::NONE;
}
Return<Error> WaydroidDisplay::setTargetLayerHandleInfo(uint32_t format, uint32_t stride) {
    mDisplay->target_layer_handle_ext = 
    {
        .format = format,
        .stride = stride
    };
    return Error::NONE;
}

// Methods from ::vendor::waydroid::display::V1_1::IWaydroidDisplay follow.
Return<Error> WaydroidDisplay::setLayerSize(uint32_t layer, uint32_t width, uint32_t height) {
    mDisplay->layer_handles_ext[layer].width = width;
    mDisplay->layer_handles_ext[layer].height = height;
    return Error::NONE;
}

Return<Error> WaydroidDisplay::setTargetLayerSize(uint32_t width, uint32_t height) {
    mDisplay->target_layer_handle_ext.width = width;
    mDisplay->target_layer_handle_ext.height = height;
    return Error::NONE;
}

// Methods from ::vendor::waydroid::display::V1_2::IWaydroidDisplay follow.
Return<Error> WaydroidDisplay::setMouseMetadata(uint32_t layer, int32_t style, float hotspotX, float hotspotY) {
    (void) layer;
    (void) style;
    // TODO: use style for cursor-shape-v1
    mDisplay->cursor_hotspot = {hotspotX, hotspotY};
    return Error::NONE;
}

// Methods from ::vendor::waydroid::display::V1_3::IWaydroidDisplay follow.
Return<void> WaydroidDisplay::postTaskBuffer(uint32_t taskId, uint32_t slot,
        const hidl_handle &buffer, uint32_t width, uint32_t height, uint32_t stride,
        int32_t format, const hidl_handle &acquireFence, postTaskBuffer_cb _hidl_cb) {
    std::vector<uint32_t> released;
    int fenceFd = -1;
    if (acquireFence.getNativeHandle() && acquireFence->numFds > 0)
        fenceFd = acquireFence->data[0];

    int ret = post_task_buffer(mPdev, taskId, slot, buffer.getNativeHandle(), width,
                               height, stride, format, fenceFd, &released);
    Error error;
    switch (ret) {
        case 0:       error = Error::NONE; break;
        case -EAGAIN: error = Error::BAD_DISPLAY; break;
        case -ENOENT: error = Error::BAD_LAYER; break;
        default:      error = Error::NO_RESOURCES; break;
    }
    hidl_vec<uint32_t> releasedSlots(released);
    _hidl_cb(error, releasedSlots);
    return Void();
}

}  // namespace implementation
}  // namespace V1_3
}  // namespace display
}  // namespace waydroid
}  // namespace vendor

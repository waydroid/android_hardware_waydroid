/*
 * Copyright (C) 2021 The Anbox Project
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

namespace vendor {
namespace anbox {
namespace display {
namespace V1_0 {
namespace implementation {

AnboxDisplay::AnboxDisplay(struct display *display)
    : mDisplay(display)
{
}

// Methods from ::vendor::anbox::display::V1_0::IAnboxDisplay follow.
Return<Error> AnboxDisplay::setLayerName(uint32_t layer, const hidl_string &name) {
    mDisplay->layer_names[layer] = std::string(name);
    return Error::NONE;
}
Return<Error> AnboxDisplay::setLayerHandleInfo(uint32_t layer, uint32_t format, uint32_t stride) {
    mDisplay->layer_handles_ext[layer] = 
    {
        .format = format,
        .stride = stride
    };
    return Error::NONE;
}
Return<Error> AnboxDisplay::setTargetLayerHandleInfo(uint32_t format, uint32_t stride) {
    mDisplay->target_layer_handle_ext = 
    {
        .format = format,
        .stride = stride
    };
    return Error::NONE;
}

}  // namespace implementation
}  // namespace V1_0
}  // namespace display
}  // namespace anbox
}  // namespace vendor

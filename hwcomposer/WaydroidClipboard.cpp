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

#include "WaydroidClipboard.h"

#include <log/log.h>

namespace aidl::vendor::waydroid::clipboard {

WaydroidClipboard::WaydroidClipboard(struct display* display)
    : mDisplay(display)
{
}

::ndk::ScopedAStatus WaydroidClipboard::sendClipboardData(const std::string& in) {
    ALOGE("aleasto: sendClipboardData %s", in.c_str());
    return ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus WaydroidClipboard::getClipboardData(std::string* out) {
    (void)out;
    (void)mDisplay;
    ALOGE("aleasto: getClipboardData");
    return ndk::ScopedAStatus::ok();
}

} // namespace aidl::vendor::waydroid::clipboard

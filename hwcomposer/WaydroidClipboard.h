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

#include <aidl/vendor/waydroid/clipboard/BnClipboard.h>

#include "wayland-hwc.h"

namespace aidl::vendor::waydroid::clipboard {

class WaydroidClipboard : public BnClipboard {
public:
    WaydroidClipboard(struct display* display);
    ::ndk::ScopedAStatus sendClipboardData(const std::string& in) override;
    ::ndk::ScopedAStatus getClipboardData(std::string* out) override;
private:
    struct display *mDisplay;
};

} // aidl::vendor::waydroid::clipboard

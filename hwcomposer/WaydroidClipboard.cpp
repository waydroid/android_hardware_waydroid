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

/* Thank you @emersion:
 * https://emersion.fr/blog/2020/wayland-clipboard-drag-and-drop/
 */

#include "WaydroidClipboard.h"

#include <log/log.h>
#include <unistd.h>
#include <algorithm>
#include <vector>

#include <wayland-client.h>

static const std::vector<const std::string> MIME_TYPES = {
    "text/plain;charset=utf-8",
    "UTF8_STRING",
    "text/plain",
    "TEXT",
    "STRING",
};

static void data_source_handle_send(void *data, struct wl_data_source *, const char *mime_type, int fd) {
    // An application wants to paste the clipboard contents
    const char *text = (const char *)data;
    if (std::find(MIME_TYPES.begin(), MIME_TYPES.end(), mime_type) != MIME_TYPES.end())
        write(fd, text, strlen((char *)text));
    else
        ALOGE("Destination client requested unsupported MIME type: %s", mime_type);
    close(fd);
}

static void data_source_handle_cancelled(void *data, struct wl_data_source *source) {
    // An application has replaced the clipboard contents
    wl_data_source_destroy(source);
    free(data);
}

static void data_source_handle_target(void *, struct wl_data_source *, const char *) {}
static void data_source_handle_dnd_drop_performed(void *, struct wl_data_source *) {}
static void data_source_handle_dnd_finished(void *, struct wl_data_source *) {}
static void data_source_handle_action(void *, struct wl_data_source *, uint32_t) {}

static const struct wl_data_source_listener data_source_listener = {
    .target = data_source_handle_target,
    .send = data_source_handle_send,
    .cancelled = data_source_handle_cancelled,
    .dnd_drop_performed = data_source_handle_dnd_drop_performed,
    .dnd_finished =data_source_handle_dnd_finished,
    .action = data_source_handle_action,
};

static void read_selection(struct display *display, struct wl_data_offer *offer, const std::string &mime_type) {
    int fds[2];
    pipe(fds);
    wl_data_offer_receive(offer, mime_type.c_str(), fds[1]);
    close(fds[1]);

    /* The wl_display_roundtrip is required here because we perform blocking read calls.
     * We need to make sure our request is sent to the compositor, otherwise the source client won’t send any data.
     * The blocking read calls stall the Wayland event loop.
     */
    wl_display_roundtrip(display->display);

    // Read the clipboard contents
    display->clipboard.clear();
    while (true) {
        char buf[1024];
        ssize_t n = read(fds[0], buf, sizeof(buf));
        if (n <= 0)
            break;
        display->clipboard.append(buf, n);
    }

    close(fds[0]);
    wl_data_offer_destroy(offer);
}

static void data_device_handle_selection(void *data, struct wl_data_device *, struct wl_data_offer *offer) {
    // An application has set the clipboard contents
    struct display *display = (struct display *)data;
    if (offer == NULL) {
        display->clipboard.clear();
        display->clipboard_offer_mime_types.clear();
        return;
    }

    std::string mime_type;
    for (auto mime_type_req : MIME_TYPES) {
        if (std::find(display->clipboard_offer_mime_types.begin(), display->clipboard_offer_mime_types.end(), mime_type_req) != display->clipboard_offer_mime_types.end()) {
            mime_type = mime_type_req;
            break;
        }
    }
    display->clipboard_offer_mime_types.clear();

    if (mime_type.empty()) {
        // No supported mime-type
        display->clipboard.clear();
        wl_data_offer_destroy(offer);
        return;
    }

    // TODO: only read if necessary. needs careful threading
    read_selection(display, offer, mime_type);
}

static void data_offer_handle_source_actions(void *, struct wl_data_offer *, uint32_t) {}
static void data_offer_handle_action(void *, struct wl_data_offer *, uint32_t) {}

static void data_offer_handle_offer(void *data, struct wl_data_offer *, const char *mime_type) {
    struct display *display = (struct display *)data;
    display->clipboard_offer_mime_types.push_back(mime_type);
}

static const struct wl_data_offer_listener data_offer_listener = {
    .offer = data_offer_handle_offer,
    .source_actions = data_offer_handle_source_actions,
    .action = data_offer_handle_action,
};

static void data_device_handle_data_offer(void *data, struct wl_data_device *, struct wl_data_offer *offer) {
    if (!offer) {
        return;
    }
    // An application has created a new data source
    struct display *display = (struct display *)data;
    wl_data_offer_add_listener(offer, &data_offer_listener, display);
}

static void data_device_handle_enter(void *, struct wl_data_device *, uint32_t, struct wl_surface *, wl_fixed_t, wl_fixed_t, struct wl_data_offer *) {}
static void data_device_handle_leave(void *, struct wl_data_device *) {}
static void data_device_handle_motion(void *, struct wl_data_device *, uint32_t, wl_fixed_t, wl_fixed_t) {}
static void data_device_handle_drop(void *, struct wl_data_device *) {}

static const struct wl_data_device_listener data_device_listener = {
    .data_offer = data_device_handle_data_offer,
    .enter = data_device_handle_enter,
    .leave = data_device_handle_leave,
    .motion = data_device_handle_motion,
    .drop = data_device_handle_drop,
    .selection = data_device_handle_selection,
};

namespace vendor::waydroid::clipboard::implementation {

WaydroidClipboard::WaydroidClipboard(struct display* display)
    : mDisplay(display)
{
    if (!mDisplay->data_device)
        return;
    wl_data_device_add_listener(mDisplay->data_device, &data_device_listener, mDisplay);
}

// Methods from ::vendor::waydroid::clipboard::V1_0::IWaydroidClipboard follow.
Return<void> WaydroidClipboard::sendClipboardData(const hidl_string& in) {
    mDisplay->clipboard = in;

    if (!mDisplay->data_device)
        return Void();

    struct wl_data_source *source = wl_data_device_manager_create_data_source(mDisplay->data_device_manager);
    wl_data_source_add_listener(source, &data_source_listener, strdup(in.c_str()));
    for (auto mime_type : MIME_TYPES)
        wl_data_source_offer(source, mime_type.c_str());
    wl_data_device_set_selection(mDisplay->data_device, source, mDisplay->keyboard_enter_serial);

    return Void();
}

Return<void> WaydroidClipboard::getClipboardData(getClipboardData_cb out_cb) {
    out_cb(mDisplay->clipboard);
    return Void();
}

}  // namespace vendor::waydroid::clipboard::implementation

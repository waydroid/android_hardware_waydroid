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

Return<void> WaydroidDisplay::postTaskScene(uint32_t taskId, uint32_t displayWidth,
        uint32_t displayHeight, const hidl_vec<IWaydroidDisplay::TaskLayer> &layers,
        const hidl_vec<uint64_t> &forgetBuffers, postTaskScene_cb _hidl_cb) {
    std::vector<scene_post_layer> scene(layers.size());
    for (size_t i = 0; i < layers.size(); i++) {
        const auto &l = layers[i];
        const native_handle_t *fence = l.acquireFence.getNativeHandle();
        scene[i] = {
            .layer_id = l.layerId,
            .buffer_id = l.bufferId,
            .frame = l.frameNumber,
            .handle = l.buffer.getNativeHandle(),
            .width = l.width,
            .height = l.height,
            .stride = l.stride,
            .format = l.format,
            .dst_left = l.dstLeft,
            .dst_top = l.dstTop,
            .dst_right = l.dstRight,
            .dst_bottom = l.dstBottom,
            .src_left = l.srcLeft,
            .src_top = l.srcTop,
            .src_right = l.srcRight,
            .src_bottom = l.srcBottom,
            .transform = l.transform,
            .color = l.color,
            .opaque = l.opaque,
            .acquire_fd = fence && fence->numFds > 0 ? fence->data[0] : -1,
        };
    }
    std::vector<uint64_t> forget(forgetBuffers.begin(), forgetBuffers.end());
    std::vector<int> fences;
    int ret = post_task_scene(mPdev, taskId, displayWidth, displayHeight, scene, forget, &fences);
    Error error;
    switch (ret) {
        case 0:            error = Error::NONE; break;
        case -EAGAIN:      error = Error::BAD_DISPLAY; break;
        case -ENOENT:      error = Error::BAD_LAYER; break;
        case -EINPROGRESS: error = Error::NOT_VALIDATED; break;
        case -ENOKEY:      error = Error::BAD_PARAMETER; break;
        case -ENOTSUP:     error = Error::UNSUPPORTED; break;
        default:           error = Error::NO_RESOURCES; break;
    }
    /* Fences are owned by the handles and closed once the reply is sent. */
    std::vector<native_handle_t *> owned;
    hidl_vec<hidl_handle> releases(fences.size());
    for (size_t i = 0; i < fences.size(); i++) {
        if (fences[i] < 0)
            continue;
        native_handle_t *h = native_handle_create(1, 0);
        h->data[0] = fences[i];
        owned.push_back(h);
        releases[i].setTo(h, false /*shouldOwn*/);
    }
    _hidl_cb(error, releases);
    for (auto *h : owned) {
        native_handle_close(h);
        native_handle_delete(h);
    }
    return Void();
}

Return<void> WaydroidDisplay::updateTaskGroups(const hidl_vec<uint32_t> &tasks,
        const hidl_vec<uint32_t> &cards) {
    if (tasks.size() != cards.size()) return Void();
    auto *display = mPdev->display;
    std::scoped_lock lock(display->windowsMutex);
    if (!display->task_streams_active) return Void();
    const auto previous = display->task_cards;
    // Missing scene roots may be backgrounded; taskRemoved retires them.
    for (size_t i = 0; i < tasks.size(); ++i) {
        const auto tid = std::to_string(tasks[i]);
        if (tasks[i] == cards[i]) display->task_cards.erase(tid);
        else display->task_cards[tid] = std::to_string(cards[i]);
    }
    for (size_t i = 0; i < tasks.size(); ++i) {
        if (tasks[i] == cards[i]) continue;
        const auto tid = std::to_string(tasks[i]);
        const auto card = std::to_string(cards[i]);
        if (!detach_task_conn(display, tasks[i]))
            display->windows.erase(tid);
        auto task = display->tasks.find(tid);
        if (task != display->tasks.end() && task->second.focused) {
            display->card_focus_tasks[card] = tid;
            auto old = previous.find(tid);
            if (old == previous.end() || old->second != card) {
                auto window = display->windows.find(card);
                if (window != display->windows.end())
                    request_window_activation(window->second.get());
                else
                    note_pending_raise(display, card);
            }
        }
    }
    for (auto it = display->card_focus_tasks.begin(); it != display->card_focus_tasks.end();) {
        auto group = display->task_cards.find(it->second);
        if (group == display->task_cards.end() || group->second != it->first)
            it = display->card_focus_tasks.erase(it);
        else
            ++it;
    }
    return Void();
}

Return<void> WaydroidDisplay::updateTaskList(const hidl_vec<uint32_t> &tasks,
        updateTaskList_cb _hidl_cb) {
    std::vector<uint32_t> wanted;
    int ret = update_task_list(mPdev, std::vector<uint32_t>(tasks.begin(), tasks.end()),
                               &wanted);
    _hidl_cb(ret == 0 ? Error::NONE : Error::BAD_DISPLAY, hidl_vec<uint32_t>(wanted));
    return Void();
}

}  // namespace implementation
}  // namespace V1_3
}  // namespace display
}  // namespace waydroid
}  // namespace vendor

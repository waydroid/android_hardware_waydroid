/*
 * Copyright (C) 2026 The Waydroid Project
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

/* Task scenes: a task card shown as a stack of subsurfaces that sample the
 * producers' own gralloc buffers, instead of one buffer SurfaceFlinger
 * rendered for it. The binder side imports buffers and hands out release
 * fences; one worker thread waits for acquire fences, applies the scenes in
 * order and signals release points. */

#undef LOG_TAG
#define LOG_TAG "hwc-scene"

#include "wayland-hwc.h"
#include "hwcomposer.h"
#include "gralloc_handler.h"

#include <cinttypes>
#include <algorithm>
#include <cmath>
#include <deque>
#include <queue>
#include <syscall.h>
#include <sys/mman.h>

#include <libsync/sw_sync.h>
#include <log/log.h>
#include <sync/sync.h>
#include <system/graphics.h>

#include <wayland-client.h>
#include "viewporter-client-protocol.h"

namespace {

constexpr size_t kMaxSceneLayers = 32;
constexpr size_t kMaxSceneBuffers = 96;
constexpr int kAcquireWaitMs = 1000;

/* The compositor may still be drawing the replaced buffer for a frame or two
 * after the commit that replaced it. */
std::chrono::milliseconds release_grace() {
    static const auto grace = std::chrono::milliseconds(
            property_get_int32("persist.waydroid.scene_release_grace_ms", 34));
    return grace;
}

struct supersede {
    std::shared_ptr<scene_timeline> timeline;
    uint32_t point;
};

struct scene_job_layer {
    uint32_t id;
    std::shared_ptr<buffer> buf;
    /* A new buffer (or frame) to attach; otherwise only geometry. */
    bool attach;
    int32_t x, y, w, h;
    float sx, sy, sw, sh;
    uint32_t transform;
    bool opaque;
};

struct scene_job {
    uint32_t task_id;
    uint64_t generation;
    uint32_t width, height;
    /* Integer buffer scale, used only without wp_viewporter. */
    int32_t scale;
    std::vector<scene_job_layer> layers;
    std::vector<int> acquire;
    /* Points whose buffers this scene takes off screen. */
    std::vector<supersede> superseded;
};

struct pending_signal {
    std::chrono::steady_clock::time_point when;
    supersede what;
    bool operator>(const pending_signal &o) const { return when > o.when; }
};

void signal_to(const supersede &s) {
    auto &tl = *s.timeline;
    if (s.point > tl.signaled) {
        sw_sync_timeline_inc(tl.fd, s.point - tl.signaled);
        tl.signaled = s.point;
    }
}

/* Largest solid buffer we fill on the CPU (no viewporter to stretch 1x1). */
constexpr int64_t kMaxSolidPixels = 4096 * 4096;

std::shared_ptr<buffer> create_solid_buffer(wl_conn *conn, uint32_t argb, int32_t w = 1,
                                            int32_t h = 1) {
    if (w < 1 || h < 1 || static_cast<int64_t>(w) * h > kMaxSolidPixels)
        return nullptr;
    const size_t size = static_cast<size_t>(w) * h * 4;
    int fd = syscall(SYS_memfd_create, "scene-solid", 0);
    if (fd < 0)
        return nullptr;
    if (ftruncate(fd, size) < 0) {
        close(fd);
        return nullptr;
    }
    void *data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return nullptr;
    }
    /* wl_shm ARGB8888 is premultiplied. */
    const uint32_t a = argb >> 24;
    const auto pm = [a](uint32_t c) { return (c * a + 127) / 255; };
    const uint32_t pixel =
            a << 24 | pm((argb >> 16) & 0xff) << 16 | pm((argb >> 8) & 0xff) << 8 | pm(argb & 0xff);
    std::fill_n(static_cast<uint32_t *>(data), static_cast<size_t>(w) * h, pixel);
    munmap(data, size);

    struct wl_shm_pool *pool = wl_shm_create_pool(conn->shm, fd, size);
    close(fd);
    auto buf = std::make_shared<buffer>();
    buf->conn = conn;
    buf->wl_buffer = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4,
            a == 0xff ? WL_SHM_FORMAT_XRGB8888 : WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    buf->metadata = {static_cast<uint32_t>(h), static_cast<uint32_t>(w), static_cast<uint32_t>(w),
                     HAL_PIXEL_FORMAT_BGRA_8888};
    return buf;
}

/* Import a producer buffer the way this gralloc reaches the host: the
 * Android handle over android_wlegl (Halium), or its dmabuf. */
std::shared_ptr<buffer> import_scene_buffer(waydroid_hwc_composer_device_1 *pdev, wl_conn *conn,
                                            const scene_post_layer &l) {
    buffer_metadata md {};
    md.width = l.width;
    md.height = l.height;
    md.pixel_stride = l.stride;
    md.format = static_cast<uint32_t>(l.format);

    std::unique_ptr<buffer> buf;
    switch (pdev->display->gtype) {
        case GrallocType::GRALLOC_ANDROID:
            buf = create_android_wl_buffer(conn, md, l.handle);
            break;
        case GrallocType::GRALLOC_GBM:
        case GrallocType::GRALLOC_CROS:
            if (!conn->dmabuf)
                return nullptr;
            buf = pdev->gralloc_handler.create_buffer(conn, md, l.handle);
            break;
        default:
            /* SHM would need a CPU copy of every frame: compose instead. */
            return nullptr;
    }
    if (!buf || !buf->wl_buffer)
        return nullptr;
    /* The handle belongs to the transport; libwayland dup'ed what it needs. */
    buf->handle = nullptr;
    return std::shared_ptr<buffer>(std::move(buf));
}

/* The source rectangle in surface space, after the HWC transform (flips,
 * then a clockwise quarter turn). */
void transform_crop(const scene_post_layer &l, float *x, float *y, float *w, float *h) {
    float left = l.src_left, top = l.src_top, right = l.src_right, bottom = l.src_bottom;
    float bw = l.width, bh = l.height;
    if (l.transform & HWC_TRANSFORM_FLIP_H) {
        std::swap(left, right);
        left = bw - left;
        right = bw - right;
    }
    if (l.transform & HWC_TRANSFORM_FLIP_V) {
        std::swap(top, bottom);
        top = bh - top;
        bottom = bh - bottom;
    }
    if (l.transform & HWC_TRANSFORM_ROT_90) {
        const float nl = bh - bottom, nt = left, nr = bh - top, nb = right;
        left = nl;
        top = nt;
        right = nr;
        bottom = nb;
    }
    *x = left;
    *y = top;
    *w = right - left;
    *h = bottom - top;
}

/* A viewport source outside the buffer is a protocol error that would end
 * the whole connection; compose such a layer instead. */
bool crop_fits(const scene_post_layer &l) {
    float x, y, w, h;
    transform_crop(l, &x, &y, &w, &h);
    const bool rotated = l.transform & HWC_TRANSFORM_ROT_90;
    const float bw = rotated ? l.height : l.width;
    const float bh = rotated ? l.width : l.height;
    constexpr float kSlack = 0.01f;
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(w) && std::isfinite(h) &&
           w >= 1.0f && h >= 1.0f && x >= -kSlack && y >= -kSlack &&
           x + w <= bw + kSlack && y + h <= bh + kSlack;
}

void destroy_wl_region_empty_input(wl_conn *conn, wl_surface *surface) {
    /* Input stays on the content surface (layers[0]): its offset is the
     * only one the input path knows for a task card. */
    struct wl_region *region = wl_compositor_create_region(conn->compositor);
    wl_surface_set_input_region(surface, region);
    wl_region_destroy(region);
}

} // namespace

scene_timeline::~scene_timeline() {
    /* Closing a sw_sync timeline signals its remaining points. */
    if (fd >= 0)
        close(fd);
}

struct scene_worker {
    struct display *display;
    std::mutex mutex;
    std::condition_variable cond;
    std::deque<scene_job> jobs;
    std::priority_queue<pending_signal, std::vector<pending_signal>, std::greater<>> signals;
    std::thread thread;

    explicit scene_worker(struct display *d) : display(d) {
        thread = std::thread([this] { run(); });
        pthread_setname_np(thread.native_handle(), "hwc-scene");
    }

    void push(scene_job &&job) {
        {
            std::scoped_lock lock(mutex);
            jobs.push_back(std::move(job));
        }
        cond.notify_one();
    }

    void release_later(std::vector<supersede> &&what) {
        if (what.empty())
            return;
        const auto when = std::chrono::steady_clock::now() + release_grace();
        {
            std::scoped_lock lock(mutex);
            for (auto &s : what)
                signals.push({when, std::move(s)});
        }
        cond.notify_one();
    }

    void run() {
        std::unique_lock lock(mutex);
        for (;;) {
            const auto now = std::chrono::steady_clock::now();
            while (!signals.empty() && signals.top().when <= now) {
                signal_to(signals.top().what);
                signals.pop();
            }
            if (!jobs.empty()) {
                scene_job job = std::move(jobs.front());
                jobs.pop_front();
                lock.unlock();
                apply(job);
                lock.lock();
                continue;
            }
            if (signals.empty())
                cond.wait(lock);
            else
                cond.wait_until(lock, signals.top().when);
        }
    }

    void apply(scene_job &job) {
        for (int fd : job.acquire) {
            if (sync_wait(fd, kAcquireWaitMs) < 0)
                ALOGW("task %u: acquire fence not done after %d ms", job.task_id, kAcquireWaitMs);
            close(fd);
        }
        job.acquire.clear();
        {
            std::scoped_lock lock(display->windowsMutex);
            apply_locked(job);
        }
        release_later(std::move(job.superseded));
    }

    void apply_locked(scene_job &job) {
        const std::string tid = std::to_string(job.task_id);
        auto wit = display->windows.find(tid);
        auto sit = display->task_streams.find(job.task_id);
        if (wit == display->windows.end() || sit == display->task_streams.end() ||
            sit->second.scene_generation != job.generation)
            return;
        window *w = wit->second.get();
        wl_conn *conn = w->conn;
        if (!conn->proxies_valid || w->layers.empty() || !conn->subcompositor)
            return;
        const bool vp = conn->viewporter != nullptr;
        task_stream &stream = sit->second;

        /* The content surface becomes an opaque backing: it keeps the card's
         * input and fills whatever the scene leaves uncovered. */
        auto &content = w->layers[0];
        /* Without a viewport the backing must be the window's real size. */
        const int32_t bw = vp ? 1 : display->width * job.scale;
        const int32_t bh = vp ? 1 : display->height * job.scale;
        if (!w->scene_shown || !w->scene_backing ||
            w->scene_backing->metadata.width != static_cast<uint32_t>(bw) ||
            w->scene_backing->metadata.height != static_cast<uint32_t>(bh)) {
            w->scene_backing = create_solid_buffer(conn, 0xff000000, bw, bh);
            if (!w->scene_backing)
                return;
            content.attach_buffer(w->scene_backing);
            content.damage_surface(0, 0, INT32_MAX, INT32_MAX);
            if (vp)
                wp_viewport_set_source(content.viewport, wl_fixed_from_int(0), wl_fixed_from_int(0),
                                       wl_fixed_from_int(1), wl_fixed_from_int(1));
            else
                wl_surface_set_buffer_scale(content.surface, job.scale);
        }
        if (!w->scene_shown) {
            w->scene_shown = true;
            if (stream.attached_slot != UINT32_MAX) {
                auto slot = stream.slots.find(stream.attached_slot);
                if (slot != stream.slots.end() && slot->second.busy) {
                    slot->second.busy = false;
                    stream.released.push_back(stream.attached_slot);
                }
                stream.attached_slot = UINT32_MAX;
            }
        }
        if (content.viewport)
            wp_viewport_set_destination(content.viewport, display->width, display->height);
        wl_surface_commit(content.surface);

        const double fx = static_cast<double>(display->width) / job.width;
        const double fy = static_cast<double>(display->height) / job.height;

        std::vector<window::scene_layer> next;
        next.reserve(job.layers.size());
        bool restack = job.layers.size() != w->scene.size();
        for (size_t i = 0; i < job.layers.size(); i++) {
            const auto &jl = job.layers[i];
            auto old = std::find_if(w->scene.begin(), w->scene.end(),
                                    [&](const auto &s) { return s.id == jl.id && s.surface; });
            bool fresh = old == w->scene.end();
            if (fresh) {
                wl_surface *surface = wl_compositor_create_surface(conn->compositor);
                wl_surface_set_user_data(surface, w);
                wl_subsurface *sub =
                        wl_subcompositor_get_subsurface(conn->subcompositor, surface, w->surface);
                wp_viewport *viewport =
                        vp ? wp_viewporter_get_viewport(conn->viewporter, surface) : nullptr;
                destroy_wl_region_empty_input(conn, surface);
                next.emplace_back(window::layer(surface, viewport, sub), jl.id);
                restack = true;
            } else {
                if (static_cast<size_t>(old - w->scene.begin()) != i)
                    restack = true;
                next.push_back(std::move(*old));
            }
            auto &sl = next.back();
            if (jl.attach || fresh) {
                sl.attach_buffer(jl.buf);
                sl.damage_surface(0, 0, INT32_MAX, INT32_MAX);
            }
            sl.set_buffer_transform(hwc_transform_to_buffer_transform(jl.transform));
            /* Round both edges, so neighbouring layers share them exactly. */
            const int32_t x0 = std::lround(jl.x * fx), y0 = std::lround(jl.y * fy);
            const int32_t x1 = std::lround((jl.x + jl.w) * fx), y1 = std::lround((jl.y + jl.h) * fy);
            if (sl.viewport) {
                wp_viewport_set_source(sl.viewport, wl_fixed_from_double(jl.sx),
                                       wl_fixed_from_double(jl.sy), wl_fixed_from_double(jl.sw),
                                       wl_fixed_from_double(jl.sh));
                wp_viewport_set_destination(sl.viewport, std::max(1, x1 - x0),
                                            std::max(1, y1 - y0));
            } else {
                /* post_task_scene only lets uncropped, unscaled layers through. */
                wl_surface_set_buffer_scale(sl.surface, job.scale);
            }
            sl.set_position(x0, y0);
            if (jl.opaque) {
                struct wl_region *region = wl_compositor_create_region(conn->compositor);
                wl_region_add(region, 0, 0, INT32_MAX, INT32_MAX);
                wl_surface_set_opaque_region(sl.surface, region);
                wl_region_destroy(region);
            } else {
                wl_surface_set_opaque_region(sl.surface, nullptr);
            }
            wl_surface_commit(sl.surface);
        }
        /* Whatever is left in w->scene is no longer shown; destroying it
         * unmaps it with the parent commit below. */
        w->scene.clear();
        if (restack) {
            wl_surface *below = content.surface;
            for (auto &sl : next) {
                wl_subsurface_place_above(sl.subsurface, below);
                below = sl.surface;
            }
        }
        w->scene = std::move(next);
        /* Synchronized subsurfaces: one parent commit shows the scene. */
        wl_surface_commit(w->surface);
        wl_display_flush(conn->display);
    }
};

int post_task_scene(struct waydroid_hwc_composer_device_1 *pdev, uint32_t taskId,
                    uint32_t width, uint32_t height,
                    const std::vector<scene_post_layer> &layers,
                    const std::vector<uint64_t> &forget, std::vector<int> *fences)
{
    struct display *display = pdev->display;
    std::scoped_lock lock(display->windowsMutex);

    if (!display->task_streams_active)
        return -EAGAIN;
    const std::string tid = std::to_string(taskId);
    auto task = display->tasks.find(tid);
    if (task == display->tasks.end() || task->second.closing)
        return -ENOENT;
    if (task->second.appID.empty() ||
        is_blacklisted(pdev, task->second.appID, task->second.component))
        return -ENOENT;
    if (!width || !height || !display->width || !display->height)
        return -EAGAIN;
    if (layers.empty() || layers.size() > kMaxSceneLayers)
        return -ENOSPC;

    auto &stream = display->task_streams[taskId];
    for (uint64_t id : forget)
        stream.scene_buffers.erase(id);

    /* The window is created and configured by postTaskBuffer. */
    auto wit = display->windows.find(tid);
    if (wit == display->windows.end() || !wit->second->configured ||
        wit->second->layers.empty()) {
        if (stream.scene_active)
            leave_task_scene(display, stream, wit == display->windows.end() ? nullptr
                                                                           : wit->second.get());
        return -EINPROGRESS;
    }
    window *w = wit->second.get();
    wl_conn *conn = w->conn;
    if (!conn->proxies_valid || !conn->subcompositor)
        return -ENOTSUP;
    /* Mir 1.8 has no wp_viewporter: then a buffer can only be shown whole, at
     * an integer buffer scale that maps Android pixels 1:1 onto the window. */
    const bool vp = conn->viewporter != nullptr;
    int32_t scale = 1;
    if (!vp) {
        scale = static_cast<int32_t>(std::lround(display->scale));
        const double fx = static_cast<double>(display->width) / width;
        const double fy = static_cast<double>(display->height) / height;
        if (scale < 1 || std::fabs(fx * scale - 1.0) > 0.01 || std::fabs(fy * scale - 1.0) > 0.01)
            return -ENOTSUP;
    }

    /* Validate and import before changing any state. */
    std::vector<std::shared_ptr<buffer>> bufs(layers.size());
    for (size_t i = 0; i < layers.size(); i++) {
        const auto &l = layers[i];
        if (l.dst_right <= l.dst_left || l.dst_bottom <= l.dst_top || l.transform > 7) {
            ALOGW("task %u: layer %u bad dst %d,%d-%d,%d tr %u", taskId, l.layer_id, l.dst_left,
                  l.dst_top, l.dst_right, l.dst_bottom, l.transform);
            return -ENOTSUP;
        }
        if (l.buffer_id && !crop_fits(l)) {
            ALOGW("task %u: layer %u crop %.1f,%.1f-%.1f,%.1f outside %ux%u tr %u", taskId,
                  l.layer_id, l.src_left, l.src_top, l.src_right, l.src_bottom, l.width, l.height,
                  l.transform);
            return -ENOTSUP;
        }
        if (!vp && l.buffer_id) {
            float x, y, cw, ch;
            transform_crop(l, &x, &y, &cw, &ch);
            const bool rotated = l.transform & HWC_TRANSFORM_ROT_90;
            const float bw = rotated ? l.height : l.width;
            const float bh = rotated ? l.width : l.height;
            if (std::fabs(x) > 0.5f || std::fabs(y) > 0.5f || std::fabs(cw - bw) > 0.5f ||
                std::fabs(ch - bh) > 0.5f ||
                std::abs(l.dst_right - l.dst_left - static_cast<int32_t>(bw)) > 1 ||
                std::abs(l.dst_bottom - l.dst_top - static_cast<int32_t>(bh)) > 1)
                return -ENOTSUP; /* cropped or scaled: compose */
        }
        if (!l.buffer_id) {
            const int32_t sw = vp ? 1 : l.dst_right - l.dst_left;
            const int32_t sh = vp ? 1 : l.dst_bottom - l.dst_top;
            auto &solid = stream.solid_buffers[{l.color, sw, sh}];
            if (!solid)
                solid = create_solid_buffer(conn, l.color, sw, sh);
            if (!solid) {
                ALOGW("task %u: cannot create solid buffer", taskId);
                return -ENOTSUP;
            }
            bufs[i] = solid;
            continue;
        }
        auto cached = stream.scene_buffers.find(l.buffer_id);
        if (cached != stream.scene_buffers.end() && cached->second->conn == conn) {
            bufs[i] = cached->second;
            continue;
        }
        if (!l.handle)
            return -ENOKEY;
        if (stream.scene_buffers.size() >= kMaxSceneBuffers)
            return -ENOSPC;
        auto buf = import_scene_buffer(pdev, conn, l);
        if (!buf) {
            ALOGW("task %u: cannot import buffer %" PRIu64 " (format %d)", taskId, l.buffer_id,
                  l.format);
            return -ENOTSUP;
        }
        stream.scene_buffers[l.buffer_id] = buf;
        bufs[i] = std::move(buf);
    }

    if (!display->scene_worker)
        display->scene_worker = new scene_worker(display);

    scene_job job {};
    job.task_id = taskId;
    job.generation = stream.scene_generation;
    job.width = width;
    job.height = height;
    job.scale = scale;
    std::map<uint32_t, scene_track> tracks;
    fences->assign(layers.size(), -1);
    for (size_t i = 0; i < layers.size(); i++) {
        const auto &l = layers[i];
        scene_job_layer jl {};
        jl.id = l.layer_id;
        jl.buf = bufs[i];
        jl.x = l.dst_left;
        jl.y = l.dst_top;
        jl.w = l.dst_right - l.dst_left;
        jl.h = l.dst_bottom - l.dst_top;
        jl.transform = l.buffer_id ? l.transform : 0;
        jl.opaque = l.opaque;
        if (l.buffer_id)
            transform_crop(l, &jl.sx, &jl.sy, &jl.sw, &jl.sh);
        else
            jl.sx = 0, jl.sy = 0, jl.sw = 1, jl.sh = 1;

        auto prev = stream.tracks.find(l.layer_id);
        scene_track track = prev != stream.tracks.end() ? std::move(prev->second) : scene_track {};
        if (prev != stream.tracks.end())
            stream.tracks.erase(prev);
        const bool changed = !track.fence || track.buffer_id != l.buffer_id || track.frame != l.frame;
        if (changed) {
            if (track.point)
                job.superseded.push_back({track.timeline, track.point});
            if (!track.timeline) {
                track.timeline = std::make_shared<scene_timeline>();
                track.timeline->fd = sw_sync_timeline_create();
            }
            track.point = ++track.timeline->created;
            const int fd = sw_sync_fence_create(track.timeline->fd, "waydroid-scene", track.point);
            track.fence = std::shared_ptr<int>(new int(fd), [](int *p) {
                if (*p >= 0)
                    close(*p);
                delete p;
            });
            track.buffer_id = l.buffer_id;
            track.frame = l.frame;
        }
        jl.attach = changed;
        if (l.buffer_id && *track.fence >= 0)
            (*fences)[i] = dup(*track.fence);
        if (l.acquire_fd >= 0 && changed) {
            const int fd = dup(l.acquire_fd);
            if (fd >= 0)
                job.acquire.push_back(fd);
        }
        tracks.emplace(l.layer_id, std::move(track));
        job.layers.push_back(std::move(jl));
    }
    /* Layers that left the scene come off screen with it. */
    for (auto &[id, track] : stream.tracks) {
        if (track.point)
            job.superseded.push_back({track.timeline, track.point});
    }
    stream.tracks = std::move(tracks);
    stream.scene_active = true;

    display->scene_worker->push(std::move(job));
    return 0;
}

void leave_task_scene(struct display *display, task_stream &stream, struct window *w)
{
    stream.scene_generation++;
    stream.scene_active = false;
    std::vector<supersede> all;
    for (auto &[id, track] : stream.tracks) {
        if (track.point)
            all.push_back({track.timeline, track.point});
    }
    stream.tracks.clear();
    if (w) {
        w->scene.clear();
        if (w->scene_shown && !w->layers.empty()) {
            /* Undo what the backing set up on the content surface. */
            if (w->layers[0].viewport)
                wp_viewport_set_source(w->layers[0].viewport, wl_fixed_from_int(-1),
                                       wl_fixed_from_int(-1), wl_fixed_from_int(-1),
                                       wl_fixed_from_int(-1));
            else
                wl_surface_set_buffer_scale(w->layers[0].surface, 1);
        }
        w->scene_shown = false;
        wl_surface_commit(w->surface);
    }
    if (display->scene_worker)
        display->scene_worker->release_later(std::move(all));
}

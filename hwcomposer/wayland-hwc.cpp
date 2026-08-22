/*
 * Copyright © 2011 Benjamin Franzke
 * Copyright © 2010 Intel Corporation
 * Copyright © 2014 Collabora Ltd.
 * Copyright © 2021 Waydroid Project.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "wayland-hwc.h"

#include "hwcomposer.h"
#include "egl-tools.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <getopt.h>
#include <errno.h>
#include <linux/input.h>
#include <linux/memfd.h>
#include <drm_fourcc.h>
#include <system/graphics.h>
#include <syscall.h>
#include <cmath>
#include <algorithm>

#include <libsync/sw_sync.h>
#include <sync/sync.h>
#include <hardware/gralloc.h>
#include <log/log.h>
#include <sys/resource.h>

#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#include <cutils/trace.h>
#include <cutils/properties.h>
#include <sys/system_properties.h>

#include <xkbcommon/xkbcommon.h>
#include <cinttypes>

#include "gralloc_handler.h"

#include <wayland-client.h>
#include <wayland-android-client-protocol.h>
#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "presentation-time-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include "tablet-unstable-v2-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "pointer-gestures-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "idle-inhibit-unstable-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"

using ::android::hardware::hidl_string;

struct buffer;

buffer::~buffer() {
    if (wl_buffer)
        wl_buffer_destroy(wl_buffer);
    if (isShm && shm_data && shm_data != MAP_FAILED)
        munmap(shm_data, size);
}

/* The platform service (WayDroidService) saves each task's WMS snapshot as
 * raw RGBA when the task leaves the foreground -- content captured by
 * Android's SnapshotController at transition start, i.e. before the next app
 * painted over it. Blit it into the SHM card buffer (nearest-neighbour if
 * sizes differ, RGBA -> wl ARGB8888 swizzle). */
static bool load_task_snapshot_file(struct window *window, struct buffer *buf) {
    std::string path = std::string("/data/waydroid_snapshots/") + window->taskID + ".raw";
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;

    struct { uint32_t magic, width, height, row_bytes; } hdr;
    bool ok = read(fd, &hdr, sizeof(hdr)) == (ssize_t)sizeof(hdr)
              && hdr.magic == 0x57444150
              && hdr.width > 0 && hdr.height > 0
              && hdr.width < 16384 && hdr.height < 16384
              && hdr.row_bytes >= hdr.width * 4;
    if (ok) {
        size_t src_size = (size_t)hdr.row_bytes * hdr.height;
        std::vector<uint8_t> src(src_size);
        ok = read(fd, src.data(), src_size) == (ssize_t)src_size;
        if (ok) {
            uint32_t dw = buf->metadata.width, dh = buf->metadata.height;
            for (uint32_t y = 0; y < dh; y++) {
                const uint8_t *srow = src.data() + (size_t)((uint64_t)y * hdr.height / dh) * hdr.row_bytes;
                uint32_t *drow = (uint32_t *)buf->shm_data + (size_t)y * dw;
                for (uint32_t x = 0; x < dw; x++) {
                    uint32_t c = ((const uint32_t *)srow)[(uint64_t)x * hdr.width / dw];
                    drow[x] = (c & 0xFF00FF00) | ((c & 0xFF0000) >> 16) | ((c & 0xFF) << 16);
                }
            }
        }
    }
    close(fd);
    return ok;
}

// Call me from egl_worker_thread only!
void snapshot_inactive_app_window(struct display *display, struct window *window) {
    if (!window->layers[0].surface || !window->last_layer_buffer
        || window->last_layer_buffer->isShm || window->snapshot_buffer) {
        // Need a surface to draw and a non-SHM buffer to make snapshot from
        return;
    }

    ALOGI("Making inactive window snapshot for %s", window->taskID.c_str());

    auto old_buf = window->last_layer_buffer;

    window->snapshot_buffer = create_shm_wl_buffer(window->conn, old_buf->metadata, old_buf->handle);
    if (!window->snapshot_buffer) {
        ALOGE("failed to create a wayland buffer for window snapshot");
        return;
    }

    // FIXME won't work as expected if there are multiple surfaces
    struct wl_surface *surface = window->layers[0].surface;

    if (display->gtype == GrallocType::GRALLOC_ANDROID) {
        /* The EGL readback aborts in mapper.pixel here, so prefer the WMS
         * snapshot file: correct pre-switch content, no gralloc access. The
         * platform service writes it moments after the task defocuses, so
         * wait a bounded number of frames for it before falling back to the
         * CPU readback (refused on grallocs with GPU-only buffers, then the
         * card keeps its last live buffer as before). */
        bool from_file = load_task_snapshot_file(window, window->snapshot_buffer.get());
        if (!from_file && ++window->snapshot_file_attempts < 180) {
            window->snapshot_buffer = nullptr;
            return;
        }
        if (!from_file && !cpu_render_to_pixels(window->snapshot_buffer.get())) {
            window->snapshot_buffer = nullptr;
            window->snapshot_unavailable = true;
            return;
        }
        ALOGI("Frozen card %s#%s from %s", window->appID.c_str(), window->taskID.c_str(),
              from_file ? "WMS snapshot" : "CPU readback");
    } else {
        egl_render_to_pixels(display, window->snapshot_buffer.get());
    }

    wl_surface_attach(surface, window->snapshot_buffer->wl_buffer, 0, 0);
    wl_surface_damage(surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(surface);

    window->last_layer_buffer = nullptr;
}

/* Attach a task-stream buffer that was stashed while the window could not
 * take it (initial configure not acked yet, or card deactivated). Runs on the
 * wayland thread. */
static void
attach_pending_stream_buffer(struct window *window)
{
    struct display *display = window->conn->dpy;
    std::shared_ptr<buffer> pending;
    {
        std::scoped_lock lock(display->windowsMutex);
        pending = std::move(window->pending_stream_buffer);
        window->pending_stream_buffer = nullptr;
    }
    if (pending && !window->layers.empty()) {
        auto &layer = window->layers[0];
        layer.attach_buffer(pending);
        layer.damage_surface(0, 0, INT32_MAX, INT32_MAX);
        if (layer.viewport)
            wp_viewport_set_destination(layer.viewport, display->width, display->height);
        wl_surface_commit(layer.surface);
        wl_display_flush(display->ctl->display);
    }
}

static void
xdg_surface_handle_configure(void *data, struct xdg_surface *surface,
                 uint32_t serial)
{
    auto window = static_cast<struct window *>(data);

    xdg_surface_ack_configure(surface, serial);

    window->configured = true;

    /* First task-stream buffer posted before this configure: attach it now
     * (post_task_buffer must not wait for the configure, see there). */
    attach_pending_stream_buffer(window);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    xdg_surface_handle_configure,
};

void choose_width_height(struct display* display, int32_t hint_width, int32_t hint_height) {
    char property[PROPERTY_VALUE_MAX];
    int width = hint_width;
    int height = hint_height;

    // Ignore hint it requested
    if (property_get("persist.waydroid.width", property, nullptr) > 0) {
        display->isMaximized = false;
        width = atoi(property);
    }

    if (property_get("persist.waydroid.height", property, nullptr) > 0) {
        display->isMaximized = false;
        height = atoi(property);
    }

    display->width = width;
    display->height = height;
}

void
finished_calibrating(struct display *d)
{
    char property[PROPERTY_VALUE_MAX];
    int default_density = 180;
    std::string display_scale = std::to_string(d->scale);
    property_set("waydroid.display_scale", display_scale.c_str());
    if (property_get("ro.sf.lcd_density", property, nullptr) <= 0) {
        std::string lcd_density = std::to_string(int(default_density * d->scale));
        property_set("ro.sf.lcd_density", lcd_density.c_str());
    }

    choose_width_height(d, d->req_width, d->req_height);
}

/* A uinput FIFO is one Android input device for the whole HAL, not a property
 * of the connection whose seat happened to announce the capability: EventHub
 * registers a device for every node that exists, and a second connection must
 * neither disturb the fd another one is writing to nor unlink the node.
 * mkfifo on an existing path is a harmless EEXIST. */
void
ensure_input_pipe(int input_type)
{
    mkfifo(INPUT_PIPE_NAME[input_type], S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    chown(INPUT_PIPE_NAME[input_type], 1000, 1000);
}

/* Recreate the node so InputFlinger reopens it -- that is how a display
 * geometry change reaches the touchscreen. */
void
reset_input_pipe(struct display *display, int input_type)
{
    display->input_fd[input_type] = -1;
    remove(INPUT_PIPE_NAME[input_type]);
    ensure_input_pipe(input_type);
}

void
init_input_devices(struct display *display)
{
    /* display is value-initialised, so the fds start at 0 -- a valid
     * descriptor as far as ensure_pipe is concerned, which would send every
     * event to stdin. */
    for (int i = 0; i < INPUT_TOTAL; i++)
        display->input_fd[i] = -1;

    display->reverseScroll = property_get_bool("persist.waydroid.reverse_scrolling", false);
    display->scrollSensitivity = property_get_int32("persist.waydroid.scroll_sensitivity", 25);
    display->zoomSensitivity = property_get_int32("persist.waydroid.zoom_sensitivity", 96);
}

void
do_hotplug(struct display *display) {
    if (display->ctl->touch) {
        char property[PROPERTY_VALUE_MAX];
        int width = floor(display->width * display->scale);
        int height = floor(display->height * display->scale);

        if (property_get("persist.waydroid.width_padding", property, nullptr) > 0)
            width -= atoi(property);
        std::string width_str = std::to_string(width);
        property_set("waydroid.display_width", width_str.c_str());

        if (property_get("persist.waydroid.height_padding", property, nullptr) > 0)
            height -= atoi(property);
        std::string height_str = std::to_string(height);
        property_set("waydroid.display_height", height_str.c_str());

        reset_input_pipe(display, INPUT_TOUCH);
    }
    if (display->procs && display->procs->invalidate) {
        display->needHotplug = true;
        display->procs->invalidate(display->procs);
    }
}

/* Grace before concluding "no Waydroid toplevel is engaged" and dozing.
 * Deliberately lazy: brief focus loss (a drawer peek, an A->B switch, reading
 * a notification) must not pause apps. Closing all cards and host visibility
 * loss doze through the immediate visibility path instead; this timer is only
 * insurance for host states that emit no visibility signal. Tunable via
 * persist.waydroid.unfocused_doze_ms; 0 disables the unfocused doze. */
static constexpr int32_t kUnfocusedDozeDefaultMs = 10000;

static std::chrono::milliseconds
unfocused_doze_grace()
{
    return std::chrono::milliseconds(property_get_int32(
        "persist.waydroid.unfocused_doze_ms", kUnfocusedDozeDefaultMs));
}

/* Repeat asserts from a stream of touches are pointless; one per gesture is
 * plenty and setFocusedTask is a binder round trip. */
static constexpr auto kFocusAssertDebounce = std::chrono::milliseconds(500);

/* Where a layer surface sits in the Android display, for translating
 * surface-local input. Only the layer-driven modes record offsets; a task
 * stream is a full-display canvas on its own toplevel, so {0,0} is its real
 * offset and not a fallback. Never insert: this runs on the wayland thread
 * while the compose thread writes the map. */
static struct layerFrame
layer_offset(struct wl_conn *conn, struct wl_surface *surface)
{
    auto it = conn->layers.find(surface);
    return it == conn->layers.end() ? layerFrame{0, 0} : it->second;
}

/* Input lands on whichever surface the compositor picked: a toplevel or one of
 * its layer subsurfaces. Both carry their window as user data, but a freed
 * proxy's memory can be handed back out by another connection's next
 * wl_compositor_create_surface, so confirm the window belongs to this
 * connection before trusting it. Caller must hold windowsMutex. */
static struct window *
window_for_surface(struct wl_conn *conn, struct wl_surface *surface)
{
    if (!surface)
        return nullptr;

    auto *w = reinterpret_cast<struct window *>(wl_surface_get_user_data(surface));
    if (w && w->conn == conn)
        return w;

    for (auto const &[key, window] : conn->dpy->windows) {
        (void)key;
        if (window->conn != conn)
            continue;
        if (window->surface == surface)
            return window.get();
        for (auto const &layer : window->layers) {
            if (layer.surface == surface)
                return window.get();
        }
    }
    return nullptr;
}

/* Android can background a task by itself -- back on the root activity, an
 * in-app exit -- while the host keeps showing and focusing its card. Focus
 * edges say nothing then: the host focus never changed, so no ACTIVATED edge
 * arrives and the card stays frozen with no way back. Reconcile on real
 * engagement instead, comparing against the focus the @1.3 control plane
 * reports and acting only when the two disagree. Engagement is the guard that
 * keeps this from fighting an intentional back press, which arrives with no
 * host input at all. */
static void
reassert_task_focus(struct wl_conn *conn, struct wl_surface *surface, const char *reason)
{
    struct display *display = conn->dpy;
    if (display->task == nullptr || !surface)
        return;

    /* Only from this connection's wayland thread. window::create pumps
     * configure events from hwc_set with windowsMutex held, and a binder call
     * under that lock deadlocks against the adapter mutex. */
    if (!pthread_equal(pthread_self(), conn->wayland_thread))
        return;

    int task_id;
    std::string appID;
    {
        /* Resolve and read under one lock: hwc_set destroys windows on another
         * thread, so a window pointer must not outlive the lock. */
        std::scoped_lock lock(display->windowsMutex);
        struct window *window = window_for_surface(conn, surface);
        if (!window)
            return;

        const std::string &tid = window->taskID;
        if (tid == "none" || tid == "0")
            return;

        auto it = display->tasks.find(tid);
        if (it != display->tasks.end()) {
            if (it->second.focused || it->second.closing)
                return;
        } else if (display->task_events_seen) {
            // The control plane is live and knows no such task: nothing to raise.
            return;
        }

        auto now = std::chrono::steady_clock::now();
        if (now - window->last_focus_assert < kFocusAssertDebounce)
            return;
        window->last_focus_assert = now;

        task_id = stoi(tid);
        appID = window->appID;
    }

    /* Off the mutex: setFocusedTask is a binder call, and hwc_set waits for
     * windowsMutex while holding the adapter lock. */
    ALOGI("%s on %s#%d while Android focus is elsewhere -> setFocusedTask",
          reason, appID.c_str(), task_id);
    force_screen_wakeup(display);
    display->task->setFocusedTask(task_id);
}

/* (Re)start the grace timer; deactivate_worker re-checks state once it
 * expires. Safe to call repeatedly -- each call just pushes the deadline. */
static void
arm_deactivate_check(struct display *display)
{
    auto grace = unfocused_doze_grace();
    if (grace <= std::chrono::milliseconds::zero())
        return;
    std::scoped_lock lock(display->deactivate_mutex);
    display->deactivate_deadline = std::chrono::steady_clock::now() + grace;
    display->deactivate_armed = true;
    display->deactivate_cond.notify_one();
}

static void
xdg_toplevel_handle_configure(void *data, struct xdg_toplevel *,
                              int32_t width, int32_t height,
                              struct wl_array *states)
{
    struct window *window = (struct window *)data;
    struct display *display = window->conn->dpy;

    bool is_activated = false;
#ifdef XDG_TOPLEVEL_STATE_SUSPENDED
    bool is_suspended = false;
#endif
    if (states) {
        const uint32_t *state = static_cast<const uint32_t *>(states->data);
        const size_t count = states->size / sizeof(uint32_t);
        for (size_t i = 0; i < count; i++) {
            if (state[i] == XDG_TOPLEVEL_STATE_ACTIVATED)
                is_activated = true;
#ifdef XDG_TOPLEVEL_STATE_SUSPENDED
            else if (state[i] == XDG_TOPLEVEL_STATE_SUSPENDED)
                is_suspended = true;
#endif
        }
    }

    /* Rising edge of ACTIVATED: the host compositor (Lomiri) just gave this
     * toplevel focus, e.g. the user tapped its entry in the staged switcher.
     * Bring the matching Android task to the front so SurfaceFlinger starts
     * sending its layers again (it currently shows a snapshot). setFocusedTask()
     * moves the task to top and resumes it. taskID "0"/"none" is the launcher /
     * full-ui surface, which has no app task to focus. */
    /* The screen_on belief can go stale (Android dozing on its own, a key
     * injection lost while the input pipe was down); then set_screen_state
     * becomes a no-op and Android never wakes again. KEY_WAKEUP is not a
     * toggle, so on real engagement force one regardless of belief — and do
     * it BEFORE setFocusedTask so the resume isn't swallowed by sleep. */
    if (is_activated && !window->activated)
        force_screen_wakeup(display);

    if (is_activated && !window->activated && display->task != nullptr
        && window->taskID != "none" && window->taskID != "0") {
        ALOGI("ACTIVATED edge for %s#%s -> setFocusedTask",
              window->appID.c_str(), window->taskID.c_str());
        display->task->setFocusedTask(stoi(window->taskID));
    } else if (is_activated) {
        /* Already activated, so no edge: the host re-configured a card it
         * still considers focused. Android may have moved on since. */
        reassert_task_focus(window->conn, window->surface, "activated configure");
    }

    /* Any activation edge re-evaluates screen power: a rising edge wakes
     * Android immediately (the resume needs the display awake to produce a
     * frame); a falling edge only arms the debounced doze via
     * update_screen_power, never acts synchronously -- during an A->B switch
     * B's ACTIVATED can arrive right after A's deactivation, and enter/leave
     * flapping is known. */
    bool activation_edge = is_activated != window->activated;

    window->activated = is_activated;

    if (activation_edge)
        update_screen_power(display);

    /* Reactivated card: show the freshest frame that was withheld while it
     * was deactivated (post_task_buffer stashes instead of attaching then).
     * Not before the initial configure is acked — the surface handler covers
     * that case. */
    if (is_activated && activation_edge && window->configured)
        attach_pending_stream_buffer(window);

#ifdef XDG_TOPLEVEL_STATE_SUSPENDED
    /* suspended (xdg-shell v6+) is the explicit "content not visible" signal,
     * which covers minimize. Feed it into the screen-power decision. Compiled
     * in only when the protocol header is new enough; harmless if the host
     * never sends it. */
    if (window->suspended != is_suspended) {
        window->suspended = is_suspended;
        update_screen_power(display);
    }
#endif

    if (width <= 1 || height <= 1) {
		/* Compositor is deferring to us */
        /* or invalid during calibration */
		return;
	}

    display->req_width = width;
    display->req_height = height;

    if (display->height && display->width) {
        const int32_t old_width = display->width, old_height = display->height;
        choose_width_height(display, width, height);
        if (display->ctl->wm_base)
            xdg_surface_set_window_geometry(window->xdg_surface, 0, 0, display->width, display->height);
        /* Only a real size change warrants a hotplug. Every window creation
         * lands here with the unchanged display size, and hotplugging then is
         * not just wasted work: hwc1Invalidate takes the adapter mutex, which
         * a presenting hwc_set thread holds while waiting for windowsMutex —
         * deadlock if this handler runs on the wayland thread while
         * post_task_buffer (windowsMutex held) waits for wayland events. */
        if (display->width != old_width || display->height != old_height)
            do_hotplug(display);
    }
}

static void
send_key_event(display *data, uint32_t key, wl_keyboard_key_state state);

static void
xdg_toplevel_handle_close(void *data, struct xdg_toplevel *)
{
    struct window *window = (struct window *)data;
    struct display *display = window->conn->dpy;

    ALOGI("compositor closed toplevel task=%s app=%s",
          window->taskID.c_str(), window->appID.c_str());

    // simulate user input to restart idle timeout (TODO: find a better way)
    send_key_event(window->conn->dpy, 0, WL_KEYBOARD_KEY_STATE_PRESSED);
    send_key_event(window->conn->dpy, 0, WL_KEYBOARD_KEY_STATE_RELEASED);

    if (display->task != nullptr) {
        if (window->taskID != "none") {
            if (window->taskID == "0") {
                property_set("waydroid.active_apps", "none");
                display->task->removeAllVisibleRecentTasks();
            } else {
                display->task->removeTask(stoi(window->taskID));
            }
        }
    }

    std::scoped_lock lock(window->conn->dpy->windowsMutex);
    std::string key;
    if (window->taskID != "0" && window->taskID != "none") {
        key = window->taskID;
        /* Two-phase close: the card is gone host-side, but the task entry
         * stays marked closing until WMS confirms with taskRemoved, so
         * teardown frames (or a reused task ID) can't recreate it. */
        auto &task = display->tasks[key];
        if (task.appID.empty())
            task.appID = window->appID;
        task.closing = true;
        task.closing_since = std::chrono::steady_clock::now();
    } else {
        key = window->appID;
    }
    display->windows.erase(key);

    /* The shell may have dropped the app entry backing this connection
     * (qtmir authorizes clients only at connect time), never presenting our
     * windows again. Kill the socket; hwc_set reconnects. Not needed with
     * shells that keep the session presentable. */
    if (display->windows.size() == 0 && display->ctl->wl_alive.load() &&
            property_get_bool("persist.waydroid.reconnect_on_close", true)) {
        ALOGI("last toplevel closed by compositor, forcing wayland reconnect");
        shutdown(wl_display_get_fd(display->ctl->display), SHUT_RDWR);
    }
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    xdg_toplevel_handle_configure,
    xdg_toplevel_handle_close,
};

void
shell_surface_ping(void *, struct wl_shell_surface *shell_surface, uint32_t serial)
{
    wl_shell_surface_pong(shell_surface, serial);
}

void
shell_surface_configure(void *data, struct wl_shell_surface *, uint32_t, int32_t width, int32_t height)
{
    struct window *window = (struct window *)data;
    struct display *display = window->conn->dpy;

    if (width > 1 && height > 1) {
        display->req_width = width;
        display->req_height = height;
	}

    window->configured = true;

    if (display->height && display->width) {
        choose_width_height(display, width, height);
        do_hotplug(display);
    }
}

void
shell_surface_popup_done(void *, struct wl_shell_surface *)
{
}

struct wl_shell_surface_listener shell_surface_listener = {
	&shell_surface_ping,
	&shell_surface_configure,
	&shell_surface_popup_done
};


BufferTransform hwc_transform_to_buffer_transform(uint32_t hwc_transform) {
    switch (hwc_transform) {
        case 0:
            return BufferTransform::Normal;
        case HWC_TRANSFORM_FLIP_H:
            return BufferTransform::Flip_Rot_180;
        case HWC_TRANSFORM_FLIP_V:
            return BufferTransform::Flip;
        case HWC_TRANSFORM_ROT_90:
            return BufferTransform::Rot_90;
        case HWC_TRANSFORM_ROT_180:
            return BufferTransform::Rot_180;
        case HWC_TRANSFORM_ROT_270:
            return BufferTransform::Rot_270;
        case HWC_TRANSFORM_FLIP_H_ROT_90:
            return BufferTransform::Flip_Rot_270;
        case HWC_TRANSFORM_FLIP_V_ROT_90:
            return BufferTransform::Flip_Rot_90;
        default:
            ALOGE("Invalid hwc_transform: %" PRIu32, hwc_transform);
            abort();
    }
}

void surface_context::attach_buffer(std::shared_ptr<buffer> buf) {
    attached_buffer = std::move(buf);
    wl_surface_attach(surface, attached_buffer ? attached_buffer->wl_buffer : nullptr, 0, 0);
}

void surface_context::damage_surface(int32_t x, int32_t y, int32_t width, int32_t height) {
    wl_surface_damage(surface, x, y, width, height);
}

void surface_context::set_buffer_transform(BufferTransform transform) {
    wl_surface_set_buffer_transform(surface, static_cast<int32_t>(transform));
}

void surface_context::set_buffer_scale(double d_scale) {
    assert(!viewport);

    // Usually with no viewporter the scale is guaranteed to be integer
    // When supports_cursor_viewport == false, this might not be the case
    // thus use ceil anyway
    int32_t scale = static_cast<int32_t>(ceil(d_scale));
    wl_surface_set_buffer_scale(surface, scale);
}

void surface_context::set_crop(hwc_frect_t crop) {
    assert(viewport);

    float x = fmax(0, crop.left);
    float y = fmax(0, crop.top);
    float width = fmax(1, crop.right - crop.left);
    float height = fmax(1, crop.bottom - crop.top);

    wp_viewport_set_source(viewport,
                           wl_fixed_from_double(x),
                           wl_fixed_from_double(y),
                           wl_fixed_from_double(width),
                           wl_fixed_from_double(height));
}

void surface_context::set_display_frame(hwc_rect_t rect, double scale) {
    assert(viewport);

    int width = static_cast<int>(ceil((rect.right - rect.left) / scale));
    int height = static_cast<int>(ceil((rect.bottom - rect.top) / scale));

    wp_viewport_set_destination(viewport,
                                std::max(1, width),
                                std::max(1, height));
}

void window::reset_per_set_state() {
    lastLayer = 0;
    last_layer_buffer = nullptr;
    if (input_region) {
        wl_region_subtract(input_region, 0, 0, INT_MAX, INT_MAX);
    }
}

void window::set_maximize(bool enabled) {
    if (xdg_toplevel) {
        if (enabled) {
            xdg_toplevel_set_maximized(xdg_toplevel);
        } else {
            xdg_toplevel_unset_maximized(xdg_toplevel);
        }
    } else {
        assert(shell_surface);
        if (enabled) {
            wl_shell_surface_set_maximized(shell_surface, conn->output);
        } else {
            ALOGW("wl_shell_surface: does not support un-maximizing");
        }
    }
}

void window::set_title(const char* title) {
    if (xdg_toplevel) {
        xdg_toplevel_set_title(xdg_toplevel, title);
    } else {
        assert(shell_surface);
        wl_shell_surface_set_title(shell_surface, title);
    }
}

void window::set_app_id(std::string appID) {
    this->appID = std::move(appID);
    if (xdg_toplevel) {
        const std::string &wayland_app_id = [&](){
            if (this->appID != "Waydroid") {
                return "waydroid." + this->appID;
            } else {
                return this->appID;
            }
        }();
        xdg_toplevel_set_app_id(xdg_toplevel, wayland_app_id.c_str());
    }
}

void window::minimize() {
    if (xdg_toplevel) {
        xdg_toplevel_set_minimized(xdg_toplevel);
        wl_surface_commit(surface); // unclear if this is required
        wl_display_flush(conn->display);
    }
}

window::~window() {
    if (xdg_toplevel)
        xdg_toplevel_destroy(xdg_toplevel);
    if (xdg_surface)
        xdg_surface_destroy(xdg_surface);
    if (shell_surface)
        wl_shell_surface_destroy(shell_surface);
    if (bg_buffer)
        wl_buffer_destroy(bg_buffer);
    if (input_region)
        wl_region_destroy(input_region);
    if (idle_inhibitor)
        zwp_idle_inhibitor_v1_destroy(idle_inhibitor);

    layers.clear();
    if (dedicated_background_surface) {
        if (viewport)
            wp_viewport_destroy(viewport);
        if (surface) {
            wl_surface_set_user_data(surface, nullptr);
            wl_surface_destroy(surface);
        }
        if (locked_pointer)
            zwp_locked_pointer_v1_destroy(locked_pointer);
    }

    wl_display_flush(conn->display);
}

static void fractional_scale_handle_preferred_scale(void *data, struct wp_fractional_scale_v1 *,
            uint32_t scale_times_120) {
    struct wl_conn *conn = (struct wl_conn *)data;
    struct display *display = conn->dpy;
    if (!conn->viewporter) {
        // We should always have the viewporter if we have the fractional scale manager
        // but for debugging purpuses we may decide to disable one
        return;
    }
    display->scale = scale_times_120 / 120.0;
}

static const struct wp_fractional_scale_v1_listener fractional_scale_listener = {
    .preferred_scale = fractional_scale_handle_preferred_scale
};

/* Track which outputs a toplevel is shown on. A compositor that minimizes or
 * fully occludes a surface sends leave for its output(s); when a window is on
 * zero outputs it is off-screen. Drives Android screen power (see
 * update_screen_power). Reliability of leave-on-minimize is compositor
 * dependent; the suspended state (xdg-shell v6+) is the precise signal. */
static void
surface_handle_enter(void *data, struct wl_surface *, struct wl_output *)
{
    auto *window = static_cast<struct window *>(data);
    window->outputs_entered++;
    window->ever_shown = true;
    update_screen_power(window->conn->dpy);
}

static void
surface_handle_leave(void *data, struct wl_surface *, struct wl_output *)
{
    auto *window = static_cast<struct window *>(data);
    if (window->outputs_entered > 0)
        window->outputs_entered--;
    update_screen_power(window->conn->dpy);
}

/* Only enter/leave are wired; preferred_buffer_scale/transform (wl_surface v6)
 * are left unset and never fire because wl_compositor is bound at version <= 5.
 * Designated initializers keep this valid whether libwayland's listener struct
 * has the v1 (2-field) or v6 (4-field) layout. */
static const struct wl_surface_listener surface_listener = {
    .enter = surface_handle_enter,
    .leave = surface_handle_leave,
};

std::unique_ptr<window>
window::create(struct wl_conn *conn, bool use_subsurfaces, std::string appID, std::string taskID, hwc_color_t color, bool sync_configure)
{
    struct display *display = conn->dpy;
    std::unique_ptr<window> window { new struct window() };
    if (!window)
        return nullptr;

    window->conn = conn;
    ALOGI("creating toplevel task=%s app=%s", taskID.c_str(), appID.c_str());
    window->surface = wl_compositor_create_surface(conn->compositor);
    wl_surface_set_user_data(window->surface, window.get());
    wl_surface_add_listener(window->surface, &surface_listener, window.get());
    if (conn->viewporter)
        window->viewport = wp_viewporter_get_viewport(conn->viewporter, window->surface);
    window->taskID = std::move(taskID);
    window->dedicated_background_surface = true;
    window->bg_buffer = nullptr;

    int fd = syscall(SYS_memfd_create, "buffer", 0);
    ftruncate(fd, 4);
    void *shm_data = mmap(NULL, 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm_data == MAP_FAILED) {
        ALOGE("mmap failed");
        close(fd);
        exit(1);
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(conn->shm, fd, 4);
    close(fd);

    // Is this the first window created?
    bool calibrating = !display->height || !display->width;

    if (conn->wm_base) {
        window->xdg_surface =
                xdg_wm_base_get_xdg_surface(conn->wm_base, window->surface);
        assert(window->xdg_surface);
        xdg_surface_add_listener(window->xdg_surface,
                                     &xdg_surface_listener, window.get());

        window->xdg_toplevel = xdg_surface_get_toplevel(window->xdg_surface);
        assert(window->xdg_toplevel);
        xdg_toplevel_add_listener(window->xdg_toplevel, &xdg_toplevel_listener, window.get());
    } else if (conn->shell) {
        window->shell_surface =
            wl_shell_get_shell_surface(conn->shell, window->surface);
        assert(window->shell_surface);
        wl_shell_surface_add_listener(window->shell_surface, &shell_surface_listener, window.get());
        wl_shell_surface_set_toplevel(window->shell_surface);
    } else {
        abort();
    }

    // Set title
    if (appID != "Waydroid" && display->task) {
        const hidl_string appID_hidl(appID);
        display->task->getAppName(appID_hidl, [&](const hidl_string &value){
            window->set_title(value.c_str());
        });
    } else {
        window->set_title(appID.c_str());
    }
    // Set appID
    window->set_app_id(std::move(appID));

    /* Maximize must come after title/app_id: Mir handles set_maximized
     * immediately and creates the scene surface right there, so anything not
     * yet sent is missing from the creation params. qtmir snapshots app_id at
     * surface creation and ignores later changes, leaving the window
     * mislabeled under the generic "Waydroid" app. */
    if (display->isMaximized || calibrating) {
        window->set_maximize(true);
    }

    wl_surface_commit(window->surface);
    if (sync_configure) {
        // Wait for first configure event
        do {
            wl_display_roundtrip(conn->display);
        } while (!window->configured);
    } else {
        wl_display_flush(conn->display);
    }

    if (calibrating) {
        wp_fractional_scale_v1* fs = nullptr;
        if (conn->fractional_scale_manager) {
            // We only support one global scale
            fs = wp_fractional_scale_manager_v1_get_fractional_scale(
                    conn->fractional_scale_manager, window->surface);
            wp_fractional_scale_v1_add_listener(fs, &fractional_scale_listener, conn);
        }

        // Some compositors fail to give us a window size or scale without a buffer attached
        // See: https://github.com/swaywm/sway/issues/2176
        // Some other compositors may refine the window size after a buffer is attached (Hyprland)
        // Try second configure event, with buffer attached
        struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, 1, 1, 4, WL_SHM_FORMAT_ARGB8888);
        wl_surface_attach(window->surface, buf, 0, 0);
        if (window->viewport && display->req_width && display->req_height)
            wp_viewport_set_destination(window->viewport, display->req_width, display->req_height);
        wl_surface_commit(window->surface);
        wl_display_roundtrip(conn->display);

        if (fs) {
            wp_fractional_scale_v1_destroy(fs);
        }

        // If after all of this we still did not receive a proper configure event,
        // fallback to using the full output size.
        // NOTICE: full_width and full_heigth should be in compositor logical size!
        if (!display->req_height)
            display->req_height = display->full_height / display->scale;
        if (!display->req_width)
            display->req_width = display->full_width / display->scale;

        finished_calibrating(display);
        if (!display->isMaximized)
            window->set_maximize(false);
    }

    if (conn->wm_base)
        xdg_surface_set_window_geometry(window->xdg_surface, 0, 0, display->width, display->height);

    struct wl_region *region = wl_compositor_create_region(conn->compositor);
    if (color.a == 0) {
        wl_surface_set_input_region(window->surface, region);
        if (display->system_version >= 33)
            window->input_region = region;
        else
            wl_region_destroy(region);
    }
    if (color.a == 255) {
        wl_region_add(region, 0, 0, display->width, display->height);
        wl_surface_set_opaque_region(window->surface, region);
        wl_region_destroy(region);
    }

    // TODO: Fix background when viewport is not supported
    // No subsurface background for us!
    if (!conn->subcompositor ||
        !conn->viewporter ||
        property_get_bool("persist.waydroid.no_background_subsurface", false)) {
        window->dedicated_background_surface = false;
        window->layers.emplace_back(window->surface, window->viewport);
        wl_shm_pool_destroy(pool);
        wl_surface_commit(window->surface);
        return window;
    } else if (!use_subsurfaces) {
        /* If we do not use subsurfaces for compositing create at least one for the window content
         * This surface should be desync so that we don't need to send commits for the background surface.
         * Since the surface's position will always be (0,0) committing the parent surface is not required then*/
        window->create_new_layer();
        wl_subsurface_set_desync(window->layers[0].subsurface);
    }

    uint32_t *buf = (uint32_t*)shm_data;
    *buf = color.a << 24 | color.r << 16 | color.g << 8 | color.b;
    window->bg_buffer = wl_shm_pool_create_buffer(pool, 0, 1, 1, 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);

    wl_surface_attach(window->surface, window->bg_buffer, 0, 0);
    wl_surface_damage(window->surface, 0, 0, INT32_MAX, INT32_MAX);
    wp_viewport_set_source(window->viewport, wl_fixed_from_int(0), wl_fixed_from_int(0), wl_fixed_from_int(1), wl_fixed_from_int(1));
    wp_viewport_set_destination(window->viewport, display->width, display->height);
    wl_surface_commit(window->surface);

    return window;
}

surface_context::surface_context(wl_surface *surface, wp_viewport *viewport) : surface(surface), viewport(viewport) {}
surface_context::~surface_context() {
    if (viewport)
        wp_viewport_destroy(viewport);
    if (surface)
        wl_surface_destroy(surface);
}
surface_context::surface_context(surface_context&& other) : surface(other.surface), viewport(other.viewport),
        attached_buffer(std::move(other.attached_buffer)) {
    other.surface = nullptr;
    other.viewport = nullptr;
}
surface_context& surface_context::operator=(surface_context&& rhs) {
    if (viewport)
        wp_viewport_destroy(viewport);
    if (surface)
        wl_surface_destroy(surface);

    surface = rhs.surface;
    viewport = rhs.viewport;
    attached_buffer = std::move(rhs.attached_buffer);

    rhs.surface = nullptr;
    rhs.viewport = nullptr;

    return *this;
}

window::layer::layer(wl_surface *surface, wp_viewport *viewport, wl_subsurface *subsurface) : surface_context(surface, viewport), subsurface(subsurface) {}
window::layer::~layer() {
    if (subsurface)
        wl_subsurface_destroy(subsurface);
    if (locked_pointer)
        zwp_locked_pointer_v1_destroy(locked_pointer);
}
window::layer::layer(window::layer&& other) :
        surface_context(std::move(static_cast<surface_context&>(other))),
        subsurface(other.subsurface),
        locked_pointer(other.locked_pointer) {
    other.subsurface = nullptr;
    other.locked_pointer = nullptr;
}
window::layer& window::layer::operator=(window::layer&& rhs) {
    if (subsurface)
        wl_subsurface_destroy(subsurface);
    if (locked_pointer)
        zwp_locked_pointer_v1_destroy(locked_pointer);

    subsurface = rhs.subsurface;
    locked_pointer = rhs.locked_pointer;

    rhs.subsurface = nullptr;
    rhs.locked_pointer = nullptr;

    surface_context::operator=(std::move(rhs));

    return *this;
}

void window::layer::set_position(int32_t x, int32_t y) {
    assert(subsurface || (x == 0 && y == 0));
    if (subsurface) {
        wl_subsurface_set_position(subsurface, x, y);
    }
}

window::layer& window::get_next_layer() {
    if (lastLayer >= layers.size()) {
        assert(lastLayer == layers.size());
        create_new_layer();
    }
    return layers[lastLayer++];
}

window::layer& window::create_new_layer() {
    wl_surface *surface = wl_compositor_create_surface(conn->compositor);
    /* Input on a task card arrives on the content subsurface; tag it so the
     * lookup does not have to scan every window. */
    wl_surface_set_user_data(surface, this);
    wl_subsurface *subsurface = wl_subcompositor_get_subsurface(conn->subcompositor, surface, this->surface);
    wp_viewport *viewport = nullptr;
    if (conn->viewporter)
        viewport = wp_viewporter_get_viewport(conn->viewporter, surface);

    return layers.emplace_back(surface, viewport, subsurface);
}

static int
ensure_pipe(struct display* display, int input_type)
{
    if (display->input_fd[input_type] == -1) {
        /* Two connections' dispatch threads can reach this at once; without
         * the lock one of the two opens leaks. */
        std::scoped_lock lock(display->input_mutex);
        if (display->input_fd[input_type] != -1)
            return 0;
        display->input_fd[input_type] = open(INPUT_PIPE_NAME[input_type], O_WRONLY | O_NONBLOCK);
        if (display->input_fd[input_type] == -1) {
            ALOGE("Failed to open pipe to InputFlinger: %s", strerror(errno));
            return -1;
        }
    }
    return 0;
}

#define ADD_EVENT(type_, code_, value_)            \
    event[n].time.tv_sec = rt.tv_sec;              \
    event[n].time.tv_usec = rt.tv_nsec / 1000;     \
    event[n].type = type_;                         \
    event[n].code = code_;                         \
    event[n].value = value_;                       \
    n++;

static void
send_key_event(display *data, uint32_t key, wl_keyboard_key_state state)
{
    struct display* display = (struct display*)data;
    struct input_event event[1];
    struct timespec rt;
    unsigned int res, n = 0;

    if (key >= display->keysDown.size()) {
        ALOGE("Invalid key: %u", key);
        return;
    }

    if (ensure_pipe(display, INPUT_KEYBOARD))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }
    ADD_EVENT(EV_KEY, key, state);

    res = write(display->input_fd[INPUT_KEYBOARD], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
    display->keysDown[(uint8_t)key] = state;
}

/* How long a freshly-mapped window keeps the Android display awake while it
 * waits to be shown. Cold starts draw in well under a second once the display
 * is awake; the bound only guards against a window that never appears. */
static constexpr auto kNewWindowAwakeGrace = std::chrono::seconds(10);

static bool any_window_visible(struct display *display) {
    auto now = std::chrono::steady_clock::now();
    for (auto const& [key, w] : display->windows) {
        (void)key;
        if (!w)
            continue;
        if (w->outputs_entered > 0 && !w->suspended)
            return true;
        /* A just-launched app hasn't been shown yet, so outputs_entered == 0.
         * If we let the display sleep now it can never draw its first frame (a
         * dozed display pauses rendering), the compositor never shows it, and
         * its card is stranded on the splash. Keep the display awake until it
         * has been shown once, bounded by kNewWindowAwakeGrace. */
        if (!w->ever_shown && now - w->created_at < kNewWindowAwakeGrace)
            return true;
    }
    return false;
}

static void set_screen_state(struct display *display, bool on) {
    if (display->screen_on == on)
        return;
    display->screen_on = on;
    ALOGI("waydroid: host visibility/engagement changed, turning Android screen %s",
          on ? "on" : "off");
    /* Inject KEY_SLEEP/KEY_WAKEUP. Android's PhoneWindowManager routes these
     * through goToSleep/wakeUp, so hidden apps powersave (doze) while their
     * processes stay alive to receive notifications. HAL-internal: uses the
     * existing input pipe, so no binder call and no DEVICE_POWER permission
     * are needed. Unlike KEY_POWER these are not toggles, so a lost injection
     * (input pipe not up yet during early boot) cannot invert the state. */
    uint32_t key = on ? KEY_WAKEUP : KEY_SLEEP;
    send_key_event(display, key, WL_KEYBOARD_KEY_STATE_PRESSED);
    send_key_event(display, key, WL_KEYBOARD_KEY_STATE_RELEASED);
}

/* Belief-independent wake: KEY_WAKEUP is idempotent, so when the user
 * demonstrably engages a window we can always send one, curing any
 * screen_on-vs-reality desync (observed: Android asleep under an awake,
 * card-showing host shell, which no edge could ever fix). */
void force_screen_wakeup(struct display *display) {
    std::scoped_lock lock(display->windowsMutex);
    if (!display->screen_on)
        ALOGI("waydroid: engagement while believed asleep, waking normally");
    else
        ALOGI("waydroid: engagement wake (belief=on), sending KEY_WAKEUP anyway");
    display->screen_on = true;
    send_key_event(display, KEY_WAKEUP, WL_KEYBOARD_KEY_STATE_PRESSED);
    send_key_event(display, KEY_WAKEUP, WL_KEYBOARD_KEY_STATE_RELEASED);
}

/* Whether any window justifies keeping Android resumed: the host has one
 * focused (ACTIVATED), one is too fresh to have been focused yet (mirrors the
 * any_window_visible grace), or Android itself asked to stay awake through
 * a window's idle inhibitor (video playback, PiP). */
static bool any_window_engaged(struct display *display) {
    auto now = std::chrono::steady_clock::now();
    for (auto const& [key, w] : display->windows) {
        (void)key;
        if (!w)
            continue;
        if (w->activated || w->idle_inhibitor)
            return true;
        if (now - w->created_at < kNewWindowAwakeGrace)
            return true;
    }
    return false;
}

/* Recompute whether anything is on-screen and flip Android's screen power to
 * match. Safe to call from either the wayland thread or the compose thread:
 * windowsMutex is recursive, so re-entry during window::create's roundtrip
 * (which runs under the compose thread's lock) is fine.
 *
 * Beyond visibility, engagement gates the screen: windows that are visible
 * but all deactivated (host on its shell/drawer, cards in the spread) must
 * not keep the top task RESUMED forever -- an ATM-side relaunch of an
 * already-top RESUMED task is a no-op that never produces a frame, so the
 * relaunch would hang on the host's splash. Dozing pauses the task in place
 * (no focus/window switch Android-side), and the next ACTIVATED edge wakes
 * and genuinely resumes it. The doze is never taken synchronously, only via
 * deactivate_worker after unfocused_doze_grace(). */
void update_screen_power(struct display *display) {
    std::scoped_lock lock(display->windowsMutex);
    if (!any_window_visible(display)) {
        set_screen_state(display, false);
    } else if (any_window_engaged(display)) {
        set_screen_state(display, true);
    } else if (display->screen_on) {
        /* Visible but nothing engaged: debounce, then doze if it stays so. */
        arm_deactivate_check(display);
    }
}

/* Runs on deactivate_thread once the grace expires: doze only if nothing got
 * (re)engaged meanwhile. Re-checking current state here is what makes A->B
 * switch races and activation flapping cancel the doze naturally. */
static void deactivate_settled_check(struct display *display) {
    std::scoped_lock lock(display->windowsMutex);
    if (!display->screen_on)
        return;
    if (any_window_engaged(display))
        return;
    set_screen_state(display, false);
}

static void
deactivate_worker(struct display *display)
{
    std::unique_lock<std::mutex> lock(display->deactivate_mutex);
    while (!display->deactivate_quit) {
        if (!display->deactivate_armed) {
            display->deactivate_cond.wait(lock);
            continue;
        }
        display->deactivate_cond.wait_until(lock, display->deactivate_deadline);
        if (display->deactivate_quit)
            break;
        /* The deadline may have been pushed forward while we slept. */
        if (std::chrono::steady_clock::now() < display->deactivate_deadline)
            continue;
        display->deactivate_armed = false;
        lock.unlock();
        deactivate_settled_check(display);
        lock.lock();
    }
}

/* wl_buffer.release for a task-stream slot. Userdata is the connection, not
 * the slot: a slot (and its window) can be destroyed while the compositor
 * still holds the buffer, so we look the slot up by wl_buffer instead. */
static void
task_stream_buffer_release(void *data, struct wl_buffer *wl_buf)
{
    struct wl_conn *conn = (struct wl_conn *)data;
    struct display *display = conn->dpy;
    std::scoped_lock lock(display->windowsMutex);
    for (auto &[taskId, stream] : display->task_streams) {
        for (auto &[slot, sb] : stream.slots) {
            if (sb.buf && sb.buf->wl_buffer == wl_buf) {
                sb.busy = false;
                stream.released.push_back(slot);
                return;
            }
        }
    }
}

static const struct wl_buffer_listener task_stream_buffer_listener {
    task_stream_buffer_release
};

int post_task_buffer(struct waydroid_hwc_composer_device_1 *pdev, uint32_t taskId,
                     uint32_t slot, const native_handle_t *handle, uint32_t width,
                     uint32_t height, uint32_t stride, int32_t format, int fenceFd,
                     std::vector<uint32_t> *releasedSlots)
{
    struct display *display = pdev->display;
    std::scoped_lock lock(display->windowsMutex);

    if (!pdev->task_streams_mode_active || !display->ctl->wl_alive)
        return -EAGAIN;

    const std::string tid = std::to_string(taskId);
    auto task = display->tasks.find(tid);
    if (task == display->tasks.end() || task->second.closing)
        return -ENOENT;
    /* The launcher (waydroid.blacklist_apps) never gets a card, and neither
     * do identity-less system tasks (the resync path inserts a few). */
    if (task->second.appID.empty() ||
        is_blacklisted(pdev, task->second.appID, task->second.component))
        return -ENOENT;

    auto &stream = display->task_streams[taskId];
    *releasedSlots = std::move(stream.released);
    stream.released.clear();

    /* A display that has no size yet is still calibrating; window::create
     * would roundtrip, which is forbidden on this thread (see below). */
    if (!display->width || !display->height)
        return -EAGAIN;


    window *w;
    auto it = display->windows.find(tid);
    if (it == display->windows.end()) {
        /* Lazy window creation: a task's toplevel appears with its first
         * content frame. No subsurfaces: layers[0] is the single desync
         * content surface, scaled to the window by its viewport.
         * sync_configure=false: waiting for the configure here (binder
         * thread, windowsMutex held) deadlocks against the configure
         * handler's do_hotplug (adapter mutex, held by hwc_set's thread,
         * which itself waits on windowsMutex). */
        auto win = window::create(display->ctl.get(), false, task->second.appID, tid, {0, 0, 0, 255},
                                  false /*sync_configure*/);
        if (!win)
            return -ENOENT;
        w = win.get();
        display->windows.add(tid, std::move(win));
        update_screen_power(display);
    } else {
        w = it->second.get();
    }
    if (w->layers.empty())
        return -EINVAL;

    auto &sb = stream.slots[slot];
    buffer_metadata md {};
    md.width = width;
    md.height = height;
    md.pixel_stride = stride;
    md.format = static_cast<uint32_t>(format);
    ino_t ino = 0;
    if (handle->numFds > 0) {
        struct stat st;
        if (fstat(handle->data[0], &st) == 0)
            ino = st.st_ino;
    }
    if (sb.buf && (sb.buf->metadata != md || sb.ino != ino)) {
        /* The caller reallocated this slot (SF restart, stream re-creation).
         * attached_buffer keeps the old content alive if it is still
         * committed; a stale busy flag must not survive the old buffer. */
        sb.buf = nullptr;
        sb.busy = false;
    }
    if (sb.busy)
        return -EBUSY;
    if (!sb.buf) {
        auto buf = create_android_wl_buffer(display->ctl.get(), md, handle,
                                            &task_stream_buffer_listener, display->ctl.get());
        if (!buf)
            return -EINVAL;
        /* The handle belongs to the transport; libwayland already dup'ed the
         * fds at marshal time. */
        buf->handle = nullptr;
        sb.buf = std::move(buf);
        sb.ino = ino;
    }

    if (fenceFd >= 0)
        sync_wait(fenceFd, 3000);

    /* A deactivated card (spread open, another card focused) or an unfocused
     * task (guest-side switch) is showing exit/minimize animation frames:
     * freezing one into the card is what produced zoomed/half-drawn cards.
     * Keep the last activated-state frame on screen, but stash this one — the
     * ACTIVATED edge attaches the freshest withheld frame, so reactivation
     * snaps to current content. Only once the card has content: a fresh
     * launch posts before the activation/focus signals land. */
    if (stream.attached_slot != UINT32_MAX &&
        (!w->activated || !task->second.focused)) {
        w->pending_stream_buffer = sb.buf;
        stream.released.push_back(slot);
        return 0;
    }

    if (!w->configured) {
        /* xdg-shell forbids attaching before the initial configure is acked;
         * the configure handler (wayland thread) attaches this buffer. */
        w->pending_stream_buffer = sb.buf;
        wl_display_flush(display->ctl->display);
    } else {
        /* A live attach supersedes any frame stashed while deactivated. */
        w->pending_stream_buffer = nullptr;
        auto &layer = w->layers[0];
        layer.attach_buffer(sb.buf);
        layer.damage_surface(0, 0, INT32_MAX, INT32_MAX);
        if (layer.viewport)
            wp_viewport_set_destination(layer.viewport, display->width, display->height);
        wl_surface_commit(layer.surface);
        wl_display_flush(display->ctl->display);
    }

    sb.busy = true;
    /* This commit supersedes the previously attached slot. Mir never sends
     * wl_buffer.release here, so synthesize it (reported on the next post;
     * with 3 slots the caller always has a free one). */
    if (stream.attached_slot != UINT32_MAX && stream.attached_slot != slot) {
        auto &prev = stream.slots[stream.attached_slot];
        if (prev.busy) {
            prev.busy = false;
            stream.released.push_back(stream.attached_slot);
        }
    }
    stream.attached_slot = slot;
    return 0;
}

int update_task_list(struct waydroid_hwc_composer_device_1 *pdev,
                     const std::vector<uint32_t> &tasks, std::vector<uint32_t> *wanted)
{
    struct display *display = pdev->display;
    std::scoped_lock lock(display->windowsMutex);

    if (!pdev->task_streams_mode_active || !display->ctl->wl_alive)
        return -EAGAIN;

    for (uint32_t taskId : tasks) {
        const std::string tid = std::to_string(taskId);
        auto task = display->tasks.find(tid);
        if (task == display->tasks.end() || task->second.closing)
            continue;
        if (task->second.appID.empty() ||
            is_blacklisted(pdev, task->second.appID, task->second.component))
            continue;
        /* Same condition post_task_buffer stashes on: a card that already
         * has content and is deactivated or unfocused keeps its last frame,
         * so its posts would be withheld anyway. */
        auto stream = display->task_streams.find(taskId);
        if (stream != display->task_streams.end() &&
            stream->second.attached_slot != UINT32_MAX) {
            auto it = display->windows.find(tid);
            if (it != display->windows.end() &&
                (!it->second->activated || !task->second.focused))
                continue;
        }
        wanted->push_back(taskId);
    }
    return 0;
}

static void
keyboard_handle_keymap(void *, struct wl_keyboard *,
               uint32_t format, int fd, uint32_t size)
{
    if (format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        char* keymap_shm = (char*)mmap(NULL, size - 1, PROT_READ, MAP_PRIVATE, fd, 0);
        auto xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_DEFAULT_INCLUDES);
        auto xkb_keymap = xkb_keymap_new_from_buffer(xkb_ctx, keymap_shm, size - 1,
                            XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
        const char* namep = xkb_keymap_layout_get_name(xkb_keymap, 0);
        if (namep) {
            // Try to convert XKB name to an android identifier.
            // This is not very good, for example "English (UK)" becomes "english"
            // but android understand only "english_uk" or "english_us"
            std::string layout_name(namep);
            std::string layout_id = layout_name.substr(0, layout_name.find(' '));
            std::transform(layout_id.begin(), layout_id.end(), layout_id.begin(), ::tolower);
            property_set("waydroid.keyboard_layout", layout_id.c_str());
        }

        xkb_keymap_unref(xkb_keymap);
        xkb_context_unref(xkb_ctx);
        munmap(keymap_shm, size - 1);
    }
    close(fd);
}

static void
keyboard_handle_enter(void *data, struct wl_keyboard *,
                      uint32_t serial, struct wl_surface *surface,
                      struct wl_array *)
{
    if (!surface) {
        // The surface this enter event corresponds to was already destroyed
        return;
    }

    struct wl_conn *conn = (struct wl_conn *)data;
    struct display *display = conn->dpy;
    conn->keyboard_enter_serial = serial;

    std::scoped_lock lock(display->windowsMutex);
    auto window = reinterpret_cast<struct window *>(wl_surface_get_user_data(surface));
    if (!window)
        return; // Window was destroyed in hwc_set

    /* Keyboard focus is engagement too; cure a stale screen_on belief here
     * as well (greeter unlock re-enters without any ACTIVATED edge). Wake
     * before focusing so the resume isn't swallowed by sleep. */
    force_screen_wakeup(display);

    if (display->task != nullptr) {
        if (window->taskID != "none" && window->taskID != "0") {
            ALOGI("keyboard enter on %s#%s -> setFocusedTask",
                  window->appID.c_str(), window->taskID.c_str());
            window->conn->dpy->task->setFocusedTask(stoi(window->taskID));
        }
    }
}

static void
keyboard_handle_leave(void *data, struct wl_keyboard *,
                      uint32_t, struct wl_surface *)
{
    struct wl_conn *conn = (struct wl_conn *)data;
    struct display *display = conn->dpy;

    for (size_t i = 0; i < display->keysDown.size(); i++) {
        if (display->keysDown[i] == WL_KEYBOARD_KEY_STATE_PRESSED) {
            send_key_event(display, i, WL_KEYBOARD_KEY_STATE_RELEASED);
        }
    }
}

static void
keyboard_handle_key(void *data, struct wl_keyboard *,
                    uint32_t, uint32_t, uint32_t key,
                    uint32_t state)
{
    if (key == KEY_POWER)
        return;
    struct wl_conn *conn = (struct wl_conn *)data;
    send_key_event(conn->dpy, key, (enum wl_keyboard_key_state)state);
}

static void
keyboard_handle_modifiers(void *, struct wl_keyboard *, uint32_t, uint32_t,
                          uint32_t, uint32_t, uint32_t)
{
}

static void
keyboard_handle_repeat_info(void *, struct wl_keyboard *,
                            int32_t, int32_t)
{
}

static const struct wl_keyboard_listener keyboard_listener = {
    keyboard_handle_keymap,
    keyboard_handle_enter,
    keyboard_handle_leave,
    keyboard_handle_key,
    keyboard_handle_modifiers,
    keyboard_handle_repeat_info,
};

static void gesture_swipe_begin(struct wl_conn* conn, uint32_t id);
static void gesture_swipe_update(struct wl_conn* conn, uint32_t axis, int value);
static void gesture_swipe_end(struct wl_conn* conn);

static void
pointer_handle_enter(void *data, struct wl_pointer *,
                     uint32_t serial, struct wl_surface *surface,
                     wl_fixed_t, wl_fixed_t)
{
    if (!surface) {
        // The surface this enter event corresponds to was already destroyed
        return;
    }

    struct wl_conn *conn = (struct wl_conn *)data;
    struct display *display = conn->dpy;
    conn->pointer_surface = surface;
    conn->pointer_enter_serial = serial;
    display->cursor_handler->on_cursor_enter(display);
}

static void
pointer_handle_leave(void *data, struct wl_pointer *,
                     uint32_t, struct wl_surface *)
{
    struct wl_conn *conn = (struct wl_conn *)data;
    struct display *display = conn->dpy;
    conn->pointer_surface = NULL;
}

static void
pointer_handle_motion(void *data, struct wl_pointer *,
                      uint32_t, wl_fixed_t sx, wl_fixed_t sy)
{
    struct wl_conn *conn = (struct wl_conn *)data;
    struct display *display = conn->dpy;
    struct input_event event[5];
    struct timespec rt;
    int x, y;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_POINTER))
        return;

    if (!conn->pointer_surface)
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }
    x = wl_fixed_to_int(sx);
    y = wl_fixed_to_int(sy);
    if (display->scale != 1) {
        x = int(x * display->scale);
        y = int(y * display->scale);
    }
    const struct layerFrame off = layer_offset(conn, conn->pointer_surface);
    x += off.x;
    y += off.y;

    ADD_EVENT(EV_ABS, ABS_X, x);
    ADD_EVENT(EV_ABS, ABS_Y, y);
    ADD_EVENT(EV_REL, REL_X, x - display->ptrPrvX);
    ADD_EVENT(EV_REL, REL_Y, y - display->ptrPrvY);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);
    display->ptrPrvX = x;
    display->ptrPrvY = y;

    res = write(display->input_fd[INPUT_POINTER], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

void
handle_relative_motion(void *data, struct zwp_relative_pointer_v1*,
        uint32_t, uint32_t, wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t, wl_fixed_t)
{
    struct wl_conn *conn = (struct wl_conn *)data;
    struct display *display = conn->dpy;
    struct input_event event[3];
    struct timespec rt;
    unsigned int res, n = 0;

    static double acc_x = 0;
    static double acc_y = 0;

    if (ensure_pipe(display, INPUT_POINTER))
        return;

    acc_x += wl_fixed_to_double(dx);
    acc_y += wl_fixed_to_double(dy);

    if (abs(acc_x) < 1 && abs(acc_y) < 1)
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }

    ADD_EVENT(EV_REL, REL_X, (int)acc_x);
    ADD_EVENT(EV_REL, REL_Y, (int)acc_y);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    acc_x -= (int)acc_x;
    acc_y -= (int)acc_y;

    res = write(display->input_fd[INPUT_POINTER], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
pointer_handle_button(void *data, struct wl_pointer *,
                      uint32_t, uint32_t, uint32_t button,
                      uint32_t state)
{
    struct wl_conn *conn = (struct wl_conn *)data;
    struct display *display = conn->dpy;
    struct input_event event[2];
    struct timespec rt;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_POINTER))
        return;

    if (!conn->pointer_surface)
        return;

    if (state == WL_POINTER_BUTTON_STATE_PRESSED)
        reassert_task_focus(conn, conn->pointer_surface, "click");

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }
    ADD_EVENT(EV_KEY, button, state);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_POINTER], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
pointer_handle_axis(void *data, struct wl_pointer *,
                    uint32_t, uint32_t axis, wl_fixed_t value)
{
    struct wl_conn *conn = (struct wl_conn *)data;
    struct display *display = conn->dpy;
    struct input_event event[2];
    struct timespec rt;
    unsigned int res, n = 0;
    int move;
    double fVal = wl_fixed_to_double(value) / 10.0f;
    double step = 1.0f;

    if (ensure_pipe(display, INPUT_POINTER))
        return;

    if (!conn->pointer_surface)
        return;

    if (!display->reverseScroll) {
        fVal = -fVal;
    }

    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        conn->wheelAccumulatorY += fVal;
        if (std::abs(conn->wheelAccumulatorY) < step)
            return;
        move = (int)(conn->wheelAccumulatorY / step);
        conn->wheelAccumulatorY = conn->wheelEvtIsDiscrete ? 0 :
                                     std::fmod(conn->wheelAccumulatorY, step);
    } else {
        conn->wheelAccumulatorX += fVal;
        if (std::abs(conn->wheelAccumulatorX) < step)
            return;
        move = (int)(conn->wheelAccumulatorX / step);
        conn->wheelAccumulatorX = conn->wheelEvtIsDiscrete ? 0 :
                                     std::fmod(conn->wheelAccumulatorY, step);
    }

    if (conn->wheelEvtIsTouchpad) {
        gesture_swipe_update(conn, axis, move);
        return;
    }

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }

    ADD_EVENT(EV_REL, (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) ? REL_WHEEL : REL_HWHEEL, move);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_POINTER], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
pointer_handle_axis_source(void *data, struct wl_pointer *, uint32_t source)
{
    struct wl_conn *conn = (struct wl_conn *)data;
    struct display *display = conn->dpy;
    conn->wheelEvtIsDiscrete = (source == WL_POINTER_AXIS_SOURCE_WHEEL);
    conn->wheelEvtIsTouchpad = (source == WL_POINTER_AXIS_SOURCE_FINGER);

    if (conn->wheelEvtIsTouchpad)
        gesture_swipe_begin(conn, conn->pointer_enter_serial);
}

static void
pointer_handle_axis_stop(void *data, struct wl_pointer *, uint32_t, uint32_t)
{
    struct wl_conn *conn = (struct wl_conn *)data;
    struct display *display = conn->dpy;

    if (conn->wheelEvtIsTouchpad)
        gesture_swipe_end(conn);
}

static void
pointer_handle_axis_discrete(void *, struct wl_pointer *, uint32_t, int32_t)
{
}

static void
pointer_handle_frame(void *, struct wl_pointer *)
{
}

static const struct wl_pointer_listener pointer_listener = {
    pointer_handle_enter,
    pointer_handle_leave,
    pointer_handle_motion,
    pointer_handle_button,
    pointer_handle_axis,
    pointer_handle_frame,
    pointer_handle_axis_source,
    pointer_handle_axis_stop,
    pointer_handle_axis_discrete,
};


/* Slots are a single Android resource, so the array stays global; only the
 * key is per connection. A free slot has no connection. */
static bool
empty_touch_id(const struct wl_conn *conn) {
    struct display *display = conn->dpy;
    std::scoped_lock lock(display->input_mutex);
    for (auto const &slot : display->touch_id) {
        if (slot.conn == conn)
            return false;
    }
    return true;
}

static int
get_touch_id(const struct wl_conn *conn, int id)
{
    struct display *display = conn->dpy;
    std::scoped_lock lock(display->input_mutex);
    for (int i = 0; i < MAX_TOUCHPOINTS; i++) {
        if (display->touch_id[i].conn == conn && display->touch_id[i].id == id)
            return i;
    }
    return -1;
}

static int
create_touch_id(const struct wl_conn *conn, int id) {
    int i = get_touch_id(conn, id);
    if (i != -1)
        return i;

    struct display *display = conn->dpy;
    std::scoped_lock lock(display->input_mutex);
    for (i = 0; i < MAX_TOUCHPOINTS; i++) {
        if (!display->touch_id[i].conn) {
            display->touch_id[i] = { conn, id };
            return i;
        }
    }

    return -1;
}

static int
flush_touch_id(const struct wl_conn *conn, int id)
{
    struct display *display = conn->dpy;
    std::scoped_lock lock(display->input_mutex);
    for (int i = 0; i < MAX_TOUCHPOINTS; i++) {
        if (display->touch_id[i].conn == conn && display->touch_id[i].id == id) {
            display->touch_id[i] = {};
            return i;
        }
    }
    return -1;
}

/* Drop this connection's slots without touching anyone else's. */
static void
clear_touch_ids(const struct wl_conn *conn)
{
    struct display *display = conn->dpy;
    std::scoped_lock lock(display->input_mutex);
    for (auto &slot : display->touch_id) {
        if (slot.conn == conn)
            slot = {};
    }
}

static void
touch_handle_down(void *data, struct wl_touch *,
          uint32_t, uint32_t, struct wl_surface *surface,
          int32_t id, wl_fixed_t x_w, wl_fixed_t y_w)
{
    if (!surface) {
        // The surface this down event corresponds to was already destroyed
        return;
    }

    struct wl_conn *conn = (struct wl_conn *)data;
    struct display *display = conn->dpy;
    struct input_event event[6];
    struct timespec rt;
    int x, y;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TOUCH))
        return;

    /* Before the event, not after: a tap on a card whose task Android has
     * backgrounded must raise that task first, or the touch is delivered to
     * whatever is in front instead. */
    reassert_task_focus(conn, surface, "touch");

    int touch_id = create_touch_id(conn, id);
    conn->touch_surfaces[id] = surface;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
       ALOGE("%s:%d error in touch clock_gettime: %s",
            __FILE__, __LINE__, strerror(errno));
    }
    x = wl_fixed_to_int(x_w);
    y = wl_fixed_to_int(y_w);
    if (display->scale != 1) {
        x = int(x * display->scale);
        y = int(y * display->scale);
    }
    const struct layerFrame off = layer_offset(conn, surface);
    x += off.x;
    y += off.y;

    ADD_EVENT(EV_ABS, ABS_MT_SLOT, touch_id);
    ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, touch_id);
    ADD_EVENT(EV_ABS, ABS_MT_POSITION_X, x);
    ADD_EVENT(EV_ABS, ABS_MT_POSITION_Y, y);
    ADD_EVENT(EV_ABS, ABS_MT_PRESSURE, 50);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_TOUCH], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
touch_handle_up(void *data, struct wl_touch *,
        uint32_t, uint32_t, int32_t id)
{
    struct wl_conn *conn = (struct wl_conn *)data;
    struct display *display = conn->dpy;
    struct input_event event[3];
    struct timespec rt;
    unsigned int res, n = 0;

    int touch_id = flush_touch_id(conn, id);
    // Might not exist if we discarded the down event due to a NULL surface
    if (touch_id != -1) {
        if (ensure_pipe(display, INPUT_TOUCH))
            return;

        if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
            ALOGE("%s:%d error in touch clock_gettime: %s",
                  __FILE__, __LINE__, strerror(errno));
        }
        conn->touch_surfaces[id] = NULL;

        ADD_EVENT(EV_ABS, ABS_MT_SLOT, touch_id);
        ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, -1);
        ADD_EVENT(EV_SYN, SYN_REPORT, 0);

        res = write(display->input_fd[INPUT_TOUCH], &event, sizeof(event));
        if (res < sizeof(event))
            ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
    }
}

static void
touch_handle_motion(void *data, struct wl_touch *,
            uint32_t, int32_t id, wl_fixed_t x_w, wl_fixed_t y_w)
{
    struct wl_conn *conn = (struct wl_conn *)data;
    struct display *display = conn->dpy;
    struct input_event event[6];
    struct timespec rt;
    int x, y;
    unsigned int res, n = 0;

    int touch_id = get_touch_id(conn, id);
    // Might not exist if we discarded the down event due to a NULL surface
    if (touch_id != -1) {
        if (ensure_pipe(display, INPUT_TOUCH))
            return;

        if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
            ALOGE("%s:%d error in touch clock_gettime: %s",
                  __FILE__, __LINE__, strerror(errno));
        }
        x = wl_fixed_to_int(x_w);
        y = wl_fixed_to_int(y_w);
        if (display->scale != 1) {
            x = int(x * display->scale);
            y = int(y * display->scale);
        }
        auto ts = conn->touch_surfaces.find(id);
        const struct layerFrame off =
            layer_offset(conn, ts == conn->touch_surfaces.end() ? nullptr : ts->second);
        x += off.x;
        y += off.y;

        ADD_EVENT(EV_ABS, ABS_MT_SLOT, touch_id);
        ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, touch_id);
        ADD_EVENT(EV_ABS, ABS_MT_POSITION_X, x);
        ADD_EVENT(EV_ABS, ABS_MT_POSITION_Y, y);
        ADD_EVENT(EV_ABS, ABS_MT_PRESSURE, 50);
        ADD_EVENT(EV_SYN, SYN_REPORT, 0);

        res = write(display->input_fd[INPUT_TOUCH], &event, sizeof(event));
        if (res < sizeof(event))
            ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
    }
}

static void
touch_handle_frame(void *, struct wl_touch *)
{
}

static void
touch_handle_cancel(void *data, struct wl_touch *)
{
    struct wl_conn *conn = (struct wl_conn *)data;
    struct display *display = conn->dpy;
    struct input_event event[6];
    struct timespec rt;
    unsigned int res, n;
    int i, id;

    if (ensure_pipe(display, INPUT_TOUCH))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
       ALOGE("%s:%d error in touch clock_gettime: %s",
            __FILE__, __LINE__, strerror(errno));
    }

    // Cancel this connection's touch points; other connections keep theirs.
    for (i = 0; i < MAX_TOUCHPOINTS; i++) {
        if (display->touch_id[i].conn == conn) {
            id = display->touch_id[i].id;
            {
                std::scoped_lock lock(display->input_mutex);
                display->touch_id[i] = {};
            }
            conn->touch_surfaces[id] = NULL;

            n = 0;
            // Turn finger into palm.
            ADD_EVENT(EV_ABS, ABS_MT_SLOT, i);
            ADD_EVENT(EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_PALM);
            ADD_EVENT(EV_SYN, SYN_REPORT, 0);
            // Lift off.
            ADD_EVENT(EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_FINGER);
            ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, -1);
            ADD_EVENT(EV_SYN, SYN_REPORT, 0);

            res = write(display->input_fd[INPUT_TOUCH], &event, sizeof(event));
            if (res < sizeof(event))
                ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
        }
    }
}

static void
touch_handle_shape(void *data, struct wl_touch *, int32_t id, wl_fixed_t major, wl_fixed_t minor)
{
    struct wl_conn *conn = (struct wl_conn *)data;
    struct display *display = conn->dpy;
    struct input_event event[5];
    struct timespec rt;
    unsigned int res, n = 0;

    int touch_id = get_touch_id(conn, id);
    // Might not exist if we discarded the down event due to a NULL surface
    if (touch_id != -1) {
        if (ensure_pipe(display, INPUT_TOUCH))
            return;

        if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
           ALOGE("%s:%d error in touch clock_gettime: %s",
                __FILE__, __LINE__, strerror(errno));
        }
        ADD_EVENT(EV_ABS, ABS_MT_SLOT, touch_id);
        ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, touch_id);
        ADD_EVENT(EV_ABS, ABS_MT_TOUCH_MAJOR, wl_fixed_to_int(major));
        ADD_EVENT(EV_ABS, ABS_MT_TOUCH_MINOR, wl_fixed_to_int(minor));
        ADD_EVENT(EV_SYN, SYN_REPORT, 0);

        res = write(display->input_fd[INPUT_TOUCH], &event, sizeof(event));
        if (res < sizeof(event))
            ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
    }
}

static void
touch_handle_orientation(void *, struct wl_touch *, int32_t, wl_fixed_t)
{
}

static const struct wl_touch_listener touch_listener = {
    touch_handle_down,
    touch_handle_up,
    touch_handle_motion,
    touch_handle_frame,
    touch_handle_cancel,
    touch_handle_shape,
    touch_handle_orientation,
};

static void
gesture_swipe_begin(struct wl_conn* conn, uint32_t id) {
    struct display *display = conn->dpy;
    // Do not interrupt active gestures
    if (!empty_touch_id(conn))
        return;

    display->gesturePoints[0] = create_touch_id(conn, id);
    display->gesturePosX = display->ptrPrvX;
    display->gesturePosY = display->ptrPrvY;
    display->gestureLength = -1;
}

static void
gesture_swipe_update(struct wl_conn* conn, uint32_t axis, int value) {
    struct display *display = conn->dpy;
    struct input_event event[12];
    struct timespec rt;
    unsigned int res, n = 0;

    if (display->gesturePoints[0] == -1)
        return;

    if (ensure_pipe(display, INPUT_TOUCH))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }

    if (display->gestureLength == -1) {
        ADD_EVENT(EV_ABS, ABS_MT_SLOT, display->gesturePoints[0]);
        ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, display->gesturePoints[0]);
        ADD_EVENT(EV_ABS, ABS_MT_POSITION_X, display->gesturePosX);
        ADD_EVENT(EV_ABS, ABS_MT_POSITION_Y, display->gesturePosY);
        ADD_EVENT(EV_ABS, ABS_MT_PRESSURE, 50);
        ADD_EVENT(EV_SYN, SYN_REPORT, 0);
    }

    display->gestureLength = display->scrollSensitivity * display->scale * value;
    if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
        display->gesturePosX += display->gestureLength;
    } else {
        display->gesturePosY += display->gestureLength;
    }

    ADD_EVENT(EV_ABS, ABS_MT_SLOT, display->gesturePoints[0]);
    ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, display->gesturePoints[0]);
    ADD_EVENT(EV_ABS, ABS_MT_POSITION_X, display->gesturePosX);
    ADD_EVENT(EV_ABS, ABS_MT_POSITION_Y, display->gesturePosY);
    ADD_EVENT(EV_ABS, ABS_MT_PRESSURE, 50);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_TOUCH], &event, n * sizeof(input_event));
    if (res < n * sizeof(input_event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
gesture_swipe_end(struct wl_conn* conn) {
    struct display *display = conn->dpy;
    struct input_event event[3];
    struct timespec rt;
    unsigned int res, n = 0;

    if (display->gesturePoints[0] == -1)
        return;

    if (ensure_pipe(display, INPUT_TOUCH))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }

    ADD_EVENT(EV_ABS, ABS_MT_SLOT, display->gesturePoints[0]);
    ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, -1);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    flush_touch_id(conn, display->touch_id[display->gesturePoints[0]].id);
    display->gesturePoints[0] = -1;

    res = write(display->input_fd[INPUT_TOUCH], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
gesture_pinch_begin(void *data, struct zwp_pointer_gesture_pinch_v1 *, uint32_t id, uint32_t, struct wl_surface *, uint32_t)
{
    struct wl_conn *conn = (struct wl_conn *)data;
    struct display *display = conn->dpy;

    // Do not interrupt active gestures
    if (!empty_touch_id(conn))
        return;

    display->gesturePoints[0] = create_touch_id(conn, id);
    display->gesturePoints[1] = create_touch_id(conn, id + 1);
    display->gesturePosX = display->ptrPrvX;
    display->gesturePosY = display->ptrPrvY;
    display->gestureLength = -1;
}

static void
gesture_pinch_update(void *data, struct zwp_pointer_gesture_pinch_v1 *, uint32_t,
            wl_fixed_t, wl_fixed_t, wl_fixed_t scale, wl_fixed_t)
{
    struct wl_conn *conn = (struct wl_conn *)data;
    struct display *display = conn->dpy;
    struct input_event event[11];
    struct timespec rt;
    double zoom_scale = wl_fixed_to_double(scale);
    unsigned int res, n = 0;

    if (display->gesturePoints[0] == -1 || display->gesturePoints[1] == -1)
        return;

    if (ensure_pipe(display, INPUT_TOUCH))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }

    if (display->gestureLength == -1) {
        display->gestureLength = display->zoomSensitivity * display->scale * ((zoom_scale < 1) ? 2 : 1);

        ADD_EVENT(EV_ABS, ABS_MT_SLOT, display->gesturePoints[0]);
        ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, display->gesturePoints[0]);
        ADD_EVENT(EV_ABS, ABS_MT_POSITION_X, display->gesturePosX - (display->gestureLength / 2));
        ADD_EVENT(EV_ABS, ABS_MT_POSITION_Y, display->gesturePosY);
        ADD_EVENT(EV_ABS, ABS_MT_PRESSURE, 50);

        ADD_EVENT(EV_ABS, ABS_MT_SLOT, display->gesturePoints[1]);
        ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, display->gesturePoints[1]);
        ADD_EVENT(EV_ABS, ABS_MT_POSITION_X, display->gesturePosX + (display->gestureLength / 2));
        ADD_EVENT(EV_ABS, ABS_MT_POSITION_Y, display->gesturePosY);
        ADD_EVENT(EV_ABS, ABS_MT_PRESSURE, 50);

        ADD_EVENT(EV_SYN, SYN_REPORT, 0);

        res = write(display->input_fd[INPUT_TOUCH], &event, sizeof(event));
        if (res < sizeof(event))
            ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));

        n = 0;
    }

    ADD_EVENT(EV_ABS, ABS_MT_SLOT, display->gesturePoints[0]);
    ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, display->gesturePoints[0]);
    ADD_EVENT(EV_ABS, ABS_MT_POSITION_X, display->gesturePosX - (display->gestureLength * zoom_scale / 2));
    ADD_EVENT(EV_ABS, ABS_MT_POSITION_Y, display->gesturePosY);
    ADD_EVENT(EV_ABS, ABS_MT_PRESSURE, 50);

    ADD_EVENT(EV_ABS, ABS_MT_SLOT, display->gesturePoints[1]);
    ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, display->gesturePoints[1]);
    ADD_EVENT(EV_ABS, ABS_MT_POSITION_X, display->gesturePosX + (display->gestureLength * zoom_scale / 2));
    ADD_EVENT(EV_ABS, ABS_MT_POSITION_Y, display->gesturePosY);
    ADD_EVENT(EV_ABS, ABS_MT_PRESSURE, 50);

    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_TOUCH], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
gesture_pinch_end(void *data, struct zwp_pointer_gesture_pinch_v1 *, uint32_t, uint32_t, int)
{
    struct wl_conn *conn = (struct wl_conn *)data;
    struct display *display = conn->dpy;
    struct input_event event[6];
    struct timespec rt;
    int touch_id[2];
    unsigned int res, n = 0;

    if (display->gesturePoints[0] == -1 || display->gesturePoints[1] == -1)
        return;

    if (ensure_pipe(display, INPUT_TOUCH))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }

    ADD_EVENT(EV_ABS, ABS_MT_SLOT, display->gesturePoints[0]);
    ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, -1);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    ADD_EVENT(EV_ABS, ABS_MT_SLOT, display->gesturePoints[1]);
    ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, -1);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    flush_touch_id(conn, display->touch_id[display->gesturePoints[0]].id);
    flush_touch_id(conn, display->touch_id[display->gesturePoints[1]].id);
    display->gesturePoints[0] = display->gesturePoints[1] = -1;

    res = write(display->input_fd[INPUT_TOUCH], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static const struct zwp_pointer_gesture_pinch_v1_listener pinch_listener = {
    gesture_pinch_begin,
    gesture_pinch_update,
    gesture_pinch_end,
};

static void
xdg_wm_base_ping(void *, struct xdg_wm_base *wm_base, uint32_t serial)
{
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    xdg_wm_base_ping,
};

static void
seat_handle_capabilities(void *data, struct wl_seat *seat, uint32_t wl_caps)
{
    struct wl_conn *conn = (struct wl_conn *)data;
    struct display *d = conn->dpy;
    enum wl_seat_capability caps = (enum wl_seat_capability) wl_caps;

    /* The FIFOs and the input preferences are set up once in create_display:
     * they are one Android device set, and unlinking a node here would pull
     * it out from under every other connection. */
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !conn->pointer) {
        conn->pointer = wl_seat_get_pointer(seat);
        ensure_input_pipe(INPUT_POINTER);
        d->ptrPrvX = 0;
        d->ptrPrvY = 0;
        d->gesturePoints[0] = d->gesturePoints[1] = -1;
        wl_pointer_add_listener(conn->pointer, &pointer_listener, conn);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && conn->pointer) {
        wl_pointer_destroy(conn->pointer);
        conn->pointer = NULL;
    }

    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !conn->keyboard) {
        conn->keyboard = wl_seat_get_keyboard(seat);
        ensure_input_pipe(INPUT_KEYBOARD);
        wl_keyboard_add_listener(conn->keyboard, &keyboard_listener, conn);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && conn->keyboard) {
        wl_keyboard_destroy(conn->keyboard);
        conn->keyboard = NULL;
    }

    if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !conn->touch) {
        conn->touch = wl_seat_get_touch(seat);
        ensure_input_pipe(INPUT_TOUCH);
        clear_touch_ids(conn);
        wl_touch_set_user_data(conn->touch, conn);
        wl_touch_add_listener(conn->touch, &touch_listener, conn);
    } else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && conn->touch) {
        wl_touch_destroy(conn->touch);
        conn->touch = NULL;
    }

    if (conn->pointer_gestures && conn->pointer) {
        if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && !conn->touch) {
            ensure_input_pipe(INPUT_TOUCH);
            clear_touch_ids(conn);
        }

        conn->pointer_gesture_pinch = zwp_pointer_gestures_v1_get_pinch_gesture(conn->pointer_gestures, conn->pointer);
        zwp_pointer_gesture_pinch_v1_add_listener(conn->pointer_gesture_pinch, &pinch_listener, conn);
    }
}

static void
seat_handle_name(void *, struct wl_seat *, const char *)
{
}

static const struct wl_seat_listener seat_listener = {
    seat_handle_capabilities,
    seat_handle_name,
};

static void dmabuf_format(void *, struct zwp_linux_dmabuf_v1 *, uint32_t);
static void
dmabuf_modifiers(void *data, struct zwp_linux_dmabuf_v1 * dmabuf,
         uint32_t format, uint32_t modifier_hi, uint32_t modifier_lo)
{
    dmabuf_format(data, dmabuf, format);

    struct wl_conn *conn = (struct wl_conn *)data;
    struct display *d = conn->dpy;
    uint64_t modifier = ((uint64_t)modifier_hi << 32) | modifier_lo;
    if (modifier == DRM_FORMAT_MOD_INVALID)
        return;

    std::stringstream prop_name_stream;
    std::stringstream prop_value_stream;
    prop_name_stream << "waydroid.modifiers." << std::hex << format << "." << std::dec << conn->modifiers[format].size();
    prop_value_stream << std::hex << modifier;
    std::string prop_name = prop_name_stream.str();
    std::string prop_value = prop_value_stream.str();
    property_set(prop_name.c_str(), prop_value.c_str());

    conn->modifiers[format].push_back(modifier);
}

static void
dmabuf_format(void *data, struct zwp_linux_dmabuf_v1 *, uint32_t format)
{
    struct wl_conn *conn = (struct wl_conn *)data;
    struct display *d = conn->dpy;
    conn->formats.insert(format);
}

static const struct zwp_linux_dmabuf_v1_listener dmabuf_listener = {
    dmabuf_format,
    dmabuf_modifiers
};

static void
output_handle_mode(void *data, struct wl_output *,
                   uint32_t, int32_t width, int32_t height,
                   int32_t refresh)
{
    struct wl_conn *conn = (struct wl_conn *)data;
    struct display *d = conn->dpy;
    d->refresh = std::max(d->refresh, refresh);

    // Fallback size
    // We can't do anything meaningful if there's more than one display, just pick one at random
    // Hopefully these won't need to be used
    // NOTICE: Actually, this is wrong when using scaling. We should probably use xdg_output instead.
    d->full_width = width;
    d->full_height = height;
}

static void
output_handle_geometry(void *, struct wl_output *,
               int32_t, int32_t,
               int32_t, int32_t,
               int32_t,
               const char *, const char *,
               int32_t)
{
}

static void
output_handle_done(void *, struct wl_output *)
{
}

static void
output_handle_scale(void *data, struct wl_output *,
            int32_t scale)
{
    struct wl_conn *conn = (struct wl_conn *)data;
    struct display *d = conn->dpy;
    d->scale = std::max((int)d->scale, scale);
}

static const struct wl_output_listener output_listener = {
    output_handle_geometry,
    output_handle_mode,
    output_handle_done,
    output_handle_scale,
};

static void
presentation_clock_id(void *, struct wp_presentation *,
              uint32_t clk_id)
{
    ALOGE("*** %s: clk_id %d CLOCK_MONOTONIC %d", __func__, clk_id, CLOCK_MONOTONIC);
}

static const struct wp_presentation_listener presentation_listener = {
    presentation_clock_id
};

static void tablet_seat_handle_add_tablet(void *, struct zwp_tablet_seat_v2 *,
                                          struct zwp_tablet_v2 *)
{
}

static void tablet_seat_handle_add_pad(void*, struct zwp_tablet_seat_v2 *,
                                       struct zwp_tablet_pad_v2 *)
{
}

static void
tablet_tool_receive_type(void *data, struct zwp_tablet_tool_v2 *tool,
                         uint32_t type)
{
    struct wl_conn *conn = (struct wl_conn *)data;
    struct display *display = conn->dpy;
    uint16_t evt_code;
    switch(type) {
        case ZWP_TABLET_TOOL_V2_TYPE_PEN:
            evt_code = BTN_TOOL_PEN;
            break;
        case ZWP_TABLET_TOOL_V2_TYPE_ERASER:
            evt_code = BTN_TOOL_RUBBER;
            break;
        case ZWP_TABLET_TOOL_V2_TYPE_BRUSH:
            evt_code = BTN_TOOL_BRUSH;
            break;
        case ZWP_TABLET_TOOL_V2_TYPE_PENCIL:
            evt_code = BTN_TOOL_PENCIL;
            break;
        case ZWP_TABLET_TOOL_V2_TYPE_AIRBRUSH:
            evt_code = BTN_TOOL_AIRBRUSH;
            break;
        case ZWP_TABLET_TOOL_V2_TYPE_FINGER:
            evt_code = BTN_TOOL_FINGER;
            break;
        case ZWP_TABLET_TOOL_V2_TYPE_MOUSE:
            evt_code = BTN_TOOL_MOUSE;
            break;
        case ZWP_TABLET_TOOL_V2_TYPE_LENS:
            evt_code = BTN_TOOL_LENS;
            break;
        default:
            evt_code = BTN_DIGI;
    }
    conn->tablet_tools_evt[tool] = evt_code;
}

static void
tablet_tool_receive_hardware_serial(void *, struct zwp_tablet_tool_v2 *,
                                    uint32_t, uint32_t)
{
}

static void
tablet_tool_receive_hardware_id_wacom(void *, struct zwp_tablet_tool_v2 *,
                                      uint32_t, uint32_t)
{
}

static void
tablet_tool_receive_capability(void *, struct zwp_tablet_tool_v2 *, uint32_t)
{
}

static void
tablet_tool_receive_done(void *, struct zwp_tablet_tool_v2 *)
{
}

static void
tablet_tool_receive_removed(void *, struct zwp_tablet_tool_v2 *)
{
}

static void
tablet_tool_proximity_in(void *data, struct zwp_tablet_tool_v2 *tool,
                         uint32_t, struct zwp_tablet_v2 *,
                         struct wl_surface *surface)
{
    struct wl_conn *conn = (struct wl_conn *)data;
    struct display *display = conn->dpy;
    struct input_event event[2];
    struct timespec rt;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TABLET))
        return;

    conn->tablet_surface = surface;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }

    ADD_EVENT(EV_KEY, conn->tablet_tools_evt[tool], 1);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_TABLET], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
tablet_tool_proximity_out(void *data, struct zwp_tablet_tool_v2 *tool)
{
    struct wl_conn *conn = (struct wl_conn *)data;
    struct display *display = conn->dpy;
    struct input_event event[2];
    struct timespec rt;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TABLET))
        return;

    conn->tablet_surface = NULL;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }

    ADD_EVENT(EV_KEY, conn->tablet_tools_evt[tool], 0);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_TABLET], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
tablet_tool_down(void *data, struct zwp_tablet_tool_v2 *, uint32_t)
{
    struct wl_conn *conn = (struct wl_conn *)data;
    struct display *display = conn->dpy;
    struct input_event event[2];
    struct timespec rt;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TABLET))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }

    ADD_EVENT(EV_KEY, BTN_TOUCH, 1);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_TABLET], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
tablet_tool_up(void *data, struct zwp_tablet_tool_v2 *)
{
    struct wl_conn *conn = (struct wl_conn *)data;
    struct display *display = conn->dpy;
    struct input_event event[2];
    struct timespec rt;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TABLET))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }

    ADD_EVENT(EV_KEY, BTN_TOUCH, 0);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_TABLET], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
tablet_tool_motion(void *data, struct zwp_tablet_tool_v2 *,
                   wl_fixed_t x_w, wl_fixed_t y_w)
{
    struct wl_conn *conn = (struct wl_conn *)data;
    struct display *display = conn->dpy;
    struct input_event event[3];
    struct timespec rt;
    int x, y;
    unsigned int res, n = 0;

    if (conn->tablet_surface) {
        if (ensure_pipe(display, INPUT_TABLET))
            return;

        if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
            ALOGE("%s:%d error in touch clock_gettime: %s",
                  __FILE__, __LINE__, strerror(errno));
        }
        x = wl_fixed_to_int(x_w);
        y = wl_fixed_to_int(y_w);
        if (display->scale != 1) {
            x = int(x * display->scale);
            y = int(y * display->scale);
        }
        const struct layerFrame off = layer_offset(conn, conn->tablet_surface);
        x += off.x;
        y += off.y;

        ADD_EVENT(EV_ABS, ABS_X, x);
        ADD_EVENT(EV_ABS, ABS_Y, y);
        ADD_EVENT(EV_SYN, SYN_REPORT, 0);

        res = write(display->input_fd[INPUT_TABLET], &event, sizeof(event));
        if (res < sizeof(event))
            ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
    }
}

static void
tablet_tool_pressure(void *data, struct zwp_tablet_tool_v2 *,
                     uint32_t pressure)
{
    struct wl_conn *conn = (struct wl_conn *)data;
    struct display *display = conn->dpy;
    struct input_event event[2];
    struct timespec rt;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TABLET))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }

    // wayland value is 16 bits. android expects 8 bits max.
    pressure >>= 8;

    ADD_EVENT(EV_ABS, ABS_PRESSURE, pressure);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_TABLET], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
tablet_tool_distance(void *data, struct zwp_tablet_tool_v2 *,
                     uint32_t distance_raw)
{
    struct wl_conn *conn = (struct wl_conn *)data;
    struct display *display = conn->dpy;
    struct input_event event[2];
    struct timespec rt;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TABLET))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }

    ADD_EVENT(EV_ABS, ABS_DISTANCE, distance_raw);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_TABLET], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
tablet_tool_tilt(void *data, struct zwp_tablet_tool_v2 *,
                 wl_fixed_t tilt_x, wl_fixed_t tilt_y)
{
    struct wl_conn *conn = (struct wl_conn *)data;
    struct display *display = conn->dpy;
    struct input_event event[3];
    struct timespec rt;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TABLET))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
       ALOGE("%s:%d error in touch clock_gettime: %s",
            __FILE__, __LINE__, strerror(errno));
    }

    ADD_EVENT(EV_ABS, ABS_TILT_X, wl_fixed_to_int(tilt_x));
    ADD_EVENT(EV_ABS, ABS_TILT_Y, wl_fixed_to_int(tilt_y));
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_TABLET], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
tablet_tool_rotation(void *, struct zwp_tablet_tool_v2 *, wl_fixed_t)
{
}

static void
tablet_tool_slider(void *, struct zwp_tablet_tool_v2 *, int32_t)
{
}

static void
tablet_tool_wheel(void *, struct zwp_tablet_tool_v2 *, wl_fixed_t, int32_t)
{
}

static void
tablet_tool_button_state(void *data, struct zwp_tablet_tool_v2 *,
                         uint32_t, uint32_t button, uint32_t state)
{
    struct wl_conn *conn = (struct wl_conn *)data;
    struct display *display = conn->dpy;
    struct input_event event[2];
    struct timespec rt;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TABLET))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }

    ADD_EVENT(EV_KEY, button, state);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_TABLET], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
tablet_tool_frame(void *, struct zwp_tablet_tool_v2 *, uint32_t)
{
}

static const struct zwp_tablet_tool_v2_listener tablet_tool_listener = {
    .type = tablet_tool_receive_type,
    .hardware_serial = tablet_tool_receive_hardware_serial,
    .hardware_id_wacom = tablet_tool_receive_hardware_id_wacom,
    .capability = tablet_tool_receive_capability,
    .done = tablet_tool_receive_done,
    .removed = tablet_tool_receive_removed,
    .proximity_in = tablet_tool_proximity_in,
    .proximity_out = tablet_tool_proximity_out,
    .down = tablet_tool_down,
    .up = tablet_tool_up,
    .motion = tablet_tool_motion,
    .pressure = tablet_tool_pressure,
    .distance = tablet_tool_distance,
    .tilt = tablet_tool_tilt,
    .rotation = tablet_tool_rotation,
    .slider = tablet_tool_slider,
    .wheel = tablet_tool_wheel,
    .button = tablet_tool_button_state,
    .frame = tablet_tool_frame
};

static void tablet_seat_handle_add_tool(void *data, struct zwp_tablet_seat_v2 *,
                            struct zwp_tablet_tool_v2 *tool)
{
    if (!tool) {
        return;
    }

    struct wl_conn *conn = (struct wl_conn *)data;
    struct display *d = conn->dpy;
    conn->tablet_tools.push_back(tool);
    zwp_tablet_tool_v2_add_listener(tool, &tablet_tool_listener, conn);
    ALOGI("Added tablet tool");
}

static const struct zwp_tablet_seat_v2_listener tablet_seat_listener = {
    tablet_seat_handle_add_tablet,
    tablet_seat_handle_add_tool,
    tablet_seat_handle_add_pad
};

static void add_tablet_seat(struct wl_conn *conn) {
    ensure_input_pipe(INPUT_TABLET);
    conn->tablet_seat = zwp_tablet_manager_v2_get_tablet_seat(conn->tablet_manager, conn->seat);
    zwp_tablet_seat_v2_add_listener(conn->tablet_seat, &tablet_seat_listener, conn);
}

static void
registry_handle_global(void *data, struct wl_registry *registry,
               uint32_t id, const char *interface, uint32_t version)
{
    struct wl_conn *conn = (struct wl_conn *)data;

    if (strcmp(interface, "wl_compositor") == 0) {
        conn->compositor =
            (struct wl_compositor*)wl_registry_bind(registry,
                id, &wl_compositor_interface, std::min(version, 5U));
    } else if (strcmp(interface, "wl_subcompositor") == 0) {
        conn->subcompositor =
        (struct wl_subcompositor*)wl_registry_bind(registry,
                id, &wl_subcompositor_interface, 1);
    } else if (strcmp(interface, "xdg_wm_base") == 0) {
        conn->wm_base = (struct xdg_wm_base*)wl_registry_bind(registry,
                id, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(conn->wm_base, &xdg_wm_base_listener, conn);
    } else if(strcmp(interface, "wl_shell") == 0) {
        conn->shell = (struct wl_shell *)wl_registry_bind(
                registry, id, &wl_shell_interface, 1);
    } else if (strcmp(interface, "wl_seat") == 0) {
        conn->seat = (struct wl_seat*)wl_registry_bind(registry, id,
                &wl_seat_interface, std::min(version, (uint32_t)WL_POINTER_AXIS_SOURCE_SINCE_VERSION));
        wl_seat_add_listener(conn->seat, &seat_listener, conn);
        if (conn->tablet_manager && !conn->tablet_seat)
            add_tablet_seat(conn);
        if (conn->data_device_manager && !conn->data_device)
            conn->data_device = wl_data_device_manager_get_data_device(conn->data_device_manager, conn->seat);
    } else if (strcmp(interface, "wl_shm") == 0) {
		conn->shm = (struct wl_shm *)wl_registry_bind(registry, id,
                &wl_shm_interface, 1);
    } else if (strcmp(interface, "wl_output") == 0) {
        conn->output = (struct wl_output*)wl_registry_bind(registry, id,
                &wl_output_interface, std::min(version, 3U));
        wl_output_add_listener(conn->output, &output_listener, conn);
        wl_display_roundtrip(conn->display);
    } else if (strcmp(interface, "wp_presentation") == 0) {
        bool no_presentation = property_get_bool("persist.waydroid.no_presentation", false);
        if (!no_presentation) {
            conn->presentation = (struct wp_presentation*)wl_registry_bind(registry, id,
                    &wp_presentation_interface, 1);
            wp_presentation_add_listener(conn->presentation,
                    &presentation_listener, conn);
        }
    } else if (strcmp(interface, "wp_viewporter") == 0) {
        conn->viewporter = (struct wp_viewporter*)wl_registry_bind(registry, id,
                &wp_viewporter_interface, 1);
    } else if ((conn->dpy->gtype == GrallocType::GRALLOC_ANDROID) &&
               (strcmp(interface, "android_wlegl") == 0)) {
        conn->android_wlegl = (struct android_wlegl*)wl_registry_bind(registry, id,
                &android_wlegl_interface, 1);
    } else if ((conn->dpy->gtype == GrallocType::GRALLOC_GBM || conn->dpy->gtype == GrallocType::GRALLOC_CROS) &&
               (strcmp(interface, "zwp_linux_dmabuf_v1") == 0)) {
        if (version < 3)
            return;
        conn->dmabuf = (struct zwp_linux_dmabuf_v1*)wl_registry_bind(registry, id,
                &zwp_linux_dmabuf_v1_interface, 3);
        zwp_linux_dmabuf_v1_add_listener(conn->dmabuf, &dmabuf_listener, conn);
    } else if (strcmp(interface, "zwp_tablet_manager_v2") == 0) {
        conn->tablet_manager = (struct zwp_tablet_manager_v2 *)wl_registry_bind(registry, id,
                &zwp_tablet_manager_v2_interface, 1);
        if (conn->tablet_manager && conn->seat)
            add_tablet_seat(conn);
    } else if (strcmp(interface, "zwp_pointer_constraints_v1") == 0) {
        conn->pointer_constraints = (struct zwp_pointer_constraints_v1 *)wl_registry_bind(
                registry, id, &zwp_pointer_constraints_v1_interface, 1);
    } else if (strcmp(interface, "zwp_pointer_gestures_v1") == 0) {
        conn->pointer_gestures = (struct zwp_pointer_gestures_v1 *)wl_registry_bind(
                registry, id, &zwp_pointer_gestures_v1_interface, 1);
    } else if (strcmp(interface, "zwp_relative_pointer_manager_v1") == 0) {
        conn->relative_pointer_manager = (struct zwp_relative_pointer_manager_v1 *)wl_registry_bind(
                registry, id, &zwp_relative_pointer_manager_v1_interface, 1);
    } else if (strcmp(interface, "zwp_idle_inhibit_manager_v1") == 0) {
        conn->idle_manager = (struct zwp_idle_inhibit_manager_v1 *)wl_registry_bind(
                registry, id, &zwp_idle_inhibit_manager_v1_interface, 1);
    } else if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0) {
        conn->fractional_scale_manager = (struct wp_fractional_scale_manager_v1*)wl_registry_bind(registry, id,
                &wp_fractional_scale_manager_v1_interface, 1);
    } else if (strcmp(interface, wl_data_device_manager_interface.name) == 0) {
        conn->data_device_manager = (struct wl_data_device_manager *)wl_registry_bind(registry, id,
                &wl_data_device_manager_interface, std::min(version,  3U));
        if (conn->data_device_manager && conn->seat)
            conn->data_device = wl_data_device_manager_get_data_device(conn->data_device_manager, conn->seat);
    } else if (strcmp(interface, "gtk_shell1") == 0) {
        if (version < 6)
            conn->supports_cursor_viewport = false;
    } else if (strcmp(interface, "mir_shell_v1") == 0) {
        conn->supports_cursor_hw_buffer = false;
    }
}

static void
registry_handle_global_remove(void *, struct wl_registry *, uint32_t)
{
}

static const struct wl_registry_listener registry_listener = {
    registry_handle_global,
    registry_handle_global_remove
};

static int
str_starts_with(const char *a, const char *b)
{
    return strncmp(a, b, strlen(b));
}

GrallocType
get_gralloc_type(const char *gralloc)
{
    if (strcmp(gralloc, "default") == 0) {
        return GrallocType::GRALLOC_DEFAULT;
    } else if (strcmp(gralloc, "gbm") == 0) {
        return GrallocType::GRALLOC_GBM;
    } else if (str_starts_with(gralloc, "minigbm") == 0) {
        return GrallocType::GRALLOC_CROS;
    } else {
        return GrallocType::GRALLOC_ANDROID;
    }
}

static void
wayland_log_handler (const char *format, va_list args)
{
    LOG_PRI_VA (ANDROID_LOG_ERROR, "wayland-hwc", format, args);
}

bool display::task_closing(const std::string &tid) {
    std::scoped_lock lock(windowsMutex);
    auto it = tasks.find(tid);
    return it != tasks.end() && it->second.closing;
}

/* A close-pending task whose taskRemoved never arrived (lost oneway call,
 * removeTask failure) must not suppress its card forever. */
void display::expire_closing_marks() {
    std::scoped_lock lock(windowsMutex);
    auto now = std::chrono::steady_clock::now();
    for (auto &[tid, task] : tasks) {
        if (task.closing && now - task.closing_since > std::chrono::seconds(10)) {
            ALOGW("task %s: no taskRemoved within 10s of close, dropping close-pending mark", tid.c_str());
            task.closing = false;
        }
    }
}

/* Drop every trace of a task Android says is gone. Caller holds windowsMutex.
 * Returns whether a card was closed, so the caller can flush wayland once. */
bool display::forget_task(const std::string &tid) {
    tasks.erase(tid);

    /* task_streams is keyed by the numeric ID; entries self-healed from a
     * layer name are decimal too, but parse defensively. */
    char *end = nullptr;
    unsigned long id = strtoul(tid.c_str(), &end, 10);
    if (end != tid.c_str() && *end == '\0')
        task_streams.erase(static_cast<uint32_t>(id));

    bool had_window = windows.find(tid) != windows.end();
    if (had_window)
        windows.erase(tid);
    return had_window;
}

/* A TID layer named a task the table doesn't know: repair it. Covers tasks
 * that predate a composer restart and any missed oneway event. */
void display::note_task_from_layer(const std::string &tid, const std::string &aid) {
    std::scoped_lock lock(windowsMutex);
    auto it = tasks.find(tid);
    if (it == tasks.end()) {
        if (task_events_seen)
            ALOGW("task %s (%s) seen in layers but never pushed by WMS; self-healed", tid.c_str(), aid.c_str());
        auto &task = tasks[tid];
        task.appID = aid;
        task.from_layer = true;
    } else if (it->second.appID.empty()) {
        it->second.appID = aid;
    }
}

window *open_windows::add(waydroid_hwc_composer_device_1 *pdev, const std::string& key, const std::string& aid, const std::string& tid, hwc_color_t color) {
    window *window = nullptr;
    update([&](){
        auto res = windows.emplace(
            key,
            window::create(pdev->display->ctl.get(), pdev->should_compose, aid, tid, color)
        );
        assert(res.second);
        window = res.first->second.get();
    });
    /* A new toplevel may need the display awake to draw its first frame; if it
     * launched while Android was dozed, wake it now (see any_window_visible). */
    update_screen_power(pdev->display);
    return window;
}

void open_windows::add(const std::string& key, std::unique_ptr<window> window) {
    update([&](){
        auto res = windows.emplace(
            key,
            std::move(window)
        );
        assert(res.second); (void)res;
    });
}

void open_windows::clear() {
    windows.clear();
    property_set("waydroid.open_windows", "0");
    publish_open_apps();
}

/* Props outlive the composer but published_apps does not, so a card that was
 * open when the HAL died leaves waydroid.open.<appID>=1 with nothing left to
 * clear it: "waydroid app launch --wait" blocks forever, and the host launcher
 * therefore keeps treating the app as running, so its icon only re-focuses a
 * window that no longer exists. Call this before any window is created — every
 * such prop is stale by definition then, and publish_open_apps() re-sets the
 * ones that come back. */
void open_windows::clear_stale_props() {
    static const char kPrefix[] = "waydroid.open.";
    std::vector<std::string> stale;

    __system_property_foreach([](const prop_info *pi, void *cookie) {
        __system_property_read_callback(pi, [](void *cookie, const char *name,
                                               const char *value, uint32_t) {
            if (!strncmp(name, kPrefix, sizeof(kPrefix) - 1) && strcmp(value, "0"))
                static_cast<std::vector<std::string> *>(cookie)->emplace_back(name);
        }, cookie);
    }, &stale);

    for (auto const& name : stale) {
        ALOGI("clearing stale %s left by a previous composer instance", name.c_str());
        property_set(name.c_str(), "0");
    }
    property_set("waydroid.open_windows", "0");
}

/* Maintain waydroid.open.<appID> for "waydroid app launch --wait". */
void open_windows::publish_open_apps() {
    std::set<std::string> current;
    for (auto const& [key, win] : windows) {
        if (!win->appID.empty() && win->appID != "none")
            current.insert(win->appID);
    }
    for (auto const& aid : published_apps) {
        if (!current.count(aid))
            property_set(("waydroid.open." + aid).c_str(), "0");
    }
    for (auto const& aid : current) {
        if (!published_apps.count(aid))
            property_set(("waydroid.open." + aid).c_str(), "1");
    }
    published_apps = std::move(current);
}

void open_windows::erase(const_iterator pos) {
    struct display *display = pos->second->conn->dpy;
    update([&](){
        windows.erase(pos);
    });
    /* Destroying a surface emits no wl_output leave, so recheck visibility
     * here or the screen-power tracking stays "on" after the last window
     * closes. Not done in clear(): reconnect_display() tears down windows
     * only to recreate them, and must not flip the screen meanwhile. */
    update_screen_power(display);
}

void open_windows::erase(const key_type& key) {
    auto pos = windows.find(key);
    if (pos == windows.end())
        return;
    struct display *display = pos->second->conn->dpy;
    windows.erase(pos);
    std::string windows_size_str = std::to_string(windows.size());
    property_set("waydroid.open_windows", windows_size_str.c_str());
    publish_open_apps();
    update_screen_power(display);
}

/* Null every wayland global proxy pointer. The proxies themselves are freed by
 * wl_display_disconnect(); this just prevents stale use until they are rebound
 * by registry_handle_global() on the next roundtrip. */
static void reset_wayland_globals(struct wl_conn *conn) {
    conn->registry = nullptr;
    conn->compositor = nullptr;
    conn->subcompositor = nullptr;
    conn->seat = nullptr;
    conn->shell = nullptr;
    conn->shm = nullptr;
    conn->pointer = nullptr;
    conn->keyboard = nullptr;
    conn->touch = nullptr;
    conn->output = nullptr;
    conn->presentation = nullptr;
    conn->viewporter = nullptr;
    conn->android_wlegl = nullptr;
    conn->dmabuf = nullptr;
    conn->wm_base = nullptr;
    conn->tablet_manager = nullptr;
    conn->tablet_seat = nullptr;
    conn->pointer_constraints = nullptr;
    conn->relative_pointer_manager = nullptr;
    conn->relative_pointer = nullptr;
    conn->idle_manager = nullptr;
    conn->fractional_scale_manager = nullptr;
    conn->data_device_manager = nullptr;
    conn->data_device = nullptr;
}

/* Tear down everything that belongs to the connection and reconnect,
 * rebinding globals. The caller has already dropped the display-side state
 * that references this connection's proxies. */
/* If the previous reconnect was moments ago, the new connection died right
 * away (e.g. a protocol error every frame). Back off so a reconnect loop
 * cannot saturate a core and drown the host compositor in connection churn.
 * Called before the caller drops its own state, so the sleep bounds the whole
 * cycle and not just the wayland half. */
static void wl_conn_reconnect_backoff(struct wl_conn *conn) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (conn->last_reconnect.tv_sec && now.tv_sec - conn->last_reconnect.tv_sec < 2) {
        ALOGE("wl_conn_reconnect: reconnecting again within 2s, backing off");
        usleep(500 * 1000);
    }
    conn->last_reconnect = now;
}

static void wl_conn_reconnect(struct wl_conn *conn) {
    conn->layers.clear();
    conn->touch_surfaces.clear();
    conn->tablet_tools.clear();
    conn->tablet_tools_evt.clear();
    conn->pointer_surface = nullptr;
    conn->tablet_surface = nullptr;
    /* Cached wl_buffers are keyed by gralloc handle, which Android reuses
     * across the reconnect: a stale hit would attach a freed proxy. */
    conn->buffer_map.clear();
    conn->formats.clear();
    conn->modifiers.clear();

    if (conn->registry)
        wl_registry_destroy(conn->registry);
    wl_display_disconnect(conn->display);
    conn->display = nullptr;
    reset_wayland_globals(conn);

    /* Reconnect with backoff; the host compositor may not be ready yet. */
    while (true) {
        conn->display = wl_display_connect(NULL);
        if (conn->display)
            break;
        ALOGE("wl_conn_reconnect: wl_display_connect failed, retrying");
        usleep(500 * 1000);
    }

    conn->registry = wl_display_get_registry(conn->display);
    wl_registry_add_listener(conn->registry, &registry_listener, conn);
    wl_display_roundtrip(conn->display);
    ALOGI("wl_conn_reconnect: rebound wayland globals on new connection");
}

void reconnect_display(struct display *display) {
    wl_conn_reconnect_backoff(display->ctl.get());

    /* Caller holds windowsMutex. Drop everything that references the dead
     * connection's proxies BEFORE wl_display_disconnect() frees them. */
    display->windows.clear();
    display->task_streams.clear();
    /* The cursor handler holds wl_surfaces; drop it now and let the caller
     * (which has pdev) recreate it after we rebind the compositor. */
    display->cursor_handler.reset();

    wl_conn_reconnect(display->ctl.get());
}

static void* wl_conn_thread(void* data) {
    auto* conn = static_cast<struct wl_conn *>(data);

    setpriority(PRIO_PROCESS, 0, HAL_PRIORITY_URGENT_DISPLAY);

    while (true) {
        if (wl_display_dispatch(conn->display) != -1)
            continue;

        /* The connection dropped. Rather than aborting the whole HAL, signal
         * the compose thread to reconnect and park until it wakes us on the
         * fresh connection. */
        ALOGE("*** %s: Wayland client was disconnected: %s; awaiting reconnect",
              __PRETTY_FUNCTION__, strerror(errno));
        conn->wl_alive = false;
        sem_wait(&conn->reconnect_resume);
        ALOGI("*** %s: resuming dispatch on reconnected display", __PRETTY_FUNCTION__);
    }
}

/* Connect to the host compositor, bind the globals and start dispatching.
 * Returns false with nothing left running on failure. */
static bool wl_conn_open(struct wl_conn *conn) {
    conn->supports_cursor_viewport = true;
    conn->supports_cursor_hw_buffer = property_get_bool("persist.waydroid.cursor_force_shm", false);
    sem_init(&conn->reconnect_resume, 0, 0);

    conn->display = wl_display_connect(NULL);
    ALOGI("WAYLAND_DISPLAY: %s", getenv("WAYLAND_DISPLAY"));
    ALOGI("XDG_RUNTIME_DIR: %s", getenv("XDG_RUNTIME_DIR"));
    if (!conn->display) {
        ALOGE("Couldn't open Wayland display.");
        sem_destroy(&conn->reconnect_resume);
        return false;
    }

    conn->registry = wl_display_get_registry(conn->display);
    wl_registry_add_listener(conn->registry, &registry_listener, conn);
    wl_display_roundtrip(conn->display);

    if (conn->dpy->gtype == GrallocType::GRALLOC_ANDROID && !conn->android_wlegl)
        ALOGE("GRALLOC_ANDROID requested, but the Wayland compositor did not advertise android_wlegl");

    if (pthread_create(&conn->wayland_thread, nullptr, wl_conn_thread, conn) != 0) {
        ALOGE("Couldn't create wayland thread");
        wl_display_disconnect(conn->display);
        sem_destroy(&conn->reconnect_resume);
        return false;
    }

    return true;
}

/* Stop dispatching. Separate from wl_conn_destroy so a caller can quiesce the
 * connection before tearing down state the dispatch thread can reach. */
static void wl_conn_stop(struct wl_conn *conn) {
    pthread_kill(conn->wayland_thread, SIGTERM);
    pthread_join(conn->wayland_thread, nullptr);
}

static void wl_conn_destroy(struct wl_conn *conn) {
    if (conn->wm_base)
        xdg_wm_base_destroy(conn->wm_base);

    if (conn->shell)
        wl_shell_destroy(conn->shell);

    if (conn->compositor)
        wl_compositor_destroy(conn->compositor);

    if (conn->tablet_manager) {
        for (struct zwp_tablet_tool_v2 *t : conn->tablet_tools) {
            zwp_tablet_tool_v2_destroy(t);
        }
        zwp_tablet_seat_v2_destroy(conn->tablet_seat);
        zwp_tablet_manager_v2_destroy(conn->tablet_manager);
    }

    if (conn->relative_pointer_manager)
        zwp_relative_pointer_manager_v1_destroy(conn->relative_pointer_manager);

    if (conn->pointer_constraints)
        zwp_pointer_constraints_v1_destroy(conn->pointer_constraints);

    wl_registry_destroy(conn->registry);
    wl_display_flush(conn->display);
    wl_display_disconnect(conn->display);

    sem_destroy(&conn->reconnect_resume);
}

struct display *
create_display(const char *gralloc)
{
    struct display *display = new struct display();
    if (display == NULL) {
        ALOGE("out of memory");
        return NULL;
    }
    wl_log_set_handler_client(wayland_log_handler);
    display->system_version = property_get_int32("ro.system.build.version.sdk", 0);
    display->gtype = get_gralloc_type(gralloc);
    display->refresh = 0;
    display->isMaximized = true;

    sem_init(&display->egl_go, 0, 0);
    sem_init(&display->egl_done, 0, 0);

    umask(0);
    mkdir("/dev/input", S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
    chown("/dev/input", 1000, 1000);
    init_input_devices(display);

    display->ctl = std::make_unique<wl_conn>();
    display->ctl->dpy = display;
    display->ctl->is_ctl = true;
    if (!wl_conn_open(display->ctl.get())) {
        sem_destroy(&display->egl_go);
        sem_destroy(&display->egl_done);
        return nullptr;
    }

    display->task = IWaydroidTask::getService();
    display->deactivate_thread = std::thread(deactivate_worker, display);
    return display;
}

void
destroy_display(struct display *display)
{
    wl_conn_stop(display->ctl.get());

    if (display->deactivate_thread.joinable()) {
        {
            std::scoped_lock lock(display->deactivate_mutex);
            display->deactivate_quit = true;
            display->deactivate_cond.notify_one();
        }
        display->deactivate_thread.join();
    }

    wl_conn_destroy(display->ctl.get());

    delete display;
}

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
#include "egl-tools.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <unistd.h>
#include <sys/mman.h>
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

#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#include <cutils/trace.h>
#include <cutils/properties.h>

#include <xkbcommon/xkbcommon.h>

#include "gralloc_handler.h"

#include <wayland-client.h>
#include <wayland-android-client-protocol.h>
#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "presentation-time-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include "tablet-unstable-v2-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "idle-inhibit-unstable-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"

using ::android::hardware::hidl_string;

struct buffer;

void
destroy_buffer(struct buffer* buf) {
    wl_buffer_destroy(buf->buffer);
    if (buf->isShm)
        munmap(buf->shm_data, buf->size);
    delete buf;
}

// Call me from egl_worker_thread only!
void snapshot_inactive_app_window(struct display *display, struct window *window) {
    if (!window->layers[0].surface || !window->last_layer_buffer
        || window->last_layer_buffer->isShm || window->snapshot_buffer) {
        // Need a surface to draw and a non-SHM buffer to make snapshot from
        return;
    }

    ALOGI("Making inactive window snapshot for %s", window->taskID.c_str());

    struct buffer *old_buf = window->last_layer_buffer;

    buffer_metadata metadata {
        old_buf->height,
        old_buf->width,
        old_buf->pixel_stride,
        old_buf->hal_format
    };
    // TODO: Actually use unique_ptr
    struct buffer *new_buf = create_shm_wl_buffer(display, metadata, old_buf->handle).release();
    if (!new_buf) {
        ALOGE("failed to create a wayland buffer for window snapshot");
        return;
    }

    // FIXME won't work as expected if there are multiple surfaces
    struct wl_surface *surface = window->layers[0].surface;

    egl_render_to_pixels(display, new_buf);

    wl_surface_attach(surface, new_buf->buffer, 0, 0);
    if (wl_surface_get_version(surface) >= WL_SURFACE_DAMAGE_BUFFER_SINCE_VERSION)
        wl_surface_damage_buffer(surface, 0, 0, new_buf->width, new_buf->height);
    else
        wl_surface_damage(surface, 0, 0, new_buf->width, new_buf->height);
    if (!display->viewporter && display->scale > 1) {
        // With no viewporter the scale is guaranteed to be integer
        wl_surface_set_buffer_scale(surface, (int)display->scale);
    }
    wl_surface_commit(surface);

    window->snapshot_buffer = new_buf;
}

static void
xdg_surface_handle_configure(void *, struct xdg_surface *surface,
                 uint32_t serial)
{
    xdg_surface_ack_configure(surface, serial);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    xdg_surface_handle_configure,
};


void
finished_computing_scale(struct display *d)
{
    char property[PROPERTY_VALUE_MAX];
    int default_density = 180;
    std::string display_scale = std::to_string(d->scale);
    property_set("waydroid.display_scale", display_scale.c_str());
    if (property_get("ro.sf.lcd_density", property, nullptr) <= 0) {
        std::string lcd_density = std::to_string(int(default_density * d->scale));
        property_set("ro.sf.lcd_density", lcd_density.c_str());
    }
}

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

static void
xdg_toplevel_handle_configure(void *data, struct xdg_toplevel *,
                              int32_t width, int32_t height,
                              struct wl_array *)
{
    struct window *window = (struct window *)data;
    struct display *display = window->display;

    if (width == 0 || height == 0) {
		/* Compositor is deferring to us */
		return;
	}

    if (!display->width || !display->height) {
        choose_width_height(display, width, height);
        if (!display->isMaximized)
            xdg_toplevel_unset_maximized(window->xdg_toplevel);
    }
}

static void
send_key_event(display *data, uint32_t key, wl_keyboard_key_state state);

static void
xdg_toplevel_handle_close(void *data, struct xdg_toplevel *)
{
    struct window *window = (struct window *)data;

    // simulate user input to restart idle timeout (TODO: find a better way)
    send_key_event(window->display, 0, WL_KEYBOARD_KEY_STATE_PRESSED);
    send_key_event(window->display, 0, WL_KEYBOARD_KEY_STATE_RELEASED);

    if (window->display->task != nullptr) {
        if (window->taskID != "none") {
            if (window->taskID == "0") {
                property_set("waydroid.active_apps", "none");
                window->display->task->removeAllVisibleRecentTasks();
            } else {
                window->display->task->removeTask(stoi(window->taskID));
            }
        }
    }

    std::scoped_lock lock(window->display->windowsMutex);
    destroy_window(window, true);
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
    struct display *display = window->display;

    if (width == 0 || height == 0) {
		/* Compositor is deferring to us */
		return;
	}

    if (!display->width || !display->height) {
        choose_width_height(display, width, height);
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

void
destroy_window(struct window *window, bool keep)
{
    if (window->isActive) {
        if (window->xdg_toplevel)
            xdg_toplevel_destroy(window->xdg_toplevel);
        if (window->xdg_surface)
            xdg_surface_destroy(window->xdg_surface);
        if (window->shell_surface)
            wl_shell_surface_destroy(window->shell_surface);
        if (window->bg_buffer)
            wl_buffer_destroy(window->bg_buffer);
        if (window->input_region)
            wl_region_destroy(window->input_region);

        window->layers.clear();
        if (window->destroy_background_objects) {
            if (window->viewport)
                wp_viewport_destroy(window->viewport);
            if (window->surface) {
                wl_surface_set_user_data(window->surface, nullptr);
                wl_surface_destroy(window->surface);
            }
        }

        wl_display_flush(window->display->display);

        auto index = std::find(window->display->windows.cbegin(), window->display->windows.cend(), window);
        window->display->windows.erase(index);
    }
    if (keep)
        window->isActive = false;
    else
        delete window;
}

static void fractional_scale_handle_preferred_scale(void *data, struct wp_fractional_scale_v1 *,
            uint32_t scale_times_120) {
    struct display *display = (struct display *)data;
    if (!display->viewporter) {
        // We should always have the viewporter if we have the fractional scale manager
        // but for debugging purpuses we may decide to disable one
        return;
    }
    display->scale = scale_times_120 / 120.0;
}

static const struct wp_fractional_scale_v1_listener fractional_scale_listener = {
    .preferred_scale = fractional_scale_handle_preferred_scale
};

struct window *
create_window(struct display *display, bool use_subsurfaces, std::string appID, std::string taskID, hwc_color_t color)
{
    struct window *window = new struct window();
    if (!window)
        return NULL;

    window->display = display;
    window->surface = wl_compositor_create_surface(display->compositor);
    window->appID = appID;
    window->taskID = taskID;
    window->isActive = true;
    window->destroy_background_objects = true;
    window->bg_buffer = NULL;

    int fd = syscall(SYS_memfd_create, "buffer", 0);
    ftruncate(fd, 4);
    void *shm_data = mmap(NULL, 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm_data == MAP_FAILED) {
        ALOGE("mmap failed");
        close(fd);
        exit(1);
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(display->shm, fd, 4);

    bool calibrating = !display->height || !display->width;
    if (calibrating) {
        // Initialize width and height with user-provided overrides if any
        choose_width_height(display, 0, 0);
    }

    if (display->wm_base) {
        window->xdg_surface =
                xdg_wm_base_get_xdg_surface(display->wm_base, window->surface);
        assert(window->xdg_surface);
        
        xdg_surface_add_listener(window->xdg_surface,
                                     &xdg_surface_listener, window);

        window->xdg_toplevel = xdg_surface_get_toplevel(window->xdg_surface);
        assert(window->xdg_toplevel);
        xdg_toplevel_add_listener(window->xdg_toplevel, &xdg_toplevel_listener, window);
        if (display->isMaximized || !display->height || !display->width)
            xdg_toplevel_set_maximized(window->xdg_toplevel);
        const hidl_string appID_hidl(appID);
        hidl_string appName_hidl(appID);
        if (appID != "Waydroid" && display->task)
            display->task->getAppName(appID_hidl, [&](const hidl_string &value)
                                      { xdg_toplevel_set_title(window->xdg_toplevel, value.c_str()); });
        else
            xdg_toplevel_set_title(window->xdg_toplevel, appID.c_str());

        if (appID != "Waydroid")
            appID = "waydroid." + appID;
        xdg_toplevel_set_app_id(window->xdg_toplevel, appID.c_str());
    } else if (display->shell) {
        window->shell_surface =
            wl_shell_get_shell_surface(display->shell, window->surface);
        assert(window->shell_surface);

        wl_shell_surface_add_listener(window->shell_surface, &shell_surface_listener, window);
        wl_shell_surface_set_toplevel(window->shell_surface);
        if (display->isMaximized || !display->height || !display->width)
            wl_shell_surface_set_maximized(window->shell_surface, display->output);
        const hidl_string appID_hidl(appID);
        hidl_string appName_hidl(appID);
        if (appID != "Waydroid" && display->task)
            display->task->getAppName(appID_hidl, [&](const hidl_string &value)
                                      { wl_shell_surface_set_title(window->shell_surface, value.c_str()); });
        else
            wl_shell_surface_set_title(window->shell_surface, appID.c_str());
    } else {
        abort();
    }

    wl_surface_commit(window->surface);
    // Handle first configure event
    wl_display_roundtrip(display->display);

    if (calibrating) {
        wp_fractional_scale_v1* fs;
        if (display->fractional_scale_manager) {
            // We only support one global scale
            fs = wp_fractional_scale_manager_v1_get_fractional_scale(
                    display->fractional_scale_manager, window->surface);
            wp_fractional_scale_v1_add_listener(fs, &fractional_scale_listener, display);
        }
        if (!display->height || !display->width) {
            // Some compositors fail to give us a window size without a buffer attached
            // See: https://github.com/swaywm/sway/issues/2176
            struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, 1, 1, 4, WL_SHM_FORMAT_ARGB8888);
            wl_surface_attach(window->surface, buf, 0, 0);
            wl_surface_commit(window->surface);
            // Try second configure event, with buffer attached
            wl_display_roundtrip(display->display);
        }
        if (display->fractional_scale_manager) {
            wl_surface_commit(window->surface);
            wl_display_roundtrip(display->display);
            wp_fractional_scale_v1_destroy(fs);
        }

        // If after all of this we still did not receive a proper configure event,
        // fallback to using the full output size.
        // NOTICE: full_width and full_heigth should be in compositor logical size!
        if (!display->height)
            display->height = display->full_height / display->scale;
        if (!display->width)
            display->width = display->full_width / display->scale;

        finished_computing_scale(display);
    }

    if (display->viewporter) {
        window->viewport = wp_viewporter_get_viewport(display->viewporter, window->surface);
    }

    wl_surface_set_user_data(window->surface, window);
    display->windows.push_back(window);

    // TODO: Fix background when viewport is not supported
    // No subsurface background for us!
    if (!display->subcompositor ||
        !display->viewporter ||
        property_get_bool("persist.waydroid.no_background_subsurface", false)) {
        window->destroy_background_objects = false;
        window->layers.emplace_back(window->surface, window->viewport);
        wl_shm_pool_destroy(pool);
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
    close(fd);

    struct wl_surface *surface = window->surface;
    wl_surface_attach(surface, window->bg_buffer, 0, 0);
    wl_surface_damage(surface, 0, 0, INT32_MAX, INT32_MAX);
    wp_viewport_set_source(window->viewport, wl_fixed_from_int(0), wl_fixed_from_int(0), wl_fixed_from_int(1), wl_fixed_from_int(1));
    wp_viewport_set_destination(window->viewport, display->width, display->height);

    if (display->wm_base)
        xdg_surface_set_window_geometry(window->xdg_surface, 0, 0, display->width, display->height);

    struct wl_region *region = wl_compositor_create_region(display->compositor);
    if (color.a == 0) {
        wl_surface_set_input_region(surface, region);
        if (display->system_version >= 33)
            window->input_region = region;
        else
            wl_region_destroy(region);
    }
    if (color.a == 255) {
        wl_region_add(region, 0, 0, display->width, display->height);
        wl_surface_set_opaque_region(surface, region);
        wl_region_destroy(region);
    }

    wl_surface_commit(surface);

    return window;
}

window::layer::layer(wl_surface *surface, wp_viewport *viewport, wl_subsurface *subsurface) : surface(surface), subsurface(subsurface), viewport(viewport) {}

window::layer::~layer() {
    if (viewport)
        wp_viewport_destroy(viewport);
    if (subsurface)
        wl_subsurface_destroy(subsurface);
    if (surface)
        wl_surface_destroy(surface);
}

window::layer::layer(window::layer&& other) : surface(other.surface), subsurface(other.subsurface), viewport(other.viewport) {
    other.surface = nullptr;
    other.subsurface = nullptr;
    other.viewport = nullptr;
}

window::layer& window::layer::operator=(window::layer&& rhs) {
    if (viewport)
        wp_viewport_destroy(viewport);
    if (subsurface)
        wl_subsurface_destroy(subsurface);
    if (surface)
        wl_surface_destroy(surface);

    surface = rhs.surface;
    subsurface = rhs.subsurface;
    viewport = rhs.viewport;

    rhs.surface = nullptr;
    rhs.subsurface = nullptr;
    rhs.viewport = nullptr;

    return *this;
}

window::layer& window::create_new_layer() {
    wl_surface *surface = wl_compositor_create_surface(display->compositor);
    wl_subsurface *subsurface = wl_subcompositor_get_subsurface(display->subcompositor, surface, this->surface);
    wp_viewport *viewport = nullptr;
    if (display->viewporter)
        viewport = wp_viewporter_get_viewport(display->viewporter, surface);

    return layers.emplace_back(surface, viewport, subsurface);
}

static int
ensure_pipe(struct display* display, int input_type)
{
    if (display->input_fd[input_type] == -1) {
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
    struct display *display = (struct display *)data;
    display->keyboard_enter_serial = serial;

    std::scoped_lock lock(display->windowsMutex);
    auto window = reinterpret_cast<struct window *>(wl_surface_get_user_data(surface));
    if (!window)
        return; // Window was destroyed in hwc_set

    if (display->task != nullptr) {
        if (window->taskID != "none" && window->taskID != "0") {
            window->display->task->setFocusedTask(stoi(window->taskID));
        }
    }
}

static void
keyboard_handle_leave(void *data, struct wl_keyboard *,
                      uint32_t, struct wl_surface *)
{
    struct display *display = (struct display *)data;

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
    send_key_event((struct display*)data, key, (enum wl_keyboard_key_state)state);
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

static void
pointer_handle_enter(void *data, struct wl_pointer *pointer,
                     uint32_t serial, struct wl_surface *surface,
                     wl_fixed_t, wl_fixed_t)
{
    struct display *display = (struct display *)data;
    display->pointer_surface = surface;
    display->pointer_enter_serial = serial;
    if (display->cursor_surface)
        wl_pointer_set_cursor(pointer, serial, display->cursor_surface,
                              int(display->cursor_hotspot.x / display->scale),
                              int(display->cursor_hotspot.y / display->scale));
}

static void
pointer_handle_leave(void *data, struct wl_pointer *pointer,
                     uint32_t serial, struct wl_surface *)
{
    struct display *display = (struct display *)data;
    display->pointer_surface = NULL;
    if (display->cursor_surface)
        wl_pointer_set_cursor(pointer, serial, NULL, 0, 0);
}

static void
pointer_handle_motion(void *data, struct wl_pointer *,
                      uint32_t, wl_fixed_t sx, wl_fixed_t sy)
{
    struct display* display = (struct display*)data;
    struct input_event event[5];
    struct timespec rt;
    int x, y;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_POINTER))
        return;

    if (!display->pointer_surface)
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
    x += display->layers[display->pointer_surface].x;
    y += display->layers[display->pointer_surface].y;

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
    struct display *display = (struct display *)data;
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
    struct display* display = (struct display*)data;
    struct input_event event[2];
    struct timespec rt;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_POINTER))
        return;

    if (!display->pointer_surface)
        return;

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
    struct display* display = (struct display*)data;
    struct input_event event[2];
    struct timespec rt;
    unsigned int res, move, n = 0;
    double fVal = wl_fixed_to_double(value) / 10.0f;
    double step = 1.0f;

    if (ensure_pipe(display, INPUT_POINTER))
        return;

    if (!display->pointer_surface)
        return;

    if (!display->reverseScroll) {
        fVal = -fVal;
    }

    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        display->wheelAccumulatorY += fVal;
        if (std::abs(display->wheelAccumulatorY) < step)
            return;
        move = (int)(display->wheelAccumulatorY / step);
        display->wheelAccumulatorY = display->wheelEvtIsDiscrete ? 0 :
                                     std::fmod(display->wheelAccumulatorY, step);
    } else {
        display->wheelAccumulatorX += fVal;
        if (std::abs(display->wheelAccumulatorX) < step)
            return;
        move = (int)(display->wheelAccumulatorX / step);
        display->wheelAccumulatorX = display->wheelEvtIsDiscrete ? 0 :
                                     std::fmod(display->wheelAccumulatorY, step);
    }

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }

    ADD_EVENT(EV_REL, (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
              ? REL_WHEEL : REL_HWHEEL, move);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_POINTER], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
pointer_handle_axis_source(void *data, struct wl_pointer *, uint32_t source)
{
    struct display* display = (struct display*)data;
    display->wheelEvtIsDiscrete = (source == WL_POINTER_AXIS_SOURCE_WHEEL);
}

static void
pointer_handle_axis_stop(void *, struct wl_pointer *, uint32_t, uint32_t)
{
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

static int
get_touch_id(struct display *display, int id)
{
    int i = 0;
    for (i = 0; i < MAX_TOUCHPOINTS; i++) {
        if (display->touch_id[i] == id)
            return i;
    }
    for (i = 0; i < MAX_TOUCHPOINTS; i++) {
        if (display->touch_id[i] == -1) {
            display->touch_id[i] = id;
            return i;
        }
    }
    return -1;
}

static int
flush_touch_id(struct display *display, int id)
{
    for (int i = 0; i < MAX_TOUCHPOINTS; i++) {
        if (display->touch_id[i] == id) {
            display->touch_id[i] = -1;
            return i;
        }
    }
    return -1;
}

static void
touch_handle_down(void *data, struct wl_touch *,
          uint32_t, uint32_t, struct wl_surface *surface,
          int32_t id, wl_fixed_t x_w, wl_fixed_t y_w)
{
    struct display* display = (struct display*)data;
    struct input_event event[6];
    struct timespec rt;
    int x, y;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TOUCH))
        return;

    display->touch_surfaces[id] = surface;

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
    x += display->layers[surface].x;
    y += display->layers[surface].y;

    ADD_EVENT(EV_ABS, ABS_MT_SLOT, get_touch_id(display, id));
    ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, get_touch_id(display, id));
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
    struct display* display = (struct display*)data;
    struct input_event event[3];
    struct timespec rt;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TOUCH))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
       ALOGE("%s:%d error in touch clock_gettime: %s",
            __FILE__, __LINE__, strerror(errno));
    }
    display->touch_surfaces[id] = NULL;

    ADD_EVENT(EV_ABS, ABS_MT_SLOT, flush_touch_id(display, id));
    ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, -1);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_TOUCH], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
touch_handle_motion(void *data, struct wl_touch *,
            uint32_t, int32_t id, wl_fixed_t x_w, wl_fixed_t y_w)
{
    struct display* display = (struct display*)data;
    struct input_event event[6];
    struct timespec rt;
    int x, y;
    unsigned int res, n = 0;

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
    x += display->layers[display->touch_surfaces[id]].x;
    y += display->layers[display->touch_surfaces[id]].y;

    ADD_EVENT(EV_ABS, ABS_MT_SLOT, get_touch_id(display, id));
    ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, get_touch_id(display, id));
    ADD_EVENT(EV_ABS, ABS_MT_POSITION_X, x);
    ADD_EVENT(EV_ABS, ABS_MT_POSITION_Y, y);
    ADD_EVENT(EV_ABS, ABS_MT_PRESSURE, 50);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_TOUCH], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
touch_handle_frame(void *, struct wl_touch *)
{
}

static void
touch_handle_cancel(void *data, struct wl_touch *)
{
    struct display* display = (struct display*)data;
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

    // Cancel all touch points.
    for (i = 0; i < MAX_TOUCHPOINTS; i++) {
        if (display->touch_id[i] != -1) {
            id = display->touch_id[i];
            display->touch_id[i] = -1;
            display->touch_surfaces[id] = NULL;

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
    struct display* display = (struct display*)data;
    struct input_event event[5];
    struct timespec rt;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TOUCH))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
       ALOGE("%s:%d error in touch clock_gettime: %s",
            __FILE__, __LINE__, strerror(errno));
    }
    ADD_EVENT(EV_ABS, ABS_MT_SLOT, get_touch_id(display, id));
    ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, get_touch_id(display, id));
    ADD_EVENT(EV_ABS, ABS_MT_TOUCH_MAJOR, wl_fixed_to_int(major));
    ADD_EVENT(EV_ABS, ABS_MT_TOUCH_MINOR, wl_fixed_to_int(minor));
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_TOUCH], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
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
    struct display *d = (struct display*)data;
    enum wl_seat_capability caps = (enum wl_seat_capability) wl_caps;

    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !d->pointer) {
        d->pointer = wl_seat_get_pointer(seat);
        d->input_fd[INPUT_POINTER] = -1;
        d->ptrPrvX = 0;
        d->ptrPrvY = 0;
        d->reverseScroll = property_get_bool("persist.waydroid.reverse_scrolling", false);
        mkfifo(INPUT_PIPE_NAME[INPUT_POINTER], S_IRWXO | S_IRWXG | S_IRWXU);
        chown(INPUT_PIPE_NAME[INPUT_POINTER], 1000, 1000);
        wl_pointer_add_listener(d->pointer, &pointer_listener, d);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && d->pointer) {
        remove(INPUT_PIPE_NAME[INPUT_POINTER]);
        wl_pointer_destroy(d->pointer);
        d->pointer = NULL;
    }

    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !d->keyboard) {
        d->keyboard = wl_seat_get_keyboard(seat);
        d->input_fd[INPUT_KEYBOARD] = -1;
        mkfifo(INPUT_PIPE_NAME[INPUT_KEYBOARD], S_IRWXO | S_IRWXG | S_IRWXU);
        chown(INPUT_PIPE_NAME[INPUT_KEYBOARD], 1000, 1000);
        wl_keyboard_add_listener(d->keyboard, &keyboard_listener, d);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && d->keyboard) {
        remove(INPUT_PIPE_NAME[INPUT_KEYBOARD]);
        wl_keyboard_destroy(d->keyboard);
        d->keyboard = NULL;
    }

    if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !d->touch) {
        d->touch = wl_seat_get_touch(seat);
        d->input_fd[INPUT_TOUCH] = -1;
        mkfifo(INPUT_PIPE_NAME[INPUT_TOUCH], S_IRWXO | S_IRWXG | S_IRWXU);
        chown(INPUT_PIPE_NAME[INPUT_TOUCH], 1000, 1000);
        for (int i = 0; i < MAX_TOUCHPOINTS; i++)
            d->touch_id[i] = -1;
        wl_touch_set_user_data(d->touch, d);
        wl_touch_add_listener(d->touch, &touch_listener, d);
    } else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && d->touch) {
        remove(INPUT_PIPE_NAME[INPUT_TOUCH]);
        wl_touch_destroy(d->touch);
        d->touch = NULL;
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

    struct display *d = (struct display*)data;
    uint64_t modifier = ((uint64_t)modifier_hi << 32) | modifier_lo;
    if (modifier == DRM_FORMAT_MOD_INVALID)
        return;

    std::stringstream prop_name_stream;
    std::stringstream prop_value_stream;
    prop_name_stream << "waydroid.modifiers." << std::hex << format << "." << std::dec << d->modifiers[format].size();
    prop_value_stream << std::hex << modifier;
    std::string prop_name = prop_name_stream.str();
    std::string prop_value = prop_value_stream.str();
    property_set(prop_name.c_str(), prop_value.c_str());

    d->modifiers[format].push_back(modifier);
}

static void
dmabuf_format(void *data, struct zwp_linux_dmabuf_v1 *, uint32_t format)
{
    struct display *d = (struct display*)data;

    ++d->formats_count;
    d->formats = (uint32_t*)realloc(d->formats,
                    d->formats_count * sizeof(*d->formats));
    d->formats[d->formats_count - 1] = format;
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
    struct display *d = (struct display *)data;
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
    struct display *d = (struct display*)data;
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
    struct display* display = (struct display*)data;
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
    display->tablet_tools_evt[tool] = evt_code;
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
    struct display* display = (struct display*)data;
    struct input_event event[2];
    struct timespec rt;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TABLET))
        return;

    display->tablet_surface = surface;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }

    ADD_EVENT(EV_KEY, display->tablet_tools_evt[tool], 1);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_TABLET], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
tablet_tool_proximity_out(void *data, struct zwp_tablet_tool_v2 *tool)
{
    struct display* display = (struct display*)data;
    struct input_event event[2];
    struct timespec rt;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TABLET))
        return;

    display->tablet_surface = NULL;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }

    ADD_EVENT(EV_KEY, display->tablet_tools_evt[tool], 0);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_TABLET], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
tablet_tool_down(void *data, struct zwp_tablet_tool_v2 *, uint32_t)
{
    struct display* display = (struct display*)data;
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
    struct display* display = (struct display*)data;
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
    struct display* display = (struct display*)data;
    struct input_event event[3];
    struct timespec rt;
    int x, y;
    unsigned int res, n = 0;

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
    x += display->layers[display->tablet_surface].x;
    y += display->layers[display->tablet_surface].y;

    ADD_EVENT(EV_ABS, ABS_X, x);
    ADD_EVENT(EV_ABS, ABS_Y, y);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_TABLET], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
tablet_tool_pressure(void *data, struct zwp_tablet_tool_v2 *,
                     uint32_t pressure)
{
    struct display* display = (struct display*)data;
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
    struct display* display = (struct display*)data;
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
    struct display* display = (struct display*)data;
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
    struct display* display = (struct display*)data;
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
    struct display *d = (struct display*)data;
    d->tablet_tools.push_back(tool);
    zwp_tablet_tool_v2_add_listener(tool, &tablet_tool_listener, d);
    ALOGI("Added tablet tool");
}

static const struct zwp_tablet_seat_v2_listener tablet_seat_listener = {
    tablet_seat_handle_add_tablet,
    tablet_seat_handle_add_tool,
    tablet_seat_handle_add_pad
};

static void add_tablet_seat(struct display *d) {
    d->input_fd[INPUT_TABLET] = -1;
    mkfifo(INPUT_PIPE_NAME[INPUT_TABLET], S_IRWXO | S_IRWXG | S_IRWXU);
    chown(INPUT_PIPE_NAME[INPUT_TABLET], 1000, 1000);

    d->tablet_seat = zwp_tablet_manager_v2_get_tablet_seat(d->tablet_manager, d->seat);
    zwp_tablet_seat_v2_add_listener(d->tablet_seat, &tablet_seat_listener, d);
}

static void
registry_handle_global(void *data, struct wl_registry *registry,
               uint32_t id, const char *interface, uint32_t version)
{
    struct display *d = (struct display*)data;

    if (strcmp(interface, "wl_compositor") == 0) {
        d->compositor =
            (struct wl_compositor*)wl_registry_bind(registry,
                id, &wl_compositor_interface, std::min(version, 5U));
    } else if (strcmp(interface, "wl_subcompositor") == 0) {
        d->subcompositor =
        (struct wl_subcompositor*)wl_registry_bind(registry,
                id, &wl_subcompositor_interface, 1);
    } else if (strcmp(interface, "xdg_wm_base") == 0) {
        d->wm_base = (struct xdg_wm_base*)wl_registry_bind(registry,
                id, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(d->wm_base, &xdg_wm_base_listener, d);
    } else if(strcmp(interface, "wl_shell") == 0) {
        d->shell = (struct wl_shell *)wl_registry_bind(
                registry, id, &wl_shell_interface, 1);
    } else if (strcmp(interface, "wl_seat") == 0) {
        d->seat = (struct wl_seat*)wl_registry_bind(registry, id,
                &wl_seat_interface, std::min(version, (uint32_t)WL_POINTER_AXIS_SOURCE_SINCE_VERSION));
        wl_seat_add_listener(d->seat, &seat_listener, d);
        if (d->tablet_manager && !d->tablet_seat)
            add_tablet_seat(d);
        if (d->data_device_manager && !d->data_device)
            d->data_device = wl_data_device_manager_get_data_device(d->data_device_manager, d->seat);
    } else if (strcmp(interface, "wl_shm") == 0) {
		d->shm = (struct wl_shm *)wl_registry_bind(registry, id,
                &wl_shm_interface, 1);
    } else if (strcmp(interface, "wl_output") == 0) {
        d->output = (struct wl_output*)wl_registry_bind(registry, id,
                &wl_output_interface, std::min(version, 3U));
        wl_output_add_listener(d->output, &output_listener, d);
        wl_display_roundtrip(d->display);
    } else if (strcmp(interface, "wp_presentation") == 0) {
        bool no_presentation = property_get_bool("persist.waydroid.no_presentation", false);
        if (!no_presentation) {
            d->presentation = (struct wp_presentation*)wl_registry_bind(registry, id,
                    &wp_presentation_interface, 1);
            wp_presentation_add_listener(d->presentation,
                    &presentation_listener, d);
        }
    } else if (strcmp(interface, "wp_viewporter") == 0) {
        d->viewporter = (struct wp_viewporter*)wl_registry_bind(registry, id,
                &wp_viewporter_interface, 1);
    } else if ((d->gtype == GRALLOC_ANDROID) &&
               (strcmp(interface, "android_wlegl") == 0)) {
        d->android_wlegl = (struct android_wlegl*)wl_registry_bind(registry, id,
                &android_wlegl_interface, 1);
    } else if ((d->gtype == GRALLOC_GBM || d->gtype == GRALLOC_CROS) &&
               (strcmp(interface, "zwp_linux_dmabuf_v1") == 0)) {
        if (version < 3)
            return;
        d->dmabuf = (struct zwp_linux_dmabuf_v1*)wl_registry_bind(registry, id,
                &zwp_linux_dmabuf_v1_interface, 3);
        zwp_linux_dmabuf_v1_add_listener(d->dmabuf, &dmabuf_listener, d);
    } else if (strcmp(interface, "zwp_tablet_manager_v2") == 0) {
        d->tablet_manager = (struct zwp_tablet_manager_v2 *)wl_registry_bind(registry, id,
                &zwp_tablet_manager_v2_interface, 1);
        if (d->tablet_manager && d->seat)
            add_tablet_seat(d);
    } else if (strcmp(interface, "zwp_pointer_constraints_v1") == 0) {
        d->pointer_constraints = (struct zwp_pointer_constraints_v1 *)wl_registry_bind(
                registry, id, &zwp_pointer_constraints_v1_interface, 1);
    } else if (strcmp(interface, "zwp_relative_pointer_manager_v1") == 0) {
        d->relative_pointer_manager = (struct zwp_relative_pointer_manager_v1 *)wl_registry_bind(
                registry, id, &zwp_relative_pointer_manager_v1_interface, 1);
    } else if (strcmp(interface, "zwp_idle_inhibit_manager_v1") == 0) {
        d->idle_manager = (struct zwp_idle_inhibit_manager_v1 *)wl_registry_bind(
                registry, id, &zwp_idle_inhibit_manager_v1_interface, 1);
    } else if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0) {
        d->fractional_scale_manager = (struct wp_fractional_scale_manager_v1*)wl_registry_bind(registry, id,
                &wp_fractional_scale_manager_v1_interface, 1);
    } else if (strcmp(interface, wl_data_device_manager_interface.name) == 0) {
        d->data_device_manager = (struct wl_data_device_manager *)wl_registry_bind(registry, id,
                &wl_data_device_manager_interface, std::min(version,  3U));
        if (d->data_device_manager && d->seat)
            d->data_device = wl_data_device_manager_get_data_device(d->data_device_manager, d->seat);
    } else if (strcmp(interface, "gtk_shell1") == 0) {
        if (version < 6)
            d->supports_cursor_viewport = false;
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

int
get_gralloc_type(const char *gralloc)
{
    if (strcmp(gralloc, "default") == 0) {
        return GRALLOC_DEFAULT;
    } else if (strcmp(gralloc, "gbm") == 0) {
        return GRALLOC_GBM;
    } else if (str_starts_with(gralloc, "minigbm_") == 0) {
        return GRALLOC_CROS;
    } else {
        return GRALLOC_ANDROID;
    }
}

static void
wayland_log_handler (const char *format, va_list args)
{
    LOG_PRI_VA (ANDROID_LOG_ERROR, "wayland-hwc", format, args);
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
    display->supports_cursor_viewport = true;
    display->display = wl_display_connect(NULL);
    ALOGI("WAYLAND_DISPLAY: %s", getenv("WAYLAND_DISPLAY"));
    ALOGI("XDG_RUNTIME_DIR: %s", getenv("XDG_RUNTIME_DIR"));
    if (!display->display) {
        ALOGE("Couldn't open Wayland display.");
        return NULL;
    }
    sem_init(&display->egl_go, 0, 0);
    sem_init(&display->egl_done, 0, 0);

    umask(0);
    mkdir("/dev/input", S_IRWXO | S_IRWXG | S_IRWXU);
    chown("/dev/input", 1000, 1000);
    display->registry = wl_display_get_registry(display->display);
    wl_registry_add_listener(display->registry,
                 &registry_listener, display);
    wl_display_roundtrip(display->display);

    display->task = IWaydroidTask::getService();
    return display;
}

void
destroy_display(struct display *display)
{
    if (display->wm_base)
        xdg_wm_base_destroy(display->wm_base);

    if (display->shell)
        wl_shell_destroy(display->shell);

    if (display->compositor)
        wl_compositor_destroy(display->compositor);

    if (display->tablet_manager) {
        for (struct zwp_tablet_tool_v2 *t : display->tablet_tools) {
            zwp_tablet_tool_v2_destroy(t);
        }
        zwp_tablet_seat_v2_destroy(display->tablet_seat);
        zwp_tablet_manager_v2_destroy(display->tablet_manager);
    }

    if (display->relative_pointer_manager)
        zwp_relative_pointer_manager_v1_destroy(display->relative_pointer_manager);

    if (display->pointer_constraints)
        zwp_pointer_constraints_v1_destroy(display->pointer_constraints);

    wl_registry_destroy(display->registry);
    wl_display_flush(display->display);
    wl_display_disconnect(display->display);
    delete display;
}

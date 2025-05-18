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

#pragma once

#include <cutils/native_handle.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <stdbool.h>
#include <assert.h>
#include <unistd.h>
#include <sys/mman.h>
#include <signal.h>
#include <fcntl.h>
#include <getopt.h>
#include <errno.h>
#include <map>
#include <list>
#include <set>
#include <pthread.h>
#include <semaphore.h>
#include <hardware/hwcomposer.h>
#include <vendor/waydroid/task/1.0/IWaydroidTask.h>
#include <wayland-util.h>
#include <wayland-client.h>

#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <functional>
#include <unordered_map>

using ::android::sp;
using ::vendor::waydroid::task::V1_0::IWaydroidTask;

enum {
    INPUT_TOUCH,
    INPUT_KEYBOARD,
    INPUT_POINTER,
    INPUT_TABLET,
    INPUT_TOTAL
};

static const char *INPUT_PIPE_NAME[INPUT_TOTAL] = {
    "/dev/input/wl_touch_events",
    "/dev/input/wl_keyboard_events",
    "/dev/input/wl_pointer_events",
    "/dev/input/wl_tablet_events"
};

enum class GrallocType {
    GRALLOC_ANDROID,
    GRALLOC_GBM,
    GRALLOC_CROS,
    GRALLOC_DEFAULT
};

#define MAX_TOUCHPOINTS 10

struct layerFrame {
    int x;
    int y;
};

struct handleExt {
    uint32_t format;
    uint32_t stride;
    uint32_t width;
    uint32_t height;
};

struct window;
struct buffer;
struct display;
struct waydroid_hwc_composer_device_1;

struct cursor_handler {
    virtual ~cursor_handler() = default;
    virtual int apply_cursor(waydroid_hwc_composer_device_1 *pdev, hwc_layer_1 *hwc_layer, size_t hwc_layer_index) = 0;
    virtual int reset_cursor(waydroid_hwc_composer_device_1 *pdev __unused) { return 0; }
    virtual int cursor_enter(display *display __unused) { return 0; };
};

struct display {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_subcompositor *subcompositor;
    struct wl_seat *seat;
    struct wl_shell *shell;
    struct wl_shm *shm;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    struct wl_touch *touch;
    struct wl_output *output;
    struct wp_presentation *presentation;
    struct wp_viewporter *viewporter;
    struct android_wlegl *android_wlegl;
    struct zwp_linux_dmabuf_v1 *dmabuf;
    struct xdg_wm_base *wm_base;
    struct zwp_tablet_manager_v2* tablet_manager;
    struct zwp_tablet_seat_v2 *tablet_seat;
    struct zwp_pointer_constraints_v1 *pointer_constraints;
    struct zwp_relative_pointer_manager_v1 *relative_pointer_manager;
    struct zwp_relative_pointer_v1 *relative_pointer;
    struct zwp_idle_inhibit_manager_v1 *idle_manager;
    struct wp_fractional_scale_manager_v1 *fractional_scale_manager;
    struct wl_data_device_manager *data_device_manager;
    struct wl_data_device *data_device;

    int system_version;
    GrallocType gtype;
    double scale;

    int input_fd[INPUT_TOTAL];
    int ptrPrvX;
    int ptrPrvY;
    double wheelAccumulatorX;
    double wheelAccumulatorY;
    bool wheelEvtIsDiscrete;
    bool reverseScroll;
    int touch_id[MAX_TOUCHPOINTS];
    std::map<struct wl_surface *, struct layerFrame> layers;

    std::map<std::string, std::unique_ptr<window>> windows;
    std::set<std::string> ignored_apps;
    std::mutex windowsMutex;

    std::map<int, struct wl_surface *> touch_surfaces;
    struct wl_surface *pointer_surface;
    struct wl_surface *tablet_surface;
    std::list<struct zwp_tablet_tool_v2 *> tablet_tools;
    std::map<struct zwp_tablet_tool_v2 *, uint16_t> tablet_tools_evt;
    uint32_t keyboard_enter_serial;
    std::string clipboard;
    std::list<std::string> clipboard_offer_mime_types;
    uint32_t pointer_enter_serial;
    struct {float x; float y;} cursor_hotspot;

    EGLDisplay egl_dpy;
    std::list<std::function<void()>> egl_work_queue;
    sem_t egl_go;
    sem_t egl_done;

    int width;
    int height;
    int full_width;
    int full_height;
    int refresh;

    //TODO: Replace array with set
    uint32_t *formats;
    int formats_count;

    std::map<uint32_t, std::vector<uint64_t>> modifiers;
    std::map<uint32_t, std::string> layer_names;
    std::map<uint32_t, struct handleExt> layer_handles_ext;
    struct handleExt target_layer_handle_ext;
    std::unordered_map<buffer_handle_t, std::unique_ptr<buffer>> buffer_map;
    std::array<uint8_t, 239> keysDown;

    std::unique_ptr<cursor_handler> cursor_handler;
    bool supports_cursor_viewport;

    bool isMaximized;
    sp<IWaydroidTask> task;
};

struct buffer_metadata {
    uint32_t height;
    uint32_t width;
    uint32_t pixel_stride;
    uint32_t format;
};
constexpr bool operator==(const buffer_metadata &lhs, const buffer_metadata &rhs) {
    return lhs.height == rhs.height
           && lhs.width == rhs.width
           && lhs.pixel_stride == rhs.pixel_stride
           && lhs.format == rhs.format;
}
constexpr bool operator!=(const buffer_metadata &lhs, const buffer_metadata &rhs) {
    return lhs.height != rhs.height
           || lhs.width != rhs.width
           || lhs.pixel_stride != rhs.pixel_stride
           || lhs.format != rhs.format;
}

struct buffer {
    struct wl_buffer *wl_buffer;

    buffer_handle_t handle;
    buffer_metadata metadata;

    bool isShm;
    void *shm_data;
    int size;

    ~buffer();
};

enum class BufferTransform : int32_t {
    Normal = WL_OUTPUT_TRANSFORM_NORMAL,
    Rot_90 = WL_OUTPUT_TRANSFORM_90,
    Rot_180 = WL_OUTPUT_TRANSFORM_180,
    Rot_270 = WL_OUTPUT_TRANSFORM_270,
    Flip = WL_OUTPUT_TRANSFORM_FLIPPED,
    Flip_Rot_90 = WL_OUTPUT_TRANSFORM_FLIPPED_90,
    Flip_Rot_180 = WL_OUTPUT_TRANSFORM_FLIPPED_180,
    Flip_Rot_270 = WL_OUTPUT_TRANSFORM_FLIPPED_270,
};

BufferTransform hwc_transform_to_buffer_transform(uint32_t hwc_transform);

struct surface_context {
    struct wl_surface *surface;
    struct wp_viewport *viewport;

    surface_context() = default;
    surface_context(wl_surface *surface, wp_viewport *viewport);
    ~surface_context();

    surface_context(surface_context &&other);
    surface_context &operator=(surface_context &&rhs);

    void attach_buffer(buffer& buf);
    void damage_surface(int32_t x, int32_t y, int32_t width, int32_t height);
    void set_buffer_transform(BufferTransform transform);
    void set_buffer_scale(double scale);
    // Requires the transformed rectangle
    void set_crop(hwc_frect_t crop);
    // TODO: This should not require scale. Scaling handling is broken
    void set_display_frame(hwc_rect_t rect, double scale);
};

struct window {
    struct layer : public surface_context {
        struct wl_subsurface *subsurface;

        layer() = default;
        layer(wl_surface *surface, wp_viewport *viewport, wl_subsurface *subsurface = nullptr);
        ~layer();

        layer(layer &&other);
        layer &operator=(layer &&rhs);

        void set_position(int32_t x, int32_t y);
    };

    struct display *display;

    struct wl_shell_surface *shell_surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;

    /* Used for the background color */
    bool destroy_background_objects;
    struct wl_surface *surface;
    struct wp_viewport *viewport;
    struct wl_buffer *bg_buffer;

    struct wl_region* input_region;

    struct zwp_locked_pointer_v1 *locked_pointer;
    struct zwp_idle_inhibitor_v1 *idle_inhibitor;

    std::vector<layer> layers;

    struct buffer *last_layer_buffer;
    std::unique_ptr<buffer> snapshot_buffer;

    int lastLayer;

    std::string appID;
    std::string taskID;

    ~window();

    static std::unique_ptr<window> create(struct display *display, bool with_dummy, std::string appID, std::string taskID, hwc_color_t color);

    window::layer &create_new_layer();

  private:
    window() = default;
};

void
handle_relative_motion(void *data, struct zwp_relative_pointer_v1*,
        uint32_t, uint32_t, wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t, wl_fixed_t);

void
snapshot_inactive_app_window(struct display *display, struct window *window);

struct display *
create_display(const char* gralloc);
void
destroy_display(struct display *display);

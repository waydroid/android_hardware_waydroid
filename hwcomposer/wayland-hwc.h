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
#include <pthread.h>
#include <semaphore.h>
#include <hardware/hwcomposer.h>
#include <vendor/waydroid/task/1.0/IWaydroidTask.h>
#include <wayland-util.h>

#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <functional>

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

enum {
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
    int system_version;
    int gtype;
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
    std::map<struct wl_surface *, struct window *> windows;
    std::mutex windowsMutex;
    std::map<int, struct wl_surface *> touch_surfaces;
    struct wl_surface *pointer_surface;
    struct wl_surface *cursor_surface;
    struct wp_viewport *cursor_viewport;
    struct wl_surface *tablet_surface;
    std::list<struct zwp_tablet_tool_v2 *> tablet_tools;
    std::map<struct zwp_tablet_tool_v2 *, uint16_t> tablet_tools_evt;

    EGLDisplay egl_dpy;
    std::list<std::function<void()>> egl_work_queue;
    sem_t egl_go;
    sem_t egl_done;

    int width;
    int height;
    int full_width;
    int full_height;
    int refresh;
    uint32_t *formats;
    int formats_count;
    std::map<uint32_t, std::vector<uint64_t>> modifiers;
    bool geo_changed;
    std::map<uint32_t, std::string> layer_names;
    std::map<uint32_t, struct handleExt> layer_handles_ext;
    struct handleExt target_layer_handle_ext;
    std::map<buffer_handle_t, struct buffer *> buffer_map;
    std::array<uint8_t, 239> keysDown;

    bool isMaximized;
    sp<IWaydroidTask> task;
};

struct buffer {
    struct wl_buffer *buffer;
    struct wp_presentation_feedback *feedback;

    buffer_handle_t handle;
    int width;
    int height;
    unsigned long pixel_stride;
    int format;
    uint32_t hal_format;

    bool isShm;
    void *shm_data;
    int size;
};

struct window {
    struct display *display;
    struct wl_surface *surface;
    struct wp_viewport *viewport;
    struct wl_shell_surface *shell_surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;
    struct wp_viewport *bg_viewport;
    struct wl_buffer *bg_buffer;
    struct wl_surface *bg_surface;
    struct wl_subsurface *bg_subsurface;
    struct wl_region* input_region;
    struct zwp_locked_pointer_v1 *locked_pointer;
    struct zwp_idle_inhibitor_v1 *idle_inhibitor;
    std::map<size_t, struct wl_surface *> surfaces;
    std::map<size_t, struct wl_subsurface *> subsurfaces;
    std::map<size_t, struct wp_viewport *> viewports;
    struct wl_callback *callback;
    struct buffer *last_layer_buffer;
    struct buffer *snapshot_buffer;
    int lastLayer;
    std::string appID;
    std::string taskID;
    bool isActive;
};

void
handle_relative_motion(void *data, struct zwp_relative_pointer_v1*,
        uint32_t, uint32_t, wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t, wl_fixed_t);

void
destroy_buffer(struct buffer* buf);

int
create_android_wl_buffer(struct display *display, struct buffer *buffer,
             int width, int height, int format,
             int pixel_stride, buffer_handle_t target);

int
create_dmabuf_wl_buffer(struct display *display, struct buffer *buffer,
             int width, int height, int hal_format, int format,
             int prime_fd, int pixel_stride, int byte_stride,
             int offset, uint64_t modifier, buffer_handle_t target);

int
create_shm_wl_buffer(struct display *display, struct buffer *buffer,
             int width, int height, int format, int pixel_stride, buffer_handle_t target);

void
snapshot_inactive_app_window(struct display *display, struct window *window);

struct display *
create_display(const char* gralloc);
void
destroy_display(struct display *display);

void
destroy_window(struct window *window, bool keep = false);
struct window *
create_window(struct display *display, bool with_dummy, std::string appID, std::string taskID, hwc_color_t color);
void
choose_width_height(struct display* display, int32_t hint_width, int32_t hint_height);

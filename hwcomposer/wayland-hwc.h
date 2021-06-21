/*
 * Copyright © 2011 Benjamin Franzke
 * Copyright © 2010 Intel Corporation
 * Copyright © 2014 Collabora Ltd.
 * Copyright © 2021 Anbox Project.
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

#include <cutils/native_handle.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <unistd.h>
#include <sys/mman.h>
#include <signal.h>
#include <fcntl.h>
#include <getopt.h>
#include <errno.h>
#include <map>

enum {
    INPUT_TOUCH,
    INPUT_KEYBOARD,
    INPUT_POINTER,
    INPUT_TOTAL
};

static const char *INPUT_PIPE_NAME[INPUT_TOTAL] = {
    "/dev/input/wl_touch_events",
    "/dev/input/wl_keyboard_events",
    "/dev/input/wl_pointer_events"
};

enum {
    GRALLOC_ANDROID,
    GRALLOC_GBM,
    GRALLOC_DEFAULT
};

#define MAX_TOUCHPOINTS 10

struct display {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_subcompositor *subcompositor;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    struct wl_touch *touch;
    struct wl_output *output;
    struct wp_presentation *presentation;
    struct android_wlegl *android_wlegl;
    struct zwp_linux_dmabuf_v1 *dmabuf;
    struct xdg_wm_base *wm_base;
    int gtype;
    int scale;

    int input_fd[INPUT_TOTAL];
    int ptrPrvX;
    int ptrPrvY;
    int touch_id[MAX_TOUCHPOINTS];

    int width;
    int height;
    uint32_t *formats;
    int formats_count;
};

struct buffer {
    struct wl_buffer *buffer;
    int busy;
    struct wp_presentation_feedback *feedback;

    buffer_handle_t handle;
    int width;
    int height;
    unsigned long stride;
    int format;

    int timeline_fd;
    int release_fence_fd;
};

struct window {
    struct display *display;
    int width, height;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;
    std::map<buffer_handle_t, struct buffer *> buffer_map;
    struct wl_callback *callback;
};

int
create_android_wl_buffer(struct display *display, struct buffer *buffer,
             int width, int height, int format,
             int stride, buffer_handle_t target);

int
create_dmabuf_wl_buffer(struct display *display, struct buffer *buffer,
             int width, int height, int format,
             int prime_fd, int stride, uint64_t modifier);

struct display *
create_display(const char* gralloc);
void
destroy_display(struct display *display);

struct window *
create_window(struct display *display, int width, int height);

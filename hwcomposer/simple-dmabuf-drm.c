/*
 * Copyright © 2011 Benjamin Franzke
 * Copyright © 2010 Intel Corporation
 * Copyright © 2014 Collabora Ltd.
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

#include "simple-dmabuf-drm.h"

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

#include <xf86drm.h>

#include <drm_fourcc.h>

#include <libsync/sw_sync.h>
#include <sync/sync.h>
#include <hardware/gralloc.h>

#include <wayland-client.h>
#include <wayland-android-client-protocol.h>
#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "presentation-time-client-protocol.h"

#ifndef DRM_FORMAT_MOD_LINEAR
#define DRM_FORMAT_MOD_LINEAR 0
#endif

struct buffer;

#define ALIGN(v, a) ((v + a - 1) & ~(a - 1))

static void
redraw(void *data, struct wl_callback *callback, uint32_t time);

#include <log/log.h>

#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#include <cutils/trace.h>

static void
buffer_release(void *data, struct wl_buffer *buffer)
{
	struct buffer *mybuf = data;

	//ALOGE("*** %s: Signaling release fence for buffer %p with FD %d fence %d", __func__, mybuf, mybuf->dmabuf_fd, mybuf->release_fence_fd);
	sw_sync_timeline_inc(mybuf->timeline_fd, 1);
	close(mybuf->release_fence_fd);
	mybuf->release_fence_fd = -1;
}

static const struct wl_buffer_listener buffer_listener = {
	buffer_release
};

void shell_surface_ping(void *data, struct wl_shell_surface *shell_surface, uint32_t serial) {
    ALOGW("shell surface ping");
    wl_shell_surface_pong(shell_surface, serial);
}

void shell_surface_configure(void *data, struct wl_shell_surface *shell_surface, uint32_t edges, int32_t width, int32_t height) {
    ALOGW("shell surface configure");
}

void shell_surface_popup_done(void *data, struct wl_shell_surface *shell_surface) {
    ALOGW("shell surface popup done");
}

struct wl_shell_surface_listener shell_surface_listener = {
	&shell_surface_ping,
	&shell_surface_configure,
	&shell_surface_popup_done
};

int
create_dmabuf_buffer(struct display *display, struct buffer *buffer,
		     int width, int height, int format, uint32_t opts,
		     int stride, buffer_handle_t target)
{
	struct android_wlegl_handle *wlegl_handle;
	struct wl_array ints;
	int *the_ints;

	buffer->width = width;
	buffer->height = height;
	buffer->bpp = 32;
	buffer->format = format;

	buffer->handle = target;
	buffer->stride = stride;

	wl_array_init(&ints);
	the_ints = (int *)wl_array_add(&ints, target->numInts * sizeof(int));
	memcpy(the_ints, target->data + target->numFds, target->numInts * sizeof(int));
	wlegl_handle = android_wlegl_create_handle(display->android_wlegl, target->numFds, &ints);
	wl_array_release(&ints);

	for (int i = 0; i < target->numFds; i++) {
		android_wlegl_handle_add_fd(wlegl_handle, target->data[i]);
	}

	buffer->buffer = android_wlegl_create_buffer(display->android_wlegl, buffer->width, buffer->height, buffer->stride, buffer->format, GRALLOC_USAGE_HW_RENDER, wlegl_handle);
	android_wlegl_handle_destroy(wlegl_handle);

	wl_buffer_add_listener(buffer->buffer, &buffer_listener, buffer);

	return 0;
}

struct window *
create_window(struct display *display, int width, int height)
{
	struct window *window;

	window = calloc(1, sizeof *window);
	if (!window)
		return NULL;

	window->callback = NULL;
	window->display = display;
	window->width = width;
	window->height = height;
	window->surface = wl_compositor_create_surface(display->compositor);

	if (display->shell) {
		window->shell_surface =
			wl_shell_get_shell_surface(display->shell,
									   window->surface);

		assert(window->shell_surface);
		wl_shell_surface_add_listener(window->shell_surface, &shell_surface_listener, NULL);

		wl_shell_surface_set_toplevel(window->shell_surface);
	} else {
		assert(0);
	}
	return window;
}

static void
seat_handle_capabilities(void *data, struct wl_seat *seat,
			 enum wl_seat_capability caps)
{
	struct display *d = data;

	if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !d->touch) {
		d->touch = wl_seat_get_touch(seat);
		wl_touch_set_user_data(d->touch, d);
		wl_touch_add_listener(d->touch, d->touch_listener, d->touch_data);
	} else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && d->touch) {
		wl_touch_destroy(d->touch);
		d->touch = NULL;
	}
}

static void
seat_handle_name(void *data, struct wl_seat *seat,
				 const char *name)
{
	ALOGW("seat name: %s\n", name);
}

static const struct wl_seat_listener seat_listener = {
	seat_handle_capabilities,
	seat_handle_name,
};

static void
output_handle_mode(void *data, struct wl_output *wl_output,
		   uint32_t flags, int32_t width, int32_t height,
		   int32_t refresh)
{
	struct display *d = data;

	d->width = width;
	d->height = height;
}

static void
output_handle_geometry(void *data, struct wl_output *wl_output,
		       int32_t x, int32_t y,
		       int32_t physical_width, int32_t physical_height,
		       int32_t subpixel,
		       const char *make, const char *model,
		       int32_t output_transform)
{
}

static void
output_handle_done(void *data, struct wl_output *wl_output)
{
}

static void
output_handle_scale(void *data, struct wl_output *wl_output,
		    int32_t scale)
{
}

static const struct wl_output_listener output_listener = {
	output_handle_geometry,
	output_handle_mode,
	output_handle_done,
	output_handle_scale,
};

static void
presentation_clock_id(void *data, struct wp_presentation *presentation,
		      uint32_t clk_id)
{
        ALOGE("*** %s: clk_id %d CLOCK_MONOTONIC %d", __func__, clk_id, CLOCK_MONOTONIC);
}

static const struct wp_presentation_listener presentation_listener = {
	presentation_clock_id
};

static void
registry_handle_global(void *data, struct wl_registry *registry,
		       uint32_t id, const char *interface, uint32_t version)
{
	struct display *d = data;

	if (strcmp(interface, "wl_compositor") == 0) {
		d->compositor =
			wl_registry_bind(registry,
					 id, &wl_compositor_interface, version);
	} else if (strcmp(interface, "wl_subcompositor") == 0) {
		d->subcompositor =
			wl_registry_bind(registry,
					 id, &wl_subcompositor_interface, 1);
	} else if(strcmp(interface, "wl_shell") == 0) {
		d->shell = (struct wl_shell *)wl_registry_bind(
			           registry, id, &wl_shell_interface, 1);
	} else if (strcmp(interface, "wl_seat") == 0) {
		d->seat = wl_registry_bind(registry, id,
					   &wl_seat_interface, 1);
		wl_seat_add_listener(d->seat, &seat_listener, d);
	} else if (strcmp(interface, "wl_output") == 0) {
		d->output = wl_registry_bind(registry, id,
					   &wl_output_interface, 1);
		wl_output_add_listener(d->output, &output_listener, d);
	} else if (strcmp(interface, "wp_presentation") == 0) {
		d->presentation = wl_registry_bind(registry, id,
					   &wp_presentation_interface, 1);
		wp_presentation_add_listener(d->presentation,
					     &presentation_listener, d);
	} else if(strcmp(interface, "android_wlegl") == 0) {
		d->android_wlegl = wl_registry_bind(registry, id,
						&android_wlegl_interface, 1);
	}
}

static void
registry_handle_global_remove(void *data, struct wl_registry *registry,
			      uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	registry_handle_global,
	registry_handle_global_remove
};

struct display *
create_display(const struct wl_touch_listener *touch_listener, void *touch_data)
{
	struct display *display;

	display = malloc(sizeof *display);
	if (display == NULL) {
		fprintf(stderr, "out of memory\n");
		return NULL;
	}
	display->display = wl_display_connect(NULL);
	assert(display->display);

	display->touch_listener = touch_listener;
	display->touch_data = touch_data;

	display->registry = wl_display_get_registry(display->display);
	wl_registry_add_listener(display->registry,
				 &registry_listener, display);
	wl_display_roundtrip(display->display);

	return display;
}

void
destroy_display(struct display *display)
{
	if (display->shell)
		wl_shell_destroy(display->shell);

	if (display->compositor)
		wl_compositor_destroy(display->compositor);

	wl_registry_destroy(display->registry);
	wl_display_flush(display->display);
	wl_display_disconnect(display->display);
	free(display);
}

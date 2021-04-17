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
#include "simple-dmabuf-drm-data.h"

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

#ifdef HAVE_LIBDRM_INTEL
#include <i915_drm.h>
#include <intel_bufmgr.h>
#endif
#ifdef HAVE_LIBDRM_FREEDRENO
#include <freedreno/freedreno_drmif.h>
#endif
#ifdef HAVE_LIBDRM_ETNAVIV
#include <etnaviv_drmif.h>
#endif
#include <drm_fourcc.h>

#include <libsync/sw_sync.h>
#include <sync/sync.h>

#include <wayland-client.h>
#include "xdg-shell-unstable-v6-client-protocol.h"
#include "fullscreen-shell-unstable-v1-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "presentation-time-client-protocol.h"

#ifndef DRM_FORMAT_MOD_LINEAR
#define DRM_FORMAT_MOD_LINEAR 0
#endif

struct buffer;

/* Possible options that affect the displayed image */
#define OPT_Y_INVERTED 1  /* contents has y axis inverted */
#define OPT_IMMEDIATE  2  /* create wl_buffer immediately */

#define ALIGN(v, a) ((v + a - 1) & ~(a - 1))

static int running = 1;

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
   mybuf->busy = false;
   sw_sync_timeline_inc(mybuf->timeline_fd, 1);
   close(mybuf->release_fence_fd);
   mybuf->release_fence_fd = -1;
}

static const struct wl_buffer_listener buffer_listener = {
	buffer_release
};


#ifdef HAVE_LIBDRM_INTEL
static int
intel_alloc_bo(struct buffer *my_buf)
{
	/* XXX: try different tiling modes for testing FB modifiers. */
	uint32_t tiling = I915_TILING_NONE;

	assert(my_buf->bufmgr);

	my_buf->intel_bo = drm_intel_bo_alloc_tiled(my_buf->bufmgr, "test",
						    my_buf->width, my_buf->height,
						    (my_buf->bpp / 8), &tiling,
						    &my_buf->stride, 0);

	printf("buffer allocated w %d, h %d, stride %lu, size %lu\n",
	       my_buf->width, my_buf->height, my_buf->stride, my_buf->intel_bo->size);

	if (!my_buf->intel_bo)
		return 0;

	if (tiling != I915_TILING_NONE)
		return 0;

	return 1;
}

static void
intel_free_bo(struct buffer *my_buf)
{
	drm_intel_bo_unreference(my_buf->intel_bo);
}

static int
intel_map_bo(struct buffer *my_buf)
{
	if (drm_intel_gem_bo_map_gtt(my_buf->intel_bo) != 0)
		return 0;

	my_buf->mmap = my_buf->intel_bo->virtual;

	return 1;
}

static int
intel_bo_export_to_prime(struct buffer *buffer)
{
	return drm_intel_bo_gem_export_to_prime(buffer->intel_bo, &buffer->dmabuf_fd);
}

static void
intel_unmap_bo(struct buffer *my_buf)
{
	drm_intel_gem_bo_unmap_gtt(my_buf->intel_bo);
}

static void
intel_device_destroy(struct buffer *my_buf)
{
	drm_intel_bufmgr_destroy(my_buf->bufmgr);
}

#endif /* HAVE_LIBDRM_INTEL */
#ifdef HAVE_LIBDRM_FREEDRENO

static int
fd_alloc_bo(struct buffer *buf)
{
	int flags = DRM_FREEDRENO_GEM_CACHE_WCOMBINE;
	int size;

	buf->fd_dev = fd_device_new(buf->drm_fd);
	buf->stride = ALIGN(buf->width, 32) * buf->bpp / 8;
	size = buf->stride * buf->height;
	buf->fd_dev = fd_device_new(buf->drm_fd);
	buf->fd_bo = fd_bo_new(buf->fd_dev, size, flags);

	if (!buf->fd_bo)
		return 0;
	return 1;
}

static void
fd_free_bo(struct buffer *buf)
{
	fd_bo_del(buf->fd_bo);
}

static int
fd_bo_export_to_prime(struct buffer *buf)
{
	buf->dmabuf_fd = fd_bo_dmabuf(buf->fd_bo);
	return buf->dmabuf_fd < 0;
}

static int
fd_map_bo(struct buffer *buf)
{
	buf->mmap = fd_bo_map(buf->fd_bo);
	return buf->mmap != NULL;
}

static void
fd_unmap_bo(struct buffer *buf)
{
}

static void
fd_device_destroy(struct buffer *buf)
{
	fd_device_del(buf->fd_dev);
}
#endif /* HAVE_LIBDRM_FREEDRENO */
#ifdef HAVE_LIBDRM_ETNAVIV

static int
etna_alloc_bo(struct buffer *buf)
{
	int flags = DRM_ETNA_GEM_CACHE_WC;
	int size;

	buf->stride = ALIGN(buf->width, 32) * buf->bpp / 8;
	size = 	buf->stride * buf->height;
	buf->etna_dev = etna_device_new(buf->drm_fd);
	buf->etna_bo = etna_bo_new(buf->etna_dev, size, flags);

	return buf->etna_bo != NULL;
}

static void
etna_free_bo(struct buffer *buf)
{
	etna_bo_del(buf->etna_bo);
}

static int
etna_bo_export_to_prime(struct buffer *buf)
{
	buf->dmabuf_fd = etna_bo_dmabuf(buf->etna_bo);
	return buf->dmabuf_fd < 0;
}

static int
etna_map_bo(struct buffer *buf)
{
	buf->mmap = etna_bo_map(buf->etna_bo);
	return buf->mmap != NULL;
}

static void
etna_unmap_bo(struct buffer *buf)
{
	if (munmap(buf->mmap, buf->stride * buf->height) < 0)
		fprintf(stderr, "Failed to unmap buffer: %s", strerror(errno));
	buf->mmap = NULL;
}

static void
etna_device_destroy(struct buffer *buf)
{
	etna_device_del(buf->etna_dev);
}
#endif /* HAVE_LIBDRM_ENTAVIV */

static void
drm_device_destroy(struct buffer *buf)
{
	buf->dev->device_destroy(buf);
	close(buf->drm_fd);
}

static int
drm_device_init(struct buffer *buf)
{
	struct drm_device *dev = calloc(1, sizeof(struct drm_device));

	drmVersionPtr version = drmGetVersion(buf->drm_fd);

	dev->fd = buf->drm_fd;
	dev->name = strdup(version->name);
	if (0) {
		/* nothing */
	}
#ifdef HAVE_LIBDRM_INTEL
	else if (!strcmp(dev->name, "i915")) {
		buf->bufmgr = drm_intel_bufmgr_gem_init(buf->drm_fd, 32);
		if (!buf->bufmgr)
			return 0;
		dev->alloc_bo = intel_alloc_bo;
		dev->free_bo = intel_free_bo;
		dev->export_bo_to_prime = intel_bo_export_to_prime;
		dev->map_bo = intel_map_bo;
		dev->unmap_bo = intel_unmap_bo;
		dev->device_destroy = intel_device_destroy;
	}
#endif
#ifdef HAVE_LIBDRM_FREEDRENO
	else if (!strcmp(dev->name, "msm")) {
		dev->alloc_bo = fd_alloc_bo;
		dev->free_bo = fd_free_bo;
		dev->export_bo_to_prime = fd_bo_export_to_prime;
		dev->map_bo = fd_map_bo;
		dev->unmap_bo = fd_unmap_bo;
		dev->device_destroy = fd_device_destroy;
	}
#endif
#ifdef HAVE_LIBDRM_ETNAVIV
	else if (!strcmp(dev->name, "etnaviv")) {
		dev->alloc_bo = etna_alloc_bo;
		dev->free_bo = etna_free_bo;
		dev->export_bo_to_prime = etna_bo_export_to_prime;
		dev->map_bo = etna_map_bo;
		dev->unmap_bo = etna_unmap_bo;
		dev->device_destroy = etna_device_destroy;
	}
#endif
	else {
		fprintf(stderr, "Error: drm device %s unsupported.\n",
			dev->name);
		free(dev);
		return 0;
	}
	buf->dev = dev;
	return 1;
}

static int
drm_connect(struct buffer *my_buf)
{
	/* This won't work with card0 as we need to be authenticated; instead,
	 * boot with drm.rnodes=1 and use that. */
	my_buf->drm_fd = open("/dev/dri/renderD128", O_RDWR);
	if (my_buf->drm_fd < 0)
		return 0;

	return drm_device_init(my_buf);
}

static void
drm_shutdown(struct buffer *my_buf)
{
	drm_device_destroy(my_buf);
}


static void
create_succeeded(void *data,
		 struct zwp_linux_buffer_params_v1 *params,
		 struct wl_buffer *new_buffer)
{
	struct buffer *buffer = data;

	buffer->buffer = new_buffer;
	wl_buffer_add_listener(buffer->buffer, &buffer_listener, buffer);

	zwp_linux_buffer_params_v1_destroy(params);
}

static void
create_failed(void *data, struct zwp_linux_buffer_params_v1 *params)
{
	struct buffer *buffer = data;

	buffer->buffer = NULL;
	running = 0;

	zwp_linux_buffer_params_v1_destroy(params);

	fprintf(stderr, "Error: zwp_linux_buffer_params.create failed.\n");
}

static const struct zwp_linux_buffer_params_v1_listener params_listener = {
	create_succeeded,
	create_failed
};

int
create_dmabuf_buffer(struct display *display, struct buffer *buffer,
		     int width, int height, int format, uint32_t opts,
		     int prime_fd, int stride, uint64_t modifier)
{
	struct zwp_linux_buffer_params_v1 *params;
	uint32_t flags = 0;
	struct drm_device *drm_dev;

	if (!drm_connect(buffer)) {
		fprintf(stderr, "drm_connect failed\n");
		goto error;
	}
	drm_dev = buffer->dev;

	buffer->width = width;
	switch (format) {
	case DRM_FORMAT_NV12:
		/* adjust height for allocation of NV12 Y and UV planes */
		buffer->height = height * 3 / 2;
		buffer->bpp = 8;
		modifier = display->nv12_modifier;
		break;
	default:
		buffer->height = height;
		buffer->bpp = 32;
	}
	buffer->format = format;

   assert(prime_fd >= 0);
   buffer->dmabuf_fd = prime_fd;
	buffer->stride = stride;

	/* We now have a dmabuf! For format XRGB8888, it should contain 2x2
	 * tiles (i.e. each tile is 256x256) of misc colours, and be mappable,
	 * either as ARGB8888, or XRGB8888. For format NV12, it should contain
	 * the Y and UV components, and needs to be re-adjusted for passing the
	 * correct height to the compositor.
	 */
	buffer->height = height;
	if (opts & OPT_Y_INVERTED)
		flags |= ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_Y_INVERT;

	params = zwp_linux_dmabuf_v1_create_params(display->dmabuf);
	zwp_linux_buffer_params_v1_add(params,
				       buffer->dmabuf_fd,
				       0, /* plane_idx */
				       0, /* offset */
				       buffer->stride,
				       modifier >> 32,
				       modifier & 0xffffffff);

	if (format == DRM_FORMAT_NV12) {
		/* add the second plane params */
		zwp_linux_buffer_params_v1_add(params,
					       buffer->dmabuf_fd,
					       1,
					       buffer->width * buffer->height,
					       buffer->stride,
					       modifier >> 32,
					       modifier & 0xffffffff);
	}
	zwp_linux_buffer_params_v1_add_listener(params, &params_listener, buffer);
	if (display->req_dmabuf_immediate) {
		buffer->buffer = zwp_linux_buffer_params_v1_create_immed(params,
					  buffer->width,
					  buffer->height,
					  format,
					  flags);
		wl_buffer_add_listener(buffer->buffer, &buffer_listener, buffer);
	}
	else
		zwp_linux_buffer_params_v1_create(params,
					  buffer->width,
					  buffer->height,
					  format,
					  flags);

	return 0;

error2:
	drm_dev->free_bo(buffer);
error1:
	drm_shutdown(buffer);
error:
	return -1;
}

static void
xdg_surface_handle_configure(void *data, struct zxdg_surface_v6 *surface,
			     uint32_t serial)
{
	struct window *window = data;

	zxdg_surface_v6_ack_configure(surface, serial);

	if (window->initialized && window->wait_for_configure)
		redraw(window, NULL, 0);
	window->wait_for_configure = false;
}

static const struct zxdg_surface_v6_listener xdg_surface_listener = {
	xdg_surface_handle_configure,
};

static void
xdg_toplevel_handle_configure(void *data, struct zxdg_toplevel_v6 *toplevel,
			      int32_t width, int32_t height,
			      struct wl_array *states)
{
}

static void
xdg_toplevel_handle_close(void *data, struct zxdg_toplevel_v6 *xdg_toplevel)
{
	running = 0;
}

static const struct zxdg_toplevel_v6_listener xdg_toplevel_listener = {
	xdg_toplevel_handle_configure,
	xdg_toplevel_handle_close,
};

struct window *
create_window(struct display *display, int width, int height)
{
	struct window *window;
	int i;
	int ret;

	window = calloc(1, sizeof *window);
	if (!window)
		return NULL;

	window->callback = NULL;
	window->display = display;
	window->width = width;
	window->height = height;
	window->surface = wl_compositor_create_surface(display->compositor);

	if (display->shell) {
		window->xdg_surface =
			zxdg_shell_v6_get_xdg_surface(display->shell,
						      window->surface);

		assert(window->xdg_surface);

		zxdg_surface_v6_add_listener(window->xdg_surface,
					     &xdg_surface_listener, window);

		window->xdg_toplevel =
			zxdg_surface_v6_get_toplevel(window->xdg_surface);

		assert(window->xdg_toplevel);

		zxdg_toplevel_v6_add_listener(window->xdg_toplevel,
					      &xdg_toplevel_listener, window);

		zxdg_toplevel_v6_set_title(window->xdg_toplevel, "Android");

		window->wait_for_configure = true;
		wl_surface_commit(window->surface);
	} else if (display->fshell) {
		zwp_fullscreen_shell_v1_present_surface(display->fshell,
							window->surface,
							ZWP_FULLSCREEN_SHELL_V1_PRESENT_METHOD_DEFAULT,
							NULL);
	} else {
		assert(0);
	}
#if 0
	for (i = 0; i < NUM_BUFFERS; ++i) {
		ret = create_dmabuf_buffer(display, &window->buffers[i],
		                               width, height, format, opts);

		if (ret < 0)
			return NULL;
	}
#endif
	return window;
}

static void
destroy_window(struct window *window)
{
	struct drm_device* dev;
	int i;

	if (window->callback)
		wl_callback_destroy(window->callback);

	for (i = 0; i < NUM_BUFFERS; i++) {
		if (!window->buffers[i].buffer)
			continue;

		wl_buffer_destroy(window->buffers[i].buffer);
		dev = window->buffers[i].dev;
		dev->free_bo(&window->buffers[i]);
		close(window->buffers[i].dmabuf_fd);
		drm_shutdown(&window->buffers[i]);
	}

	if (window->xdg_toplevel)
		zxdg_toplevel_v6_destroy(window->xdg_toplevel);
	if (window->xdg_surface)
		zxdg_surface_v6_destroy(window->xdg_surface);
	wl_surface_destroy(window->surface);
	free(window);
}

static struct buffer *
window_next_buffer(struct window *window)
{
	int i;

	for (i = 0; i < NUM_BUFFERS; i++)
		if (!window->buffers[i].busy)
			return &window->buffers[i];

	return NULL;
}

static const struct wl_callback_listener frame_listener;

static void
redraw(void *data, struct wl_callback *callback, uint32_t time)
{
	struct window *window = data;
	struct buffer *buffer;

	buffer = window_next_buffer(window);
	if (!buffer) {
		fprintf(stderr,
			!callback ? "Failed to create the first buffer.\n" :
			"All buffers busy at redraw(). Server bug?\n");
		abort();
	}

	/* XXX: would be nice to draw something that changes here... */

	wl_surface_attach(window->surface, buffer->buffer, 0, 0);
	wl_surface_damage(window->surface, 0, 0, window->width, window->height);

	if (callback)
		wl_callback_destroy(callback);

	window->callback = wl_surface_frame(window->surface);
	wl_callback_add_listener(window->callback, &frame_listener, window);
	wl_surface_commit(window->surface);
	buffer->busy = 1;
}

static const struct wl_callback_listener frame_listener = {
	redraw
};

static void
dmabuf_modifiers(void *data, struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf,
		 uint32_t format, uint32_t modifier_hi, uint32_t modifier_lo)
{
	struct display *d = data;
	uint64_t modifier = ((uint64_t) modifier_hi << 32) | modifier_lo;

	switch (format) {
	case DRM_FORMAT_XRGB8888:
		d->xrgb8888_format_found = 1;
		break;
	case DRM_FORMAT_NV12:
		switch (modifier) {
		case DRM_FORMAT_MOD_SAMSUNG_64_32_TILE:
		case DRM_FORMAT_MOD_LINEAR:
			d->nv12_format_found = 1;
			d->nv12_modifier = modifier;
			break;
		}
	default:
		break;
	}
}

static void
dmabuf_format(void *data, struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf, uint32_t format)
{
	/* XXX: deprecated */
}

static const struct zwp_linux_dmabuf_v1_listener dmabuf_listener = {
	dmabuf_format,
	dmabuf_modifiers
};

static void
xdg_shell_ping(void *data, struct zxdg_shell_v6 *shell, uint32_t serial)
{
	zxdg_shell_v6_pong(shell, serial);
}

static const struct zxdg_shell_v6_listener xdg_shell_listener = {
	xdg_shell_ping,
};

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

static const struct wl_seat_listener seat_listener = {
	seat_handle_capabilities,
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
					 id, &wl_compositor_interface, 1);
	} else if (strcmp(interface, "wl_subcompositor") == 0) {
		d->subcompositor =
			wl_registry_bind(registry,
					 id, &wl_subcompositor_interface, 1);
	} else if (strcmp(interface, "zxdg_shell_v6") == 0) {
		d->shell = wl_registry_bind(registry,
					    id, &zxdg_shell_v6_interface, 1);
		zxdg_shell_v6_add_listener(d->shell, &xdg_shell_listener, d);
	} else if (strcmp(interface, "zwp_fullscreen_shell_v1") == 0) {
		d->fshell = wl_registry_bind(registry,
					     id, &zwp_fullscreen_shell_v1_interface, 1);
	} else if (strcmp(interface, "zwp_linux_dmabuf_v1") == 0) {
		if (version < 3)
			return;
		d->dmabuf = wl_registry_bind(registry,
					     id, &zwp_linux_dmabuf_v1_interface, 3);
		zwp_linux_dmabuf_v1_add_listener(d->dmabuf, &dmabuf_listener, d);
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

	display->req_dmabuf_immediate = true;

	display->touch_listener = touch_listener;
	display->touch_data = touch_data;

	display->registry = wl_display_get_registry(display->display);
	wl_registry_add_listener(display->registry,
				 &registry_listener, display);
	wl_display_roundtrip(display->display);
	if (display->dmabuf == NULL) {
		fprintf(stderr, "No zwp_linux_dmabuf global\n");
		return NULL;
	}

	wl_display_roundtrip(display->display);

	return display;
}

static void
destroy_display(struct display *display)
{
	if (display->dmabuf)
		zwp_linux_dmabuf_v1_destroy(display->dmabuf);

	if (display->shell)
		zxdg_shell_v6_destroy(display->shell);

	if (display->fshell)
		zwp_fullscreen_shell_v1_release(display->fshell);

	if (display->compositor)
		wl_compositor_destroy(display->compositor);

	wl_registry_destroy(display->registry);
	wl_display_flush(display->display);
	wl_display_disconnect(display->display);
	free(display);
}

static void
signal_int(int signum)
{
	running = 0;
}

static void
print_usage_and_exit(void)
{
	printf("usage flags:\n"
		"\t'--import-immediate=<>'\n\t\t0 to import dmabuf via roundtrip,"
		"\n\t\t1 to enable import without roundtrip\n"
		"\t'--y-inverted=<>'\n\t\t0 to not pass Y_INVERTED flag,"
		"\n\t\t1 to pass Y_INVERTED flag\n"
		"\t'--import-format=<>'\n\t\tXRGB to import dmabuf as XRGB8888,"
		"\n\t\tNV12 to import as multi plane NV12\n");
	exit(0);
}

static int
is_true(const char* c)
{
	if (!strcmp(c, "1"))
		return 1;
	else if (!strcmp(c, "0"))
		return 0;
	else
		print_usage_and_exit();

	return 0;
}

static int
parse_import_format(const char* c)
{
	if (!strcmp(c, "NV12"))
		return DRM_FORMAT_NV12;
	else if (!strcmp(c, "XRGB"))
		return DRM_FORMAT_XRGB8888;
	else
		print_usage_and_exit();

	return 0;
}


#ifdef __cplusplus
extern "C"
{
#endif

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

#define HAVE_LIBDRM_ETNAVIV 1

struct display {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_subcompositor *subcompositor;
	struct wl_seat *seat;
	struct wl_touch *touch;
	struct wl_output *output;
	struct wp_presentation *presentation;
	struct zxdg_shell_v6 *shell;
	struct zwp_fullscreen_shell_v1 *fshell;
	struct zwp_linux_dmabuf_v1 *dmabuf;

	int xrgb8888_format_found;
	int nv12_format_found;
	uint64_t nv12_modifier;
	int req_dmabuf_immediate;

   struct wl_touch_listener *touch_listener;
   void *touch_data;

	int width;
	int height;
};

struct drm_device {
	int fd;
	char *name;

	int (*alloc_bo)(struct buffer *buf);
	void (*free_bo)(struct buffer *buf);
	int (*export_bo_to_prime)(struct buffer *buf);
	int (*map_bo)(struct buffer *buf);
	void (*unmap_bo)(struct buffer *buf);
	void (*device_destroy)(struct buffer *buf);
};

struct buffer {
	struct wl_buffer *buffer;
	int busy;
	struct wp_presentation_feedback *feedback;

	struct drm_device *dev;
	int drm_fd;

#ifdef HAVE_LIBDRM_INTEL
	drm_intel_bufmgr *bufmgr;
	drm_intel_bo *intel_bo;
#endif /* HAVE_LIBDRM_INTEL */
#if HAVE_LIBDRM_FREEDRENO
	struct fd_device *fd_dev;
	struct fd_bo *fd_bo;
#endif /* HAVE_LIBDRM_FREEDRENO */
#if HAVE_LIBDRM_ETNAVIV
	struct etna_device *etna_dev;
	struct etna_bo *etna_bo;
#endif /* HAVE_LIBDRM_ETNAVIV */

	uint32_t gem_handle;
	int dmabuf_fd;
	uint8_t *mmap;

	int width;
	int height;
	int bpp;
	unsigned long stride;
	int format;

   int timeline_fd;
   int release_fence_fd;
};

#define NUM_BUFFERS 1024
#define NUM_SURFACES 128

struct window {
	struct display *display;
	int width, height;
	struct wl_surface *surface;
	struct zxdg_surface_v6 *xdg_surface;
	struct zxdg_toplevel_v6 *xdg_toplevel;
	struct buffer buffers[NUM_BUFFERS];
   struct wl_surface *surfaces[NUM_SURFACES];
   struct wl_subsurface *subsurfaces[NUM_SURFACES];

	struct buffer *prev_buffer;
	struct wl_callback *callback;
	bool initialized;
	bool wait_for_configure;
};

int
create_dmabuf_buffer(struct display *display, struct buffer *buffer,
		     int width, int height, int format, uint32_t opts,
		     int prime_fd, int stride, uint64_t modifier);

struct display *
create_display(const struct wl_touch_listener *touch_listener, void *touch_data);

struct window *
create_window(struct display *display, int width, int height);

#ifdef __cplusplus
}
#endif

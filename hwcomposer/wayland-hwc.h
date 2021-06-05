#ifdef __cplusplus
extern "C"
{
#endif

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

struct display {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_subcompositor *subcompositor;
	struct wl_seat *seat;
	struct wl_shell *shell;
	struct wl_touch *touch;
	struct wl_output *output;
	struct wp_presentation *presentation;
	struct android_wlegl *android_wlegl;
	struct zwp_linux_dmabuf_v1 *dmabuf;
	int gtype;

	int input_fd[INPUT_TOTAL];

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

struct window {
	struct display *display;
	int width, height;
	struct wl_surface *surface;
	struct wl_shell_surface *shell_surface;
	struct buffer buffers[NUM_BUFFERS];

	struct buffer *prev_buffer;
	struct wl_callback *callback;
	bool initialized;
	bool wait_for_configure;
};

int
create_android_wl_buffer(struct display *display, struct buffer *buffer,
		     int width, int height, int format,
		     int stride, buffer_handle_t target);

int
create_dmabuf_wl_buffer(struct display *display, struct buffer *buffer,
             int width, int height, int format,
             int prime_fd, int stride, uint64_t modifier,
             buffer_handle_t target);

struct display *
create_display(const char* gralloc);
void
destroy_display(struct display *display);

struct window *
create_window(struct display *display, int width, int height);

#ifdef __cplusplus
}
#endif

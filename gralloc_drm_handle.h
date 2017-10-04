/*
 * Copyright (C) 2010-2011 Chia-I Wu <olvaffe@gmail.com>
 * Copyright (C) 2010-2011 LunarG Inc.
 * Copyright (C) 2016 Linaro, Ltd., Rob Herring <robh@kernel.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef _GRALLOC_DRM_HANDLE_H_
#define _GRALLOC_DRM_HANDLE_H_

#include <cutils/log.h>
#include <cutils/native_handle.h>
#include <system/graphics.h>

#ifdef __cplusplus
extern "C" {
#endif

struct gralloc_drm_handle_t {
	native_handle_t base;

	/* file descriptors */
	int prime_fd;

	/* integers */
	int magic;

	int width;
	int height;
	int format;
	int usage;

	int name;   /* the name of the bo */
	int stride; /* the stride in bytes */
	int data_owner; /* owner of data (for validation) */

	uint64_t modifier __attribute__((aligned(8))); /* buffer modifiers */
	union {
		void *data; /* pointer to struct gralloc_gbm_bo_t */
		uint64_t reserved;
	} __attribute__((aligned(8)));
};
#define GRALLOC_GBM_HANDLE_MAGIC 0x5f47424d
#define GRALLOC_GBM_HANDLE_NUM_FDS 1
#define GRALLOC_GBM_HANDLE_NUM_INTS (						\
	((sizeof(struct gralloc_drm_handle_t) - sizeof(native_handle_t))/sizeof(int))	\
	 - GRALLOC_GBM_HANDLE_NUM_FDS)

static inline struct gralloc_drm_handle_t *gralloc_drm_handle(buffer_handle_t _handle)
{
	struct gralloc_drm_handle_t *handle =
		(struct gralloc_drm_handle_t *) _handle;

	if (handle && (handle->base.version != sizeof(handle->base) ||
	               handle->base.numInts != GRALLOC_GBM_HANDLE_NUM_INTS ||
	               handle->base.numFds != GRALLOC_GBM_HANDLE_NUM_FDS ||
	               handle->magic != GRALLOC_GBM_HANDLE_MAGIC)) {
		ALOGE("invalid handle: version=%d, numInts=%d, numFds=%d, magic=%x",
			handle->base.version, handle->base.numInts,
			handle->base.numFds, handle->magic);
		handle = NULL;
	}

	return handle;
}

static inline int gralloc_drm_get_prime_fd(buffer_handle_t _handle)
{
	struct gralloc_drm_handle_t *handle = gralloc_drm_handle(_handle);
	return (handle) ? handle->prime_fd : -1;
}

static inline int gralloc_drm_get_gem_handle(buffer_handle_t handle)
{
	return 0; /* Not supported, return invalid handle. */
}

#ifdef __cplusplus
}
#endif
#endif /* _GRALLOC_DRM_HANDLE_H_ */

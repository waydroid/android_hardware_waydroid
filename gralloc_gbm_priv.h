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

#ifndef _GRALLOC_GBM_PRIV_H_
#define _GRALLOC_GBM_PRIV_H_

#ifdef __cplusplus
extern "C" {
#endif

struct gbm_device;
struct gbm_bo;

int gralloc_gbm_handle_register(buffer_handle_t handle, struct gbm_device *gbm);
int gralloc_gbm_handle_unregister(buffer_handle_t handle);

buffer_handle_t gralloc_gbm_bo_create(struct gbm_device *gbm,
		int width, int height, int format, int usage, int *stride);
void gbm_free(buffer_handle_t handle);

struct gbm_bo *gralloc_gbm_bo_from_handle(buffer_handle_t handle);
buffer_handle_t gralloc_gbm_bo_get_handle(struct gbm_bo *bo);
int gralloc_gbm_get_gem_handle(buffer_handle_t handle);

int gralloc_gbm_bo_lock(buffer_handle_t handle, int usage, int x, int y, int w, int h, void **addr);
int gralloc_gbm_bo_unlock(buffer_handle_t handle);
int gralloc_gbm_bo_lock_ycbcr(buffer_handle_t handle, int usage,
		int x, int y, int w, int h, struct android_ycbcr *ycbcr);

struct gbm_device *gbm_dev_create(void);
void gbm_dev_destroy(struct gbm_device *gbm);

#ifdef __cplusplus
}
#endif
#endif /* _GRALLOC_GBM_PRIV_H_ */

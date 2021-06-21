/*
 * Copyright (C) 2012 The Android Open Source Project
 * Copyright (C) 2021 The Anbox Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <unistd.h>
#include <wayland-client.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <log/log.h>
#include <cutils/properties.h>
#include <hardware/hwcomposer.h>
#include <libsync/sw_sync.h>
#include <sync/sync.h>
#include <drm_fourcc.h>
#include <presentation-time-client-protocol.h>
#include <gralloc_handle.h>

#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#include <cutils/trace.h>
#include <utils/Trace.h>

#include "wayland-hwc.h"

struct anbox_hwc_composer_device_1 {
    hwc_composer_device_1_t base; // constant after init
    const hwc_procs_t *procs;     // constant after init
    pthread_t wayland_thread;     // constant after init
    pthread_t vsync_thread;       // constant after init
    int32_t vsync_period_ns;      // constant after init
    struct display *display;      // constant after init
    struct window *window;        // constant after init

    pthread_mutex_t vsync_lock;
    bool vsync_callback_enabled; // protected by this->vsync_lock
    uint64_t last_vsync_ns;

    int timeline_fd;
    int next_sync_point;
};

static int hwc_prepare(hwc_composer_device_1_t* dev __unused,
                       size_t numDisplays, hwc_display_contents_1_t** displays) {
    if (!numDisplays || !displays) return 0;

    hwc_display_contents_1_t* contents = displays[HWC_DISPLAY_PRIMARY];

    if (!contents) return 0;

    for (size_t i = 0; i < contents->numHwLayers; i++) {
        if (contents->hwLayers[i].compositionType == HWC_FRAMEBUFFER_TARGET)
            continue;
        if (contents->hwLayers[i].flags & HWC_SKIP_LAYER)
            continue;

        if (contents->hwLayers[i].compositionType == HWC_OVERLAY)
            contents->hwLayers[i].compositionType = HWC_FRAMEBUFFER;
    }

    return 0;
}

static struct buffer *get_wl_buffer(struct anbox_hwc_composer_device_1 *pdev, buffer_handle_t handle, int width, int height)
{
    if (pdev->window->buffer_map.find(handle) == pdev->window->buffer_map.end()) {
        struct buffer *buf;
        int ret = 0;

        buf = (struct buffer *)calloc(1, sizeof *buf);
        int stride = property_get_int32("anbox.layer.stride", width);
        int format = property_get_int32("anbox.layer.format", HAL_PIXEL_FORMAT_RGBA_8888);
        if (pdev->display->gtype == GRALLOC_ANDROID) {
            ret = create_android_wl_buffer(pdev->display, buf, width, height, format, stride, handle);
        } else if (pdev->display->gtype == GRALLOC_GBM) {
            struct gralloc_handle_t *drm_handle = (struct gralloc_handle_t *) handle;
            ret = create_dmabuf_wl_buffer(pdev->display, buf, drm_handle->width, drm_handle->height, drm_handle->format, drm_handle->prime_fd, drm_handle->stride, drm_handle->modifier);
        }

        if (ret) {
            ALOGE("failed to create a wayland buffer");
            return NULL;
        }
        pdev->window->buffer_map[handle] = buf;
    }

    return pdev->window->buffer_map[handle];
}

static long time_to_sleep_to_next_vsync(struct timespec *rt, uint64_t last_vsync_ns, unsigned vsync_period_ns)
{
    uint64_t now = (uint64_t)rt->tv_sec * 1e9 + rt->tv_nsec;
    unsigned frames_since_last_vsync = (now - last_vsync_ns) / vsync_period_ns + 1;
    uint64_t next_vsync = last_vsync_ns + frames_since_last_vsync * vsync_period_ns;

    return next_vsync - now;
}

static void* hwc_vsync_thread(void* data) {
    struct anbox_hwc_composer_device_1* pdev = (struct anbox_hwc_composer_device_1*)data;
    setpriority(PRIO_PROCESS, 0, HAL_PRIORITY_URGENT_DISPLAY);

    struct timespec rt;
    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in vsync thread clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }
    bool vsync_enabled = false;

    struct timespec wait_time;
    wait_time.tv_sec = 0;

    pthread_mutex_lock(&pdev->vsync_lock);
    wait_time.tv_nsec = time_to_sleep_to_next_vsync(&rt, pdev->last_vsync_ns, pdev->vsync_period_ns);
    pthread_mutex_unlock(&pdev->vsync_lock);

    while (true) {
        ATRACE_BEGIN("hwc_vsync_thread");
        int err = nanosleep(&wait_time, NULL);
        if (err == -1) {
            if (errno == EINTR) {
                break;
            }
            ATRACE_END();
            ALOGE("error in vsync thread: %s", strerror(errno));
            return NULL;
        }

        pthread_mutex_lock(&pdev->vsync_lock);
        vsync_enabled = pdev->vsync_callback_enabled;
        pthread_mutex_unlock(&pdev->vsync_lock);

        if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
            ALOGE("%s:%d error in vsync thread clock_gettime: %s",
                  __FILE__, __LINE__, strerror(errno));
        }

        pthread_mutex_lock(&pdev->vsync_lock);
        wait_time.tv_nsec = time_to_sleep_to_next_vsync(&rt, pdev->last_vsync_ns, pdev->vsync_period_ns);
        pthread_mutex_unlock(&pdev->vsync_lock);

        if (!vsync_enabled) {
            ATRACE_END();
            continue;
        }

        int64_t timestamp = (uint64_t)rt.tv_sec * 1e9 + rt.tv_nsec;
        pdev->procs->vsync(pdev->procs, 0, timestamp);
        ATRACE_END();
    }

    return NULL;
}

static void
feedback_sync_output(void *, struct wp_presentation_feedback *,
             struct wl_output *)
{
}

static void
feedback_presented(void *data,
           struct wp_presentation_feedback *,
           uint32_t tv_sec_hi,
           uint32_t tv_sec_lo,
           uint32_t tv_nsec,
           uint32_t,
           uint32_t,
           uint32_t,
           uint32_t)
{
    struct anbox_hwc_composer_device_1* pdev = (struct anbox_hwc_composer_device_1*)data;

    pthread_mutex_lock(&pdev->vsync_lock);
    pdev->last_vsync_ns = (((uint64_t)tv_sec_hi << 32) + tv_sec_lo) * 1e9 + tv_nsec;
    pthread_mutex_unlock(&pdev->vsync_lock);
}

static void
feedback_discarded(void *, struct wp_presentation_feedback *)
{
}

static const struct wp_presentation_feedback_listener feedback_listener = {
    feedback_sync_output,
    feedback_presented,
    feedback_discarded
};

inline void getLayerResolution(const hwc_layer_1_t* layer,
                               int& width, int& height) {
    hwc_rect_t displayFrame  = layer->displayFrame;
    width = displayFrame.right - displayFrame.left;
    height = displayFrame.bottom - displayFrame.top;
}

static int hwc_set(struct hwc_composer_device_1* dev,size_t numDisplays,
                   hwc_display_contents_1_t** displays) {

    struct anbox_hwc_composer_device_1* pdev = (struct anbox_hwc_composer_device_1*)dev;
    struct wl_region *region;
    int width, height;

    if (!numDisplays || !displays) {
        return 0;
    }

    hwc_display_contents_1_t* contents = displays[HWC_DISPLAY_PRIMARY];
    contents->retireFenceFd = sw_sync_fence_create(pdev->timeline_fd, "hwc_contents_release", pdev->next_sync_point);

    int err = 0;
    for (size_t layer = 0; layer < contents->numHwLayers; layer++) {
        hwc_layer_1_t* fb_layer = &contents->hwLayers[layer];
        getLayerResolution(fb_layer, width, height);

        if (fb_layer->flags & HWC_SKIP_LAYER) {
            if (fb_layer->acquireFenceFd != -1) {
                close(fb_layer->acquireFenceFd);
            }
            continue;
        }

        if (fb_layer->compositionType != HWC_FRAMEBUFFER_TARGET) {
            if (fb_layer->acquireFenceFd != -1) {
                close(fb_layer->acquireFenceFd);
            }
            continue;
        }

        if (!fb_layer->handle) {
            if (fb_layer->acquireFenceFd != -1) {
                close(fb_layer->acquireFenceFd);
            }
            continue;
        }

        struct buffer *buf = get_wl_buffer(pdev, fb_layer->handle, width, height);
        if (!buf) {
            ALOGE("Failed to get wayland buffer");
            if (fb_layer->acquireFenceFd != -1) {
               close(fb_layer->acquireFenceFd);
            }
            continue;
        }

        if (buf->busy) {
            if (fb_layer->acquireFenceFd != -1) {
                close(fb_layer->acquireFenceFd);
            }
            continue;
        }

        /* These layers do not require a releaseFenceFD to be created:
         * HWC_FRAMEBUFFER, HWC_SIDEBAND
         * https://android.googlesource.com/platform/hardware/libhardware/+/master/include/hardware/hwcomposer.h#216
         */
        if (fb_layer->compositionType != HWC_FRAMEBUFFER &&
            fb_layer->compositionType != HWC_SIDEBAND)
        {
            /* To be signaled when the compositor releases the buffer */
            fb_layer->releaseFenceFd = sw_sync_fence_create(pdev->timeline_fd, "wayland_release", pdev->next_sync_point++);
            buf->release_fence_fd = fb_layer->releaseFenceFd;
        } else {
            buf->release_fence_fd = -1;
        }
        buf->timeline_fd = pdev->timeline_fd;

        struct wl_surface *surface = pdev->window->surface;
        if (!surface) {
            ALOGE("Failed to get surface");
            continue;
        }

        buf->busy = true;

        wl_surface_attach(surface, buf->buffer, 0, 0);
        wl_surface_damage(surface, 0, 0, buf->width, buf->height);
        if (pdev->display->scale > 1)
            wl_surface_set_buffer_scale(surface, pdev->display->scale);

        region = wl_compositor_create_region(pdev->display->compositor);
        wl_region_add(region, 0, 0, buf->width, buf->height);
        wl_surface_set_opaque_region(surface, region);
        wl_region_destroy(region);

        struct wp_presentation *pres = pdev->window->display->presentation;
        if (pres) {
            buf->feedback = wp_presentation_feedback(pres, surface);
            wp_presentation_feedback_add_listener(buf->feedback,
                              &feedback_listener, pdev);
        }

        wl_surface_commit(surface);

        const int kAcquireWarningMS = 100;
        err = sync_wait(fb_layer->acquireFenceFd, kAcquireWarningMS);
        if (err < 0 && errno == ETIME) {
            ALOGE("hwcomposer waited on fence %d for %d ms",
                fb_layer->acquireFenceFd, kAcquireWarningMS);
        }
        close(fb_layer->acquireFenceFd);

        wl_display_flush(pdev->display->display);
    }

    /* TODO: According to[1] the contents->retireFenceFd is the responsibility
     * of SurfaceFlinger to close, but leaving it open is causing a graphical
     * stall.
     * [1] https://android.googlesource.com/platform/hardware/libhardware/+/master/include/hardware/hwcomposer.h#333
     */
    close(contents->retireFenceFd);
    contents->retireFenceFd = -1;

    return err;
}

static int hwc_query(struct hwc_composer_device_1* dev, int what, int* value) {
    struct anbox_hwc_composer_device_1* pdev =
            (struct anbox_hwc_composer_device_1*)dev;

    switch (what) {
        case HWC_VSYNC_PERIOD:
            value[0] = pdev->vsync_period_ns;
            break;
        default:
            // unsupported query
            ALOGE("%s badness unsupported query what=%d", __FUNCTION__, what);
            return -EINVAL;
    }
    return 0;
}

static int hwc_event_control(struct hwc_composer_device_1* dev, int dpy __unused,
                             int event, int enabled) {
    struct anbox_hwc_composer_device_1* pdev =
            (struct anbox_hwc_composer_device_1*)dev;
    int ret = -EINVAL;

    // enabled can only be 0 or 1
    if (!(enabled & ~1)) {
        if (event == HWC_EVENT_VSYNC) {
            pthread_mutex_lock(&pdev->vsync_lock);
            pdev->vsync_callback_enabled = enabled;
            pthread_mutex_unlock(&pdev->vsync_lock);
            ret = 0;
        }
    }
    return ret;
}

static int hwc_blank(struct hwc_composer_device_1* dev __unused, int disp __unused,
                     int blank __unused) {
    return 0;
}

static void hwc_dump(hwc_composer_device_1* dev __unused, char* buff __unused,
                     int buff_len __unused) {
    // This is run when running dumpsys.
    // No-op for now.
}


static int hwc_get_display_configs(struct hwc_composer_device_1* dev __unused,
                                   int disp, uint32_t* configs, size_t* numConfigs) {
    if (*numConfigs == 0) {
        return 0;
    }

    if (disp == HWC_DISPLAY_PRIMARY) {
        configs[0] = 0;
        *numConfigs = 1;
        return 0;
    }

    return -EINVAL;
}


static int32_t hwc_attribute(struct anbox_hwc_composer_device_1* pdev,
                             const uint32_t attribute) {
    char property[PROPERTY_VALUE_MAX];
    int width = 0;
    int height = 0;
    int density = 0;

    switch(attribute) {
        case HWC_DISPLAY_VSYNC_PERIOD:
            return pdev->vsync_period_ns;
        case HWC_DISPLAY_WIDTH:
            if (property_get("anbox.display_width", property, nullptr) > 0) {
                width = atoi(property);
            }
            return width;
        case HWC_DISPLAY_HEIGHT:
            if (property_get("anbox.display_height", property, nullptr) > 0) {
                height = atoi(property);
            }
            return height;
        case HWC_DISPLAY_DPI_X:
        case HWC_DISPLAY_DPI_Y:
            if (property_get("ro.sf.lcd_density", property, nullptr) > 0) {
                density = atoi(property);
            }
            return density * 1000;
        case HWC_DISPLAY_COLOR_TRANSFORM:
            return HAL_COLOR_TRANSFORM_IDENTITY;
        default:
            ALOGE("unknown display attribute %u", attribute);
            return -EINVAL;
    }
}

static int hwc_get_display_attributes(struct hwc_composer_device_1* dev __unused,
                                      int disp, uint32_t config __unused,
                                      const uint32_t* attributes, int32_t* values) {
    struct anbox_hwc_composer_device_1* pdev = (struct anbox_hwc_composer_device_1*)dev;
    for (int i = 0; attributes[i] != HWC_DISPLAY_NO_ATTRIBUTE; i++) {
        if (disp == HWC_DISPLAY_PRIMARY) {
            values[i] = hwc_attribute(pdev, attributes[i]);
            if (values[i] == -EINVAL) {
                return -EINVAL;
            }
        } else {
            ALOGE("unknown display type %u", disp);
            return -EINVAL;
        }
    }

    return 0;
}

static int hwc_close(hw_device_t* dev) {
    struct anbox_hwc_composer_device_1* pdev = (struct anbox_hwc_composer_device_1*)dev;

    for (std::map<buffer_handle_t, struct buffer *>::iterator it = pdev->window->buffer_map.begin(); it != pdev->window->buffer_map.end(); it++)
    {
        wl_buffer_destroy(it->second->buffer);
    }
    pdev->window->buffer_map.clear();

    destroy_display(pdev->display);

    pthread_kill(pdev->wayland_thread, SIGTERM);
    pthread_join(pdev->wayland_thread, NULL);

    free(dev);
    return 0;
}

static void* hwc_wayland_thread(void* data) {
    struct anbox_hwc_composer_device_1* pdev = (struct anbox_hwc_composer_device_1*)data;
    int ret = 0;

    setpriority(PRIO_PROCESS, 0, HAL_PRIORITY_URGENT_DISPLAY);

    while (ret != -1)
        ret = wl_display_dispatch(pdev->display->display);

    ALOGE("*** %s: Wayland client was disconnected: %s", __PRETTY_FUNCTION__, strerror(ret));

    return NULL;
}

static void hwc_register_procs(struct hwc_composer_device_1* dev,
                               hwc_procs_t const* procs) {
    struct anbox_hwc_composer_device_1* pdev = (struct anbox_hwc_composer_device_1*)dev;
    pdev->procs = procs;
}

static int hwc_open(const struct hw_module_t* module, const char* name,
                    struct hw_device_t** device) {
    int ret = 0;
    char property[PROPERTY_VALUE_MAX];
    int width = 0;
    int height = 0;

    if (strcmp(name, HWC_HARDWARE_COMPOSER)) {
        ALOGE("%s called with bad name %s", __FUNCTION__, name);
        return -EINVAL;
    }

    anbox_hwc_composer_device_1 *pdev = new anbox_hwc_composer_device_1();
    if (!pdev) {
        ALOGE("%s failed to allocate dev", __FUNCTION__);
        return -ENOMEM;
    }

    pdev->base.common.tag = HARDWARE_DEVICE_TAG;
    pdev->base.common.version = HWC_DEVICE_API_VERSION_1_1;
    pdev->base.common.module = const_cast<hw_module_t *>(module);
    pdev->base.common.close = hwc_close;

    pdev->base.prepare = hwc_prepare;
    pdev->base.set = hwc_set;
    pdev->base.eventControl = hwc_event_control;
    pdev->base.blank = hwc_blank;
    pdev->base.query = hwc_query;
    pdev->base.registerProcs = hwc_register_procs;
    pdev->base.dump = hwc_dump;
    pdev->base.getDisplayConfigs = hwc_get_display_configs;
    pdev->base.getDisplayAttributes = hwc_get_display_attributes;

    pdev->vsync_period_ns = 1000*1000*1000/60; // vsync is 60 hz
    pdev->timeline_fd = sw_sync_timeline_create();
    pdev->next_sync_point = 1;

    if (property_get("anbox.xdg_runtime_dir", property, "/run/user/1000") > 0) {
        setenv("XDG_RUNTIME_DIR", property, 1);
    }
    if (property_get("anbox.wayland_display", property, "wayland-0") > 0) {
        setenv("WAYLAND_DISPLAY", property, 1);
    }
    if (property_get("ro.hardware.gralloc", property, "default") > 0) {
        pdev->display = create_display(property);
    }
    if (!pdev->display) {
        ALOGE("failed to open wayland connection");
        return -ENODEV;
    }
    ALOGE("wayland display %p", pdev->display);

    if (property_get("anbox.display_width", property, nullptr) > 0) {
        width = atoi(property);
    }

    if (property_get("anbox.display_height", property, nullptr) > 0) {
        height = atoi(property);
    }

    pdev->window = create_window(pdev->display, width, height);
    if (!pdev->display) {
        ALOGE("failed to create the wayland window");
        return -ENODEV;
    }

    /* Here we retrieve objects if executed without immed, or error */
    wl_display_roundtrip(pdev->display->display);
    wl_surface_commit(pdev->window->surface);

    pthread_mutex_init(&pdev->vsync_lock, NULL);
    pdev->vsync_callback_enabled = true;

    struct timespec rt;
    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
       ALOGE("%s:%d error in vsync thread clock_gettime: %s",
            __FILE__, __LINE__, strerror(errno));
    }

    pdev->last_vsync_ns = int64_t(rt.tv_sec) * 1e9 + rt.tv_nsec;

    if (!pdev->vsync_thread) {
        ret = pthread_create (&pdev->vsync_thread, NULL, hwc_vsync_thread, pdev);
        if (ret) {
            ALOGE("anbox_hw_composer could not start vsync_thread\n");
        }
    }

    ret = pthread_create (&pdev->wayland_thread, NULL, hwc_wayland_thread, pdev);
    if (ret) {
        ALOGE("anbox_hw_composer could not start wayland_thread\n");
    }

    *device = &pdev->base.common;

    return ret;
}


static struct hw_module_methods_t hwc_module_methods = {
    .open = hwc_open,
};

hwc_module_t HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = HWC_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = HWC_HARDWARE_MODULE_ID,
        .name = "Anbox hwcomposer module",
        .author = "The Android Open Source Project",
        .methods = &hwc_module_methods,
    }
};

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
#include <chrono>
#include <map>
#include <list>
#include <set>
#include <pthread.h>
#include <semaphore.h>
#include <hardware/hwcomposer.h>
#include <cutils/properties.h>
#include <vendor/waydroid/task/1.0/IWaydroidTask.h>
#include <wayland-util.h>
#include <wayland-client.h>

#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <optional>
#include <unordered_map>
#include <unordered_set>

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

/* One uinput multitouch slot. The wayland touch id is unique only within a
 * wl_touch, so the connection is part of the key: two connections both start
 * at id 0 and would otherwise drive each other's slots. */
struct touch_slot {
    const struct wl_conn *conn = nullptr;
    int id = -1;
};

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

struct waydroid_hwc_composer_device_1;

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
    struct wl_buffer *wl_buffer = nullptr;
    /* The connection this wl_buffer was created on. Only ever one, and it
     * must outlive the buffer: wl_display_disconnect frees the proxy, and
     * destroying one afterwards is a double free (see ~buffer). */
    struct wl_conn *conn = nullptr;

    buffer_handle_t handle = nullptr;
    buffer_metadata metadata {};

    bool isShm = false;
    void *shm_data = nullptr;
    int size = 0;

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

    /* Keeps the currently attached wl_buffer alive: destroying a wl_buffer
     * that is still a surface's committed content blanks the surface. */
    std::shared_ptr<buffer> attached_buffer;

    void attach_buffer(std::shared_ptr<buffer> buf);
    void damage_surface(int32_t x, int32_t y, int32_t width, int32_t height);
    void set_buffer_transform(BufferTransform transform);
    void set_buffer_scale(double scale);
    // Requires the transformed rectangle
    void set_crop(hwc_frect_t crop);
    // TODO: This should not require scale. Scaling handling is broken
    void set_display_frame(hwc_rect_t rect, double scale);
};

/* One connection to the host compositor: the wl_display, everything bound on
 * it, and the thread dispatching its events. Today there is exactly one
 * (display->ctl); per-task connections come later. */
struct wl_conn {
    /* The Android-side state this connection serves. Set before the first
     * event can be dispatched: the registry handler already reads it. */
    struct display *dpy;

    /* display->ctl, the connection that owns the Android display geometry,
     * the cursor and the clipboard. Everything else is one streamed task,
     * named by task_id. Constant after init. */
    bool is_ctl = false;
    uint32_t task_id = 0;

    pthread_t wayland_thread; // constant after init

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
    struct zwp_pointer_gestures_v1 *pointer_gestures;
    struct zwp_pointer_gesture_pinch_v1 *pointer_gesture_pinch;
    struct zwp_relative_pointer_manager_v1 *relative_pointer_manager;
    struct zwp_relative_pointer_v1 *relative_pointer;
    struct zwp_idle_inhibit_manager_v1 *idle_manager;
    struct wp_fractional_scale_manager_v1 *fractional_scale_manager;
    struct wl_data_device_manager *data_device_manager;
    struct wl_data_device *data_device;

    /* Host format/modifier advertisement of this connection. */
    std::unordered_set<uint32_t> formats;
    std::map<uint32_t, std::vector<uint64_t>> modifiers;
    bool supports_cursor_viewport;
    bool supports_cursor_hw_buffer;

    /* A wl_buffer belongs to the connection it was created on. */
    std::unordered_map<buffer_handle_t, std::shared_ptr<buffer>> buffer_map;

    /* Surfaces and seat objects of this connection. */
    std::map<struct wl_surface *, struct layerFrame> layers;
    std::map<int, struct wl_surface *> touch_surfaces;
    struct wl_surface *pointer_surface;
    struct wl_surface *tablet_surface;
    std::list<struct zwp_tablet_tool_v2 *> tablet_tools;
    std::map<struct zwp_tablet_tool_v2 *, uint16_t> tablet_tools_evt;
    uint32_t keyboard_enter_serial;
    uint32_t pointer_enter_serial;

    /* Accumulated fractions of this connection's wl_pointer axis events. */
    double wheelAccumulatorX;
    double wheelAccumulatorY;
    bool wheelEvtIsDiscrete;
    bool wheelEvtIsTouchpad;

    /* Pointer and gesture state, all in this connection's surface space. The
     * previous position from another connection would emit a screen-wide
     * relative jump, and a gesture is driven end to end by one wl_pointer. */
    int ptrPrvX = 0;
    int ptrPrvY = 0;
    int gesturePoints[2] = {-1, -1};
    int gesturePosX = 0;
    int gesturePosY = 0;
    int gestureLength = -1;
    double rel_acc_x = 0;
    double rel_acc_y = 0;

    /*
     * Reconnect support. When the host compositor drops our wl_client (e.g.
     * Lomiri tearing down the connection on a single toplevel close), the
     * wayland thread sets wl_alive=false and parks on reconnect_resume instead
     * of aborting the whole HAL. The hwc compose thread (which owns pdev, and
     * therefore the cursor handler) performs the actual reconnect and posts
     * reconnect_resume to wake the wayland thread on the new connection.
     */
    std::atomic<bool> wl_alive{true};
    /* Set before quiescing so the dispatch thread exits instead of parking
     * for a reconnect that is never coming. */
    std::atomic<bool> stopping{false};
    sem_t reconnect_resume;
    /* Last reconnect, for the retry backoff. */
    struct timespec last_reconnect {};

    /* False between wl_display_disconnect and the next successful connect:
     * every proxy made on this connection is already freed. A wl_conn is
     * destroyed only after its dispatch thread is joined and everything that
     * names it is gone, so a buffer seeing this false is a lifetime bug. */
    bool proxies_valid = true;
};

struct window {
    struct layer : public surface_context {
        struct wl_subsurface *subsurface;
        struct zwp_locked_pointer_v1 *locked_pointer {};

        layer() = default;
        layer(wl_surface *surface, wp_viewport *viewport, wl_subsurface *subsurface = nullptr);
        ~layer();

        layer(layer &&other);
        layer &operator=(layer &&rhs);

        void set_position(int32_t x, int32_t y);
    };

    /* The connection this window's surfaces live on. Android-side state is
     * reached through conn->dpy. */
    struct wl_conn *conn;

    struct wl_shell_surface *shell_surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;

    /* Used for the background color */
    bool dedicated_background_surface;
    struct wl_surface *surface;
    struct wp_viewport *viewport;
    struct wl_buffer *bg_buffer;
    struct zwp_locked_pointer_v1 *locked_pointer;

    struct zwp_idle_inhibitor_v1 *idle_inhibitor;

    std::vector<layer> layers;

    std::unique_ptr<buffer> snapshot_buffer;
    /* Snapshot was attempted and is not possible for this window; don't
     * requeue it every frame. */
    bool snapshot_unavailable = false;
    /* Frames spent waiting for the platform service to write this task's
     * snapshot file before falling back (see snapshot_inactive_app_window). */
    int snapshot_file_attempts = 0;

    std::string appID;
    std::string taskID;

    std::atomic<bool> configured;

    /* First task-stream buffer posted before the initial xdg configure was
     * acked; attached from the configure handler (xdg-shell forbids attaching
     * earlier). Guarded by windowsMutex. */
    std::shared_ptr<buffer> pending_stream_buffer;

    /* Last XDG_TOPLEVEL_STATE_ACTIVATED state seen from the host compositor.
     * Used to detect the rising edge so a Lomiri-driven focus/raise of this
     * toplevel brings the matching Android task to the front exactly once. */
    bool activated = false;

    /* Last time engagement on this window re-asserted its Android task focus
     * (see reassert_task_focus); bounds how often a stream of touches can
     * repeat the call. Guarded by windowsMutex. */
    std::chrono::steady_clock::time_point last_focus_assert {};

    /* Host-side visibility tracking, used to drive Android screen power.
     * outputs_entered: number of wl_outputs this toplevel is currently shown
     * on (wl_surface.enter/leave); 0 means off-screen/minimized on compositors
     * that report it. suspended: XDG_TOPLEVEL_STATE_SUSPENDED (xdg-shell v6+),
     * the explicit "content not visible" signal. A window counts as visible
     * when outputs_entered > 0 && !suspended. */
    int outputs_entered = 0;
    bool suspended = false;

    /* A newly-mapped toplevel has outputs_entered == 0 until the compositor
     * first shows it. ever_shown records that first wl_surface.enter; created_at
     * bounds how long we keep the Android display awake waiting for that first
     * present (see any_window_visible), so a window that never appears cannot
     * pin the screen on forever. */
    bool ever_shown = false;
    std::chrono::steady_clock::time_point created_at = std::chrono::steady_clock::now();

    // Reset every hwc_set cycle
    struct wl_region* input_region;
    int lastLayer;
    std::shared_ptr<buffer> last_layer_buffer;

    ~window();

    /* sync_configure: wait for the initial xdg configure before returning.
     * Must be false when called off the wayland thread with windowsMutex held
     * (post_task_buffer): the roundtrip can deadlock against the configure
     * handler (do_hotplug -> adapter mutex -> hwc_set -> windowsMutex). */
    static std::unique_ptr<window> create(struct wl_conn *conn, bool use_subsurfaces, std::string appID, std::string taskID, hwc_color_t color, bool sync_configure = true);

    window::layer &get_next_layer();
    window::layer &create_new_layer();
    void reset_per_set_state();

    void minimize();
    void set_maximize(bool enabled);
    void set_title(const char *title);
    void set_app_id(std::string appID);

  private:
    window() = default;
};

/* The uinput FIFOs. ensure_input_pipe creates a node once; reset_input_pipe
 * recreates it, which is how a display geometry change reaches InputFlinger.
 * Defined in wayland-hwc.cpp. */
void ensure_input_pipe(int input_type);
void reset_input_pipe(struct display *display, int input_type);
void init_input_devices(struct display *display);

/* Listen for selections on this connection's wl_data_device. Defined in
 * WaydroidClipboard.cpp. */
void clipboard_watch_conn(struct wl_conn *conn);

/* Ask the conn worker to open a task's connection. Cheap and idempotent; the
 * connection appears in display->task_conns once it is up. */
void request_task_conn(struct display *display, uint32_t taskId);

/* Detach a task's connection, window and stream from every table and hand
 * them to the conn worker to destroy. Caller must hold windowsMutex. Returns
 * false, having done nothing, for a task that has no connection of its own.
 * Safe to call from the connection's own dispatch thread. */
bool detach_task_conn(struct display *display, uint32_t taskId);
void drop_all_task_conns(struct display *display);

/* Recompute host-side visibility and flip Android screen power to match.
 * Defined in wayland-hwc.cpp. */
void update_screen_power(struct display *display);
void force_screen_wakeup(struct display *display);

/* Post an SF-rendered task frame (IWaydroidDisplay@1.3). Returns 0 on
 * success, -EAGAIN when task streams are inactive, -ENOENT for an unknown or
 * closing task, -EBUSY for a busy slot, -EINVAL on import failure. The
 * buffer handle is only borrowed (fds are dup'ed by libwayland at marshal
 * time); fenceFd is borrowed too. */
int post_task_buffer(struct waydroid_hwc_composer_device_1 *pdev, uint32_t taskId,
                     uint32_t slot, const native_handle_t *handle, uint32_t width,
                     uint32_t height, uint32_t stride, int32_t format, int fenceFd,
                     std::vector<uint32_t> *releasedSlots);

/* Filter the caller's streamable tasks down to those the HAL wants content
 * for (IWaydroidDisplay@1.3 updateTaskList). Returns -EAGAIN when task
 * streams are inactive. */
int update_task_list(struct waydroid_hwc_composer_device_1 *pdev,
                     const std::vector<uint32_t> &tasks, std::vector<uint32_t> *wanted);

class open_windows {
    using Collection = std::map<std::string, std::unique_ptr<window>>;
    Collection windows;
    /* Apps we last published a waydroid.open.<appID> prop for. */
    std::set<std::string> published_apps;
    void publish_open_apps();

  public:
    using key_type = Collection::key_type;
    using mapped_type = Collection::mapped_type;
    using size_type = Collection::size_type;
    using iterator = Collection::iterator;
    using const_iterator = Collection::const_iterator;
    using reverse_iterator = Collection::reverse_iterator;
    using const_reverse_iterator = Collection::const_reverse_iterator;

    iterator begin() {
        return windows.begin();
    }
    iterator end() {
        return windows.end();
    }

    iterator find(const key_type& key) {
        return windows.find(key);
    }

    mapped_type& operator[](const key_type& key) {
        return windows[key];
    }
    mapped_type& operator[](key_type&& key) {
        return windows[std::move(key)];
    }

    size_type size() const {
        return windows.size();
    }

    template<class Func>
    void update(Func func) {
        func();

        std::string windows_size_str = std::to_string(windows.size());
        property_set("waydroid.open_windows", windows_size_str.c_str());
        publish_open_apps();
    }

    /* Zero window-state props orphaned by a previous composer instance.
     * Call before the first window is created. */
    static void clear_stale_props();

    window *add(waydroid_hwc_composer_device_1 *pdev, const std::string& key, const std::string& aid, const std::string& tid, hwc_color_t color = {0, 0, 0, 255});
    void add(const std::string& key, std::unique_ptr<window> window);
    /* Remove the window but hand it back alive, for a caller that must
     * destroy it later and elsewhere (see dying_task_conn). */
    mapped_type extract(const key_type& key);
    /* Drop one connection's windows, leaving every other connection's alone.
     * Like clear(), it does not touch screen power: the caller is tearing
     * them down only to recreate them. */
    void clear_for_conn(const struct wl_conn *conn);
    void clear();
    void erase(const_iterator pos);
    void erase(const key_type& key);
    template<class Pred>
    void erase_if(Pred pred) {
        struct display *display = nullptr;
        update([&](){
            for (auto it = windows.begin(), end = windows.end(); it != end;) {
                if (pred(*it)) {
                    display = it->second->conn->dpy;
                    it = windows.erase(it);
                } else {
                    ++it;
                }
            }
        });
        /* See open_windows::erase: destroyed surfaces emit no output leave. */
        if (display)
            update_screen_power(display);
    }
};

struct cursor_handler {
    virtual ~cursor_handler() = default;
    virtual std::unique_ptr<buffer> create_buffer(waydroid_hwc_composer_device_1 *pdev, const buffer_metadata& metadata, hwc_layer_1 *hwc_layer);
    virtual int apply_cursor(waydroid_hwc_composer_device_1 *pdev, hwc_layer_1 *hwc_layer, size_t hwc_layer_index) = 0;
    virtual int reset_cursor(waydroid_hwc_composer_device_1 *pdev) = 0;
    virtual int on_cursor_enter(display *display) = 0;
};

/* One Android task as the HAL knows it. Entries are pushed by WayDroidService
 * over IWaydroidWindow@1.3 (authoritative) or self-healed from TID layer
 * names when no event arrived (from_layer). Guarded by windowsMutex. */
struct task_info {
    std::string appID;
    std::string component;
    bool focused = false;
    /* Two-phase close: the host closed the card, Android was asked to remove
     * the task, and until the authoritative taskRemoved arrives this task's
     * layers must not recreate a window (teardown frames, task-ID reuse). */
    bool closing = false;
    bool from_layer = false;
    std::chrono::steady_clock::time_point closing_since {};
};

/* Per-task content stream (SF-rendered frames posted over
 * IWaydroidDisplay@1.3). One imported wl_buffer per slot; busy tracks
 * wl_buffer.release, released accumulates for the next post reply.
 * Guarded by windowsMutex. */
struct task_stream_slot {
    std::shared_ptr<buffer> buf;
    /* dmabuf inode of the posted handle: metadata alone cannot tell a
     * reallocated same-size buffer from the old one. */
    ino_t ino = 0;
    bool busy = false;
};
struct task_stream {
    std::map<uint32_t, task_stream_slot> slots;
    std::vector<uint32_t> released;
    /* Slot currently attached to the window; Mir (on this stack) never sends
     * wl_buffer.release, so the release of the previous slot is synthesized
     * when a new one is attached. */
    uint32_t attached_slot = UINT32_MAX;
};

/* A task connection detached from every table but not yet destroyed. Its
 * window and stream come with it: they hold proxies of the connection and
 * must die first, and only the conn worker may block on the dispatch thread
 * that is still unwinding. See PLAN-per-task-connections.md §3.2. */
struct dying_task_conn {
    std::unique_ptr<struct wl_conn> conn;
    std::unique_ptr<struct window> win;
    struct task_stream stream;
};


struct display {
    /* The one connection that has always existed: full UI, single window,
     * cursor and clipboard all live here. */
    std::unique_ptr<wl_conn> ctl;

    int system_version;
    GrallocType gtype;
    double scale;
    /* Once wp_fractional_scale has spoken, wl_output's integer scale must not
     * be allowed back in: it only ever truncates. Nothing recalibrates after a
     * reconnect, so without this one reconnect makes the truncation permanent. */
    bool scale_is_fractional = false;

    int input_fd[INPUT_TOTAL];
    bool reverseScroll;
    int scrollSensitivity;
    int zoomSensitivity;
    /* Guarded by input_mutex: dispatch threads of different connections
     * allocate from it, and ensure_pipe's lazy open races the same way. The
     * uinput writes themselves need no lock -- every batch is well under
     * PIPE_BUF, so a write cannot be interleaved. */
    std::array<struct touch_slot, MAX_TOUCHPOINTS> touch_id;
    std::mutex input_mutex;

    open_windows windows;

    /* Task table (see task_info). task_events_seen flips once the framework
     * pushes any @1.3 task event; until then consumers fall back to the old
     * prop/layer heuristics so an unpatched platform jar keeps working. */
    std::map<std::string, task_info> tasks;
    bool task_events_seen = false;
    /* Sequence number of the last taskListSnapshot; for the dump only. */
    uint32_t task_generation = 0;
    bool task_closing(const std::string &tid);
    void expire_closing_marks();
    void note_task_from_layer(const std::string &tid, const std::string &aid);
    bool forget_task(const std::string &tid);

    std::map<uint32_t, task_stream> task_streams;

    /* One wayland connection per streamed task, so the transport itself
     * carries the task identity. Guarded by windowsMutex; ctl is separate
     * and always present. */
    std::map<uint32_t, std::unique_ptr<wl_conn>> task_conns;

    /* select_mode chose task_streams_mode this frame. Written on the compose
     * thread, read on the binder threads, the conn worker and vsync -- so it
     * must stay atomic: a stale read opens a connection into full UI. */
    std::atomic<bool> task_streams_active {false};

    /* Opening a connection blocks on connect plus a registry roundtrip, and
     * reaping one joins a dispatch thread. Neither may happen under
     * windowsMutex or on a binder thread, so both are handed to a worker.
     * Lock order: conn_worker_mutex may be taken while holding windowsMutex,
     * never the other way round, and never across a blocking wayland call. */
    std::mutex conn_worker_mutex;
    std::condition_variable conn_worker_cond;
    std::set<uint32_t> conn_open_requests;
    std::vector<dying_task_conn> conn_graveyard;
    bool conn_worker_quit = false;
    std::thread conn_worker_thread;

    std::recursive_mutex windowsMutex;

    std::string clipboard;
    std::list<std::string> clipboard_offer_mime_types;
    /* Which connection the host gave keyboard focus to; empty means ctl. The
     * selection and its serial belong to the focused client, so with a
     * session per task the clipboard has to follow focus. Stored as a task ID
     * rather than a pointer and resolved under windowsMutex, so a detached
     * connection simply stops resolving. */
    std::optional<uint32_t> focus_task;
    struct {float x; float y;} cursor_hotspot;

    EGLDisplay egl_dpy;
    std::list<std::function<void()>> egl_work_queue;
    sem_t egl_go;
    sem_t egl_done;

    int width;
    int height;
    int full_width;
    int full_height;
    int req_width;
    int req_height;
    int refresh;

    std::map<uint32_t, std::string> layer_names;
    std::map<uint32_t, struct handleExt> layer_handles_ext;
    struct handleExt target_layer_handle_ext;
    std::array<uint8_t, 239> keysDown;

    std::unique_ptr<cursor_handler> cursor_handler;

    bool isMaximized;
    sp<IWaydroidTask> task;

    const hwc_procs_t *procs;
    bool needHotplug;

    /* Desired Android screen state, tracked so we only inject a sleep/wake
     * key on an actual on<->off transition driven by host visibility. */
    bool screen_on = true;

    /* Deactivate-side propagation. When the host shell deactivates the last
     * Waydroid toplevel (user went to the shell/drawer, not an app switch)
     * Android is dozed so the top task leaves RESUMED; otherwise a relaunch
     * is a no-op that never draws (stuck on splash). A deactivation only arms
     * a deadline; deactivate_thread re-checks engagement once it expires, so
     * A->B activation races and enter/leave flapping cancel it naturally.
     * See update_screen_power. */
    std::mutex deactivate_mutex;
    std::condition_variable deactivate_cond;
    std::chrono::steady_clock::time_point deactivate_deadline;
    bool deactivate_armed = false;
    bool deactivate_quit = false;
    std::thread deactivate_thread;
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

/*
 * Tear down ctl's wayland-side state and reconnect, rebinding globals. Task
 * connections are independent and are left running. Caller MUST hold
 * display->windowsMutex and MUST recreate the cursor handler afterwards (it
 * lives in hwcomposer.cpp and holds surfaces that this drops). On return
 * display->ctl->display is a fresh, bound connection.
 */
void
reconnect_display(struct display *display);

/*
 * Copyright (C) 2026 The Waydroid Project
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

// A title bar and resize edges that a window draws itself, for compositors that leave
// decorations to clients. GNOME's offers no xdg-decoration, so its windows would have neither.
// The title bar has Back, the window controls of persist.waydroid.app_titlebar_buttons and the
// colours of persist.waydroid.app_titlebar_theme, after libadwaita's header bar.

#include "wayland-hwc.h"

#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include <cutils/properties.h>
#include <log/log.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "cursor-shape-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

namespace {

// Logical pixels
constexpr int kTitleHeight = 46;
constexpr int kEdge = 10;            // invisible resize border around the window
constexpr int kCornerGrab = 16;      // how far along an edge a corner resize reaches
constexpr double kRadius = 12;       // the title bar's top corners
constexpr double kControl = 24;      // window control circles
constexpr double kControlSlot = 34;
constexpr double kBackWidth = 34, kBackHeight = 34, kBackRadius = 8;
constexpr double kSidePadding = 6;
constexpr double kFontEm = 14.67;    // 11 pt, as GNOME's title
constexpr int64_t kDoubleClickMs = 400;

const char *const kFonts[] = {"/system/fonts/SourceSansPro-Bold.ttf", "/system/fonts/Roboto-Regular.ttf"};

enum Part { NONE, TITLE, BACK, MINIMIZE, MAXIMIZE, CLOSE };

struct Rgba {
    double r, g, b, a;
};

struct Palette {
    Rgba bg, bg_inactive, fg, shade;
};
const Palette kDark = {{0x30 / 255.0, 0x30 / 255.0, 0x30 / 255.0, 1}, {0x24 / 255.0, 0x24 / 255.0, 0x24 / 255.0, 1},
                       {1, 1, 1, 1}, {0, 0, 0, 0.36}};
const Palette kLight = {{0xeb / 255.0, 0xeb / 255.0, 0xeb / 255.0, 1}, {0xfa / 255.0, 0xfa / 255.0, 0xfa / 255.0, 1},
                        {0, 0, 0, 0.8}, {0, 0, 0, 0.07}};

struct Button {
    Part part;
    double x, y, w, h; // logical, in the title bar
};

} // namespace

struct decor {
    uintptr_t window_id = 0;
    bool client_side = false;
    bool active = false, maximized = false, fullscreen = false, tiled = false;
    bool dark = true;
    std::vector<Part> left, right;
    struct wl_surface *title = nullptr, *frame = nullptr;
    struct wl_subsurface *title_sub = nullptr, *frame_sub = nullptr;
    struct wp_viewport *title_vp = nullptr, *frame_vp = nullptr;
    struct wl_buffer *clear = nullptr; // 1x1 transparent, stretched over the resize border
    std::vector<Button> buttons;
    Part hover = NONE, pressed = NONE;
    bool mapped = false;
    int width = 0, height = 0;
    double scale = 1;
    // What the title bar's buffer shows, to skip redrawing an unchanged one
    int drawn_w = 0;
    double drawn_scale = 0;
    bool drawn_active = false, drawn_maximized = false;
    Part drawn_hover = NONE, drawn_pressed = NONE;
    std::string drawn_title;
};

namespace {

// All of it requires liveWindowsMutex
struct {
    std::unordered_map<struct wl_surface *, uintptr_t> surfaces; // title bars and frames, by window

    FT_Library ft = nullptr;
    FT_Face face = nullptr;
    bool font_tried = false;

    // The pointer or touch on a decoration surface
    uintptr_t pointer_window = 0;
    struct wl_surface *pointer_surface = nullptr;
    uint32_t enter_serial = 0;
    uint32_t shape = 0; // cursor shape set since the enter, 0 for none
    double px = 0, py = 0;
    int64_t last_title_click = 0;
    double last_click_x = 0, last_click_y = 0;
    uintptr_t touch_window = 0;
    int32_t touch_id = -1;
} d;

int64_t now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

bool shown(const struct window *w) {
    return w->decor && w->decor->client_side && !w->decor->fullscreen && w->decor->title;
}

std::vector<Part> parse_side(const std::string &side) {
    std::vector<Part> out;
    size_t start = 0;
    while (start <= side.size()) {
        size_t comma = side.find(',', start);
        std::string token = side.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (token == "minimize")
            out.push_back(MINIMIZE);
        else if (token == "maximize")
            out.push_back(MAXIMIZE);
        else if (token == "close")
            out.push_back(CLOSE);
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    return out;
}

void load_font() {
    if (d.font_tried)
        return;
    d.font_tried = true;
    if (FT_Init_FreeType(&d.ft) != 0) {
        d.ft = nullptr;
        return;
    }
    for (const char *path : kFonts) {
        if (FT_New_Face(d.ft, path, 0, &d.face) == 0)
            return;
        d.face = nullptr;
    }
    ALOGW("no font for window titles");
}

void buffer_release(void *, struct wl_buffer *buffer) {
    wl_buffer_destroy(buffer);
}
const struct wl_buffer_listener buffer_listener = {buffer_release};

// Returns a premultiplied ARGB buffer of w x h pixels, mapped at *pixels, or nullptr.
struct wl_buffer *new_buffer(struct display *display, int w, int h, uint32_t **pixels) {
    size_t stride = size_t(w) * 4, size = stride * size_t(h);
    int fd = memfd_create("decor", MFD_CLOEXEC);
    if (fd < 0)
        return nullptr;
    if (ftruncate(fd, off_t(size)) < 0) {
        close(fd);
        return nullptr;
    }
    void *data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return nullptr;
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(display->shm, fd, int32_t(size));
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, w, h, int32_t(stride), WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    *pixels = static_cast<uint32_t *>(data);
    return buffer;
}

// ---- Drawing, in buffer pixels ----

struct Canvas {
    uint32_t *px;
    int w, h;
    double s; // buffer pixels per logical pixel

    void blend(int x, int y, Rgba c, double cover) {
        if (x < 0 || y < 0 || x >= w || y >= h || cover <= 0)
            return;
        double a = c.a * std::min(cover, 1.0);
        uint32_t &p = px[size_t(y) * size_t(w) + size_t(x)];
        double inv = 1 - a;
        auto mix = [&](int shift, double v) {
            return uint32_t(std::lround(v * a * 255 + double((p >> shift) & 0xff) * inv)) << shift;
        };
        p = mix(24, 1) | mix(16, c.r) | mix(8, c.g) | mix(0, c.b);
    }

    // A rectangle with the given corners rounded, in logical pixels.
    void round_rect(double x0, double y0, double x1, double y1, double r, bool top, bool bottom, Rgba c) {
        int ix0 = int(std::floor(x0 * s)), iy0 = int(std::floor(y0 * s));
        int ix1 = int(std::ceil(x1 * s)), iy1 = int(std::ceil(y1 * s));
        double rs = r * s;
        for (int y = iy0; y < iy1; y++)
            for (int x = ix0; x < ix1; x++) {
                double fx = x + 0.5, fy = y + 0.5;
                double cover = std::min({fx - x0 * s + 0.5, x1 * s - fx + 0.5, fy - y0 * s + 0.5, y1 * s - fy + 0.5, 1.0});
                double cx = fx < x0 * s + rs ? x0 * s + rs : fx > x1 * s - rs ? x1 * s - rs : fx;
                bool in_top = top && fy < y0 * s + rs, in_bottom = bottom && fy > y1 * s - rs;
                if ((in_top || in_bottom) && cx != fx) {
                    double cy = in_top ? y0 * s + rs : y1 * s - rs;
                    cover = std::min(cover, rs + 0.5 - std::hypot(fx - cx, fy - cy));
                }
                blend(x, y, c, cover);
            }
    }

    void circle(double cx, double cy, double r, Rgba c) {
        round_rect(cx - r, cy - r, cx + r, cy + r, r, true, true, c);
    }

    // A stroke of the given width between two points, in logical pixels.
    void line(double ax, double ay, double bx, double by, double width, Rgba c) {
        ax *= s, ay *= s, bx *= s, by *= s;
        double half = width * s / 2;
        int x0 = int(std::floor(std::min(ax, bx) - half - 1)), x1 = int(std::ceil(std::max(ax, bx) + half + 1));
        int y0 = int(std::floor(std::min(ay, by) - half - 1)), y1 = int(std::ceil(std::max(ay, by) + half + 1));
        double dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++) {
                double fx = x + 0.5, fy = y + 0.5;
                double t = len2 > 0 ? std::clamp(((fx - ax) * dx + (fy - ay) * dy) / len2, 0.0, 1.0) : 0;
                double dist = std::hypot(fx - (ax + t * dx), fy - (ay + t * dy));
                blend(x, y, c, half + 0.5 - dist);
            }
    }
};

std::u32string decode_utf8(const std::string &s) {
    std::u32string out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        char32_t cp = n == 1 ? c : c & (0x7f >> n);
        for (int k = 1; k < n && i + size_t(k) < s.size(); k++)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + size_t(k)]) & 0x3f);
        out.push_back(cp);
        i += size_t(n);
    }
    return out;
}

// Advance of a glyph and its kerning with the next one, in buffer pixels
double advance(FT_UInt glyph, FT_UInt next) {
    if (FT_Load_Glyph(d.face, glyph, FT_LOAD_DEFAULT) != 0)
        return 0;
    double x = d.face->glyph->advance.x / 64.0;
    FT_Vector kern;
    if (next && FT_HAS_KERNING(d.face) && FT_Get_Kerning(d.face, glyph, next, FT_KERNING_DEFAULT, &kern) == 0)
        x += kern.x / 64.0;
    return x;
}

double text_width(const std::vector<FT_UInt> &glyphs) {
    double width = 0;
    for (size_t i = 0; i < glyphs.size(); i++)
        width += advance(glyphs[i], i + 1 < glyphs.size() ? glyphs[i + 1] : 0);
    return width;
}

// Draws `title` centred in the window, kept between x_min and x_max (buffer pixels).
void draw_title(Canvas &cv, const std::string &title, double x_min, double x_max, Rgba fg) {
    load_font();
    if (!d.face || x_max <= x_min)
        return;
    FT_Set_Char_Size(d.face, 0, FT_F26Dot6(std::lround(kFontEm * cv.s * 64)), 72, 72);
    std::vector<FT_UInt> glyphs;
    for (char32_t cp : decode_utf8(title))
        glyphs.push_back(FT_Get_Char_Index(d.face, FT_ULong(cp)));
    double width = text_width(glyphs);
    if (width > x_max - x_min) {
        FT_UInt ellipsis = FT_Get_Char_Index(d.face, 0x2026);
        std::vector<FT_UInt> tail = ellipsis ? std::vector<FT_UInt>{ellipsis}
                                             : std::vector<FT_UInt>(3, FT_Get_Char_Index(d.face, '.'));
        auto with_tail = [&] {
            std::vector<FT_UInt> v = glyphs;
            v.insert(v.end(), tail.begin(), tail.end());
            return v;
        };
        while (!glyphs.empty() && text_width(with_tail()) > x_max - x_min)
            glyphs.pop_back();
        glyphs = with_tail();
        width = text_width(glyphs);
    }
    double x = std::clamp(cv.w / 2.0 - width / 2, x_min, std::max(x_min, x_max - width));
    double ascent = d.face->size->metrics.ascender / 64.0, descent = d.face->size->metrics.descender / 64.0;
    double baseline = cv.h / 2.0 + (ascent + descent) / 2;
    for (size_t i = 0; i < glyphs.size(); i++) {
        FT_UInt next = i + 1 < glyphs.size() ? glyphs[i + 1] : 0;
        if (FT_Load_Glyph(d.face, glyphs[i], FT_LOAD_RENDER) == 0) {
            const FT_Bitmap &bm = d.face->glyph->bitmap;
            int ox = int(std::lround(x)) + d.face->glyph->bitmap_left;
            int oy = int(std::lround(baseline)) - d.face->glyph->bitmap_top;
            if (bm.pixel_mode == FT_PIXEL_MODE_GRAY)
                for (unsigned gy = 0; gy < bm.rows; gy++)
                    for (unsigned gx = 0; gx < bm.width; gx++)
                        cv.blend(ox + int(gx), oy + int(gy), fg, bm.buffer[gy * unsigned(bm.pitch) + gx] / 255.0);
        }
        x += advance(glyphs[i], next);
    }
}

// 16x16 icons after libadwaita's symbolic ones, with (ox, oy) their top left in logical pixels.
void draw_icon(Canvas &cv, Part part, bool maximized, double ox, double oy, Rgba fg) {
    constexpr double kStroke = 1.6;
    auto ln = [&](double ax, double ay, double bx, double by) { cv.line(ox + ax, oy + ay, ox + bx, oy + by, kStroke, fg); };
    switch (part) {
    case BACK:
        ln(10, 3.5, 5.5, 8);
        ln(5.5, 8, 10, 12.5);
        break;
    case MINIMIZE:
        ln(4.5, 11, 11.5, 11);
        break;
    case MAXIMIZE:
        if (maximized) {
            ln(4.5, 6.5, 9.5, 6.5), ln(9.5, 6.5, 9.5, 11.5), ln(9.5, 11.5, 4.5, 11.5), ln(4.5, 11.5, 4.5, 6.5);
            ln(6.5, 4.5, 11.5, 4.5), ln(11.5, 4.5, 11.5, 9.5);
        } else {
            ln(4.5, 4.5, 11.5, 4.5), ln(11.5, 4.5, 11.5, 11.5), ln(11.5, 11.5, 4.5, 11.5), ln(4.5, 11.5, 4.5, 4.5);
        }
        break;
    case CLOSE:
        ln(5, 5, 11, 11);
        ln(11, 5, 5, 11);
        break;
    default:
        break;
    }
}

void layout_buttons(struct decor *c) {
    c->buttons.clear();
    double cy = kTitleHeight / 2.0;
    double x = kSidePadding;
    c->buttons.push_back({BACK, x, cy - kBackHeight / 2, kBackWidth, kBackHeight});
    x += kBackWidth;
    for (Part p : c->left) {
        c->buttons.push_back({p, x, cy - kControlSlot / 2, kControlSlot, kControlSlot});
        x += kControlSlot;
    }
    x = c->width - kSidePadding;
    for (auto it = c->right.rbegin(); it != c->right.rend(); ++it) {
        x -= kControlSlot;
        c->buttons.push_back({*it, x, cy - kControlSlot / 2, kControlSlot, kControlSlot});
    }
}

bool draw(struct window *w) {
    struct decor *c = w->decor;
    int bw = std::max(1, int(std::ceil(c->width * c->scale)));
    int bh = std::max(1, int(std::ceil(kTitleHeight * c->scale)));
    uint32_t *pixels;
    struct wl_buffer *buffer = new_buffer(w->display, bw, bh, &pixels);
    if (!buffer)
        return false;
    wl_buffer_add_listener(buffer, &buffer_listener, nullptr);
    std::memset(pixels, 0, size_t(bw) * size_t(bh) * 4);
    Canvas cv{pixels, bw, bh, bw / double(c->width)};
    const Palette &pal = c->dark ? kDark : kLight;
    Rgba fg = pal.fg;
    if (!c->active)
        fg.a *= 0.5;
    bool round = !c->maximized && !c->tiled;
    cv.round_rect(0, 0, c->width, kTitleHeight, round ? kRadius : 0, round, false, c->active ? pal.bg : pal.bg_inactive);
    cv.round_rect(0, kTitleHeight - 1, c->width, kTitleHeight, 0, false, false, pal.shade);

    layout_buttons(c);
    double left_end = kSidePadding, right_start = c->width - kSidePadding;
    for (const Button &b : c->buttons) {
        double tint = b.part == c->pressed ? 0.3 : b.part == c->hover ? 0.15 : 0;
        Rgba bg = {pal.fg.r, pal.fg.g, pal.fg.b, 0};
        double ix = b.x + b.w / 2 - 8, iy = b.y + b.h / 2 - 8;
        if (b.part == BACK) {
            if (tint > 0) {
                bg.a = tint * 0.7;
                cv.round_rect(b.x, b.y, b.x + b.w, b.y + b.h, kBackRadius, true, true, bg);
            }
        } else {
            bg.a = 0.1 + tint;
            cv.circle(b.x + b.w / 2, b.y + b.h / 2, kControl / 2, bg);
        }
        draw_icon(cv, b.part, c->maximized, ix, iy, fg);
        if (b.x < c->width / 2.0)
            left_end = std::max(left_end, b.x + b.w);
        else
            right_start = std::min(right_start, b.x);
    }
    draw_title(cv, w->title, (left_end + 8) * cv.s, (right_start - 8) * cv.s, fg);

    wl_surface_attach(c->title, buffer, 0, 0);
    wl_surface_damage_buffer(c->title, 0, 0, INT32_MAX, INT32_MAX);
    wp_viewport_set_destination(c->title_vp, c->width, kTitleHeight);
    wl_surface_commit(c->title);
    munmap(pixels, size_t(bw) * size_t(bh) * 4);
    c->drawn_w = c->width;
    c->drawn_scale = c->scale;
    c->drawn_active = c->active;
    c->drawn_maximized = c->maximized;
    c->drawn_hover = c->hover;
    c->drawn_pressed = c->pressed;
    c->drawn_title = w->title;
    return true;
}

bool stale(const struct window *w) {
    const struct decor *c = w->decor;
    return c->drawn_w != c->width || c->drawn_scale != c->scale || c->drawn_active != c->active ||
           c->drawn_maximized != c->maximized || c->drawn_hover != c->hover || c->drawn_pressed != c->pressed ||
           c->drawn_title != w->title;
}

// Sets the window geometry, and shows, moves or hides the title bar and the resize border. The
// positions and geometry apply with the window surface's next commit; the title bar and the border
// are desynchronized, so their own contents apply at once.
void place(struct window *w) {
    struct decor *c = w->decor;
    // A title bar without a buffer, as when memory runs out, stays hidden until the next configure.
    // A failed redraw keeps the last buffer.
    bool drawn = shown(w) && (c->mapped || draw(w));
    if (!drawn) {
        if (c && c->mapped) {
            wl_surface_attach(c->title, nullptr, 0, 0);
            wl_surface_commit(c->title);
            wl_surface_attach(c->frame, nullptr, 0, 0);
            wl_surface_commit(c->frame);
            c->mapped = false;
        }
        int width = c && c->width ? c->width : w->display->width;
        int height = c && c->height ? c->height : w->display->height;
        xdg_surface_set_window_geometry(w->xdg_surface, 0, 0, std::max(1, width), std::max(1, height));
        return;
    }
    xdg_surface_set_window_geometry(w->xdg_surface, 0, -kTitleHeight, c->width, c->height + kTitleHeight);
    wl_subsurface_set_position(c->title_sub, 0, -kTitleHeight);
    if (stale(w))
        draw(w);
    wl_subsurface_set_position(c->frame_sub, -kEdge, -kTitleHeight - kEdge);
    wl_surface_attach(c->frame, c->clear, 0, 0);
    wl_surface_damage_buffer(c->frame, 0, 0, INT32_MAX, INT32_MAX);
    wp_viewport_set_destination(c->frame_vp, c->width + 2 * kEdge, c->height + kTitleHeight + 2 * kEdge);
    struct wl_region *region = wl_compositor_create_region(w->display->compositor);
    if (!c->maximized && !c->tiled)
        wl_region_add(region, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_set_input_region(c->frame, region);
    wl_region_destroy(region);
    wl_surface_commit(c->frame);
    c->mapped = true;
}

// Redraws the title bar outside a configure, as for hover
void refresh(struct window *w) {
    if (shown(w) && w->decor->mapped && stale(w))
        draw(w);
}

struct window *window_of(struct wl_surface *surface) {
    auto it = d.surfaces.find(surface);
    struct window *w = it != d.surfaces.end() ? find_live_window(it->second) : nullptr;
    return w && w->decor ? w : nullptr;
}

Part part_at(const struct window *w, double x, double y) {
    for (const Button &b : w->decor->buttons)
        if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h)
            return b.part;
    return TITLE;
}

uint32_t edge_at(const struct window *w, double x, double y) {
    const struct decor *c = w->decor;
    double fw = c->width + 2 * kEdge, fh = c->height + kTitleHeight + 2 * kEdge;
    bool left = x < kEdge, right = x >= fw - kEdge, top = y < kEdge, bottom = y >= fh - kEdge;
    if (top || bottom) {
        left = left || x < kEdge + kCornerGrab;
        right = right || x >= fw - kEdge - kCornerGrab;
    }
    if (left || right) {
        top = top || y < kEdge + kCornerGrab;
        bottom = bottom || y >= fh - kEdge - kCornerGrab;
    }
    uint32_t edge = 0;
    if (top)
        edge |= XDG_TOPLEVEL_RESIZE_EDGE_TOP;
    if (bottom)
        edge |= XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM;
    if (left)
        edge |= XDG_TOPLEVEL_RESIZE_EDGE_LEFT;
    if (right)
        edge |= XDG_TOPLEVEL_RESIZE_EDGE_RIGHT;
    return edge;
}

uint32_t shape_for_edge(uint32_t edge) {
    switch (edge) {
    case XDG_TOPLEVEL_RESIZE_EDGE_TOP: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE;
    case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE;
    case XDG_TOPLEVEL_RESIZE_EDGE_LEFT: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE;
    case XDG_TOPLEVEL_RESIZE_EDGE_RIGHT: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE;
    case XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE;
    case XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE;
    case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE;
    case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE;
    default: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
    }
}

// The theme's arrow over the title bar, and its resize arrows over the border
void update_cursor(struct window *w) {
    struct wp_cursor_shape_device_v1 *device = w->display->cursor_shape_device;
    if (!device)
        return;
    uint32_t shape = d.pointer_surface == w->decor->frame ? shape_for_edge(edge_at(w, d.px, d.py))
                                                          : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
    if (shape != d.shape) {
        wp_cursor_shape_device_v1_set_shape(device, d.enter_serial, shape);
        d.shape = shape;
    }
}

void clear_pointer() {
    struct window *w = d.pointer_window ? find_live_window(d.pointer_window) : nullptr;
    d.pointer_window = 0;
    d.pointer_surface = nullptr;
    if (w && w->decor) {
        w->decor->hover = w->decor->pressed = NONE;
        refresh(w);
    }
}

void update_hover(struct window *w) {
    struct decor *c = w->decor;
    Part hover = d.pointer_surface == c->title ? part_at(w, d.px, d.py) : NONE;
    if (hover == TITLE)
        hover = NONE;
    if (hover != c->hover) {
        c->hover = hover;
        refresh(w);
    }
}

// What a click on a part does once liveWindowsMutex is released
struct Action {
    Part part = NONE;
    uintptr_t window_id = 0;
    struct display *display = nullptr;
};

Action activate(struct window *w, Part part) {
    switch (part) {
    case MINIMIZE:
        xdg_toplevel_set_minimized(w->xdg_toplevel);
        return {};
    case MAXIMIZE:
        w->set_maximize(!w->decor->maximized);
        return {};
    case BACK:
    case CLOSE:
        return {part, w->id, w->display};
    default:
        return {};
    }
}

void run(const Action &a) {
    if (a.part == BACK)
        send_back(a.display);
    else if (a.part == CLOSE)
        close_window(a.window_id);
}

} // namespace

void decor_create(struct window *w, bool client_side) {
    struct display *display = w->display;
    auto *c = new decor();
    w->decor = c;
    c->window_id = w->id;
    c->client_side = client_side;

    char value[PROPERTY_VALUE_MAX];
    property_get("persist.waydroid.app_titlebar_theme", value, "dark");
    c->dark = strcmp(value, "light") != 0;
    // GNOME's button-layout
    property_get("persist.waydroid.app_titlebar_buttons", value, ":minimize,maximize,close");
    std::string layout = value;
    size_t colon = layout.find(':');
    c->left = parse_side(colon == std::string::npos ? "" : layout.substr(0, colon));
    c->right = parse_side(colon == std::string::npos ? layout : layout.substr(colon + 1));

    uint32_t *pixels;
    if (!display->subcompositor || !display->viewporter || !display->shm ||
        !(c->clear = new_buffer(display, 1, 1, &pixels))) {
        // No title bar of its own, so leave it to the compositor where it draws one
        if (client_side && w->decoration)
            zxdg_toplevel_decoration_v1_set_mode(w->decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        return;
    }
    pixels[0] = 0;
    munmap(pixels, 4);

    c->frame = wl_compositor_create_surface(display->compositor);
    c->frame_sub = wl_subcompositor_get_subsurface(display->subcompositor, c->frame, w->surface);
    wl_subsurface_place_below(c->frame_sub, w->surface);
    wl_subsurface_set_desync(c->frame_sub);
    c->frame_vp = wp_viewporter_get_viewport(display->viewporter, c->frame);
    c->title = wl_compositor_create_surface(display->compositor);
    c->title_sub = wl_subcompositor_get_subsurface(display->subcompositor, c->title, w->surface);
    wl_subsurface_set_desync(c->title_sub);
    c->title_vp = wp_viewporter_get_viewport(display->viewporter, c->title);
    d.surfaces[c->frame] = w->id;
    d.surfaces[c->title] = w->id;
}

void decor_destroy(struct window *w) {
    struct decor *c = w->decor;
    if (!c)
        return;
    if (d.pointer_window == w->id) {
        d.pointer_window = 0;
        d.pointer_surface = nullptr;
    }
    if (d.touch_window == w->id)
        d.touch_window = 0;
    if (c->title) {
        d.surfaces.erase(c->title);
        d.surfaces.erase(c->frame);
        wp_viewport_destroy(c->title_vp);
        wl_subsurface_destroy(c->title_sub);
        wl_surface_destroy(c->title);
        wp_viewport_destroy(c->frame_vp);
        wl_subsurface_destroy(c->frame_sub);
        wl_surface_destroy(c->frame);
    }
    if (c->clear)
        wl_buffer_destroy(c->clear);
    delete c;
    w->decor = nullptr;
}

void decor_set_client_side(struct window *w, bool client_side) {
    if (w->decor)
        w->decor->client_side = client_side;
}

void decor_states(struct window *w, struct wl_array *states) {
    struct decor *c = w->decor;
    if (!c)
        return;
    c->active = c->maximized = c->fullscreen = c->tiled = false;
    const auto *state = static_cast<const uint32_t *>(states->data);
    for (size_t i = 0; i < states->size / sizeof(*state); i++) {
        switch (state[i]) {
        case XDG_TOPLEVEL_STATE_ACTIVATED: c->active = true; break;
        case XDG_TOPLEVEL_STATE_MAXIMIZED: c->maximized = true; break;
        case XDG_TOPLEVEL_STATE_FULLSCREEN: c->fullscreen = true; break;
        case XDG_TOPLEVEL_STATE_TILED_LEFT:
        case XDG_TOPLEVEL_STATE_TILED_RIGHT:
        case XDG_TOPLEVEL_STATE_TILED_TOP:
        case XDG_TOPLEVEL_STATE_TILED_BOTTOM: c->tiled = true; break;
        default: break;
        }
    }
    refresh(w);
}

int decor_title_height(const struct window *w) {
    return shown(w) ? kTitleHeight : 0;
}

void decor_set_size(struct window *w, int width, int height) {
    if (w->decor) {
        w->decor->width = width;
        w->decor->height = height;
        w->decor->scale = w->display->scale > 0 ? w->display->scale : 1;
    }
    place(w);
}

void decor_place(struct window *w) {
    place(w);
}

bool decor_pointer_enter(struct wl_surface *surface, uint32_t serial, double x, double y) {
    auto lock = lock_live_windows();
    struct window *w = window_of(surface);
    // Without a leave before it, as after the pointer was unplugged
    clear_pointer();
    if (!w)
        return false;
    d.pointer_window = w->id;
    d.pointer_surface = surface;
    d.enter_serial = serial;
    d.shape = 0;
    d.px = x;
    d.py = y;
    update_hover(w);
    update_cursor(w);
    return true;
}

bool decor_pointer_motion(double x, double y) {
    auto lock = lock_live_windows();
    if (!d.pointer_window)
        return false;
    d.px = x;
    d.py = y;
    if (struct window *w = find_live_window(d.pointer_window); w && w->decor) {
        update_hover(w);
        update_cursor(w);
    }
    return true;
}

bool decor_pointer_leave() {
    auto lock = lock_live_windows();
    if (!d.pointer_window)
        return false;
    clear_pointer();
    return true;
}

bool decor_pointer_button(uint32_t serial, uint32_t button, bool pressed) {
    Action action;
    {
        auto lock = lock_live_windows();
        if (!d.pointer_window)
            return false;
        struct window *w = find_live_window(d.pointer_window);
        if (!w || !w->decor)
            return true;
        struct decor *c = w->decor;
        struct wl_seat *seat = w->display->seat;
        if (d.pointer_surface == c->frame) {
            uint32_t edge = edge_at(w, d.px, d.py);
            if (pressed && button == BTN_LEFT && edge && seat)
                xdg_toplevel_resize(w->xdg_toplevel, seat, serial, edge);
            return true;
        }
        Part part = part_at(w, d.px, d.py);
        if (part == TITLE) {
            if (!pressed || !seat)
                return true;
            if (button == BTN_RIGHT) {
                xdg_toplevel_show_window_menu(w->xdg_toplevel, seat, serial, int32_t(d.px),
                                              int32_t(d.py - kTitleHeight));
            } else if (button == BTN_MIDDLE) {
                activate(w, MAXIMIZE);
            } else if (button == BTN_LEFT) {
                int64_t now = now_ms();
                bool twice = now - d.last_title_click < kDoubleClickMs && std::abs(d.px - d.last_click_x) < 8 &&
                             std::abs(d.py - d.last_click_y) < 8;
                d.last_title_click = twice ? 0 : now;
                d.last_click_x = d.px;
                d.last_click_y = d.py;
                if (twice)
                    activate(w, MAXIMIZE);
                else
                    xdg_toplevel_move(w->xdg_toplevel, seat, serial);
            }
            return true;
        }
        if (button != BTN_LEFT)
            return true;
        if (pressed) {
            c->pressed = part;
        } else {
            Part was = c->pressed;
            c->pressed = NONE;
            if (was == part)
                action = activate(w, part);
        }
        refresh(w);
    }
    run(action);
    return true;
}

bool decor_touch_down(struct wl_surface *surface, uint32_t serial, int32_t id, double x, double y) {
    auto lock = lock_live_windows();
    struct window *w = window_of(surface);
    if (!w)
        return false;
    if (d.touch_window)
        return true;
    struct decor *c = w->decor;
    struct wl_seat *seat = w->display->seat;
    if (surface == c->frame) {
        uint32_t edge = edge_at(w, x, y);
        if (edge && seat)
            xdg_toplevel_resize(w->xdg_toplevel, seat, serial, edge);
        return true;
    }
    Part part = part_at(w, x, y);
    if (part == TITLE) {
        if (seat)
            xdg_toplevel_move(w->xdg_toplevel, seat, serial);
        return true;
    }
    d.touch_window = w->id;
    d.touch_id = id;
    c->pressed = part;
    refresh(w);
    return true;
}

void decor_touch_cancel() {
    auto lock = lock_live_windows();
    if (!d.touch_window)
        return;
    if (struct window *w = find_live_window(d.touch_window); w && w->decor) {
        w->decor->pressed = NONE;
        refresh(w);
    }
    d.touch_window = 0;
    d.touch_id = -1;
}

bool decor_owns_surface(struct wl_surface *surface) {
    auto lock = lock_live_windows();
    return window_of(surface) != nullptr;
}

bool decor_touch_up(int32_t id) {
    Action action;
    {
        auto lock = lock_live_windows();
        if (!d.touch_window || id != d.touch_id)
            return false;
        struct window *w = find_live_window(d.touch_window);
        d.touch_window = 0;
        if (w && w->decor) {
            Part part = w->decor->pressed;
            w->decor->pressed = NONE;
            action = activate(w, part);
            refresh(w);
        }
    }
    run(action);
    return true;
}

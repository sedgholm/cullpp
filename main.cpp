#include <cstdint>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <cairo/cairo.h>
#include <pixman.h>
#include <xkbcommon/xkbcommon.h>
#include "shm-pool.hpp"
#include <sys/mman.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <unistd.h>
#include <signal.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <array>
#include <iostream>
#include <limits>

#include <cstddef>
#include <optional>


static uint32_t BgColor = 0x4A4A4A57;
static uint32_t BorderColor = 0x00000000;
static double BorderWidth = 1.0;

/* override at runtime via CULLPP_RESELECT_MOD env var. */
static constexpr const char* DefaultReselectMod = XKB_MOD_NAME_SHIFT; /* ALT, LOGO, CTRL */
static const char* ReselectMod = nullptr;

#ifdef __cplusplus
extern "C" {
#endif

#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include "cursor-shape-v1-client-protocol.h"

#ifdef __cplusplus
}
#endif

static volatile sig_atomic_t g_quit = 0;
static void onSignal(int) { g_quit = 1; }
static void onBusError(int, siginfo_t*, void*) { _exit(1); }

struct State;

struct Box { double x=0,y=0,w=0,h=0; bool empty()const{return w<=0||h<=0;} };
static bool isPointInBox(const Box&b,double px,double py){
    return px>=b.x&&px<b.x+b.w&&py>=b.y&&py<b.y+b.h;
}

struct SnapRect { Box rect; std::string label; bool hovered=false; };


struct Region {
    pixman_region32_t r;
    Region()  { pixman_region32_init(&r); }
    ~Region() { pixman_region32_fini(&r); }
    Region(const Region&) = delete;
    Region& operator=(const Region& o) { pixman_region32_copy(&r, &o.r); return *this; }
    void clear()       { pixman_region32_clear(&r); }
    bool isEmpty()     { return pixman_region32_not_empty(&r) == 0; }
};

struct Label {
    std::array<char, 128> text{};
    double tx = 0, ty = 0;               /* text origin */
    double bx = 0, by = 0, bw = 0, bh = 0; /* background box */
    cairo_text_extents_t te{};
};

struct SceneSnapshot {
    bool valid = false;                  /* :false:= next frame is a full repaint and full damage */
    bool has_sel = false;
    double sx = 0, sy = 0, sw = 0, sh = 0;
    bool show_dims = false;
    Label dims_label;
    bool cross = false;
    double cx = 0, cy = 0;
    Label cross_label;
    std::vector<char> hovered;
    Region sel_ext;
    Region cross_ext;
};

struct Output {
    State*                  st         = nullptr;
    wl_output*              wl_out     = nullptr;
    zxdg_output_v1*         xdg_out    = nullptr;
    zwlr_layer_surface_v1*  layer_surf = nullptr;
    wl_surface*             surface    = nullptr;
    wl_callback*            frame_cb   = nullptr;
    uint32_t                global_name= 0;

    int lx=0,ly=0,lw=0,lh=0;
    int px=0,py=0,pw=0,ph=0;
    int surf_w=0,surf_h=0;
    int scale=1;
    std::string name;

    ShmPool pool;
    bool configured= false;
    bool needs_draw= false;

    SceneSnapshot prev;
};

struct State {
    wl_display*                 display    = nullptr;
    wl_registry*                registry   = nullptr;
    wl_compositor*              compositor = nullptr;
    wl_shm*                     shm        = nullptr;
    zwlr_layer_shell_v1*        shell      = nullptr;
    zxdg_output_manager_v1*     xdg_mgr    = nullptr;
    wp_cursor_shape_manager_v1* cursor_mgr = nullptr;

    std::vector<Output*>  outputs;
    std::vector<SnapRect> snaps;

    bool   pressing  = false;
    double anchor_x=0, anchor_y=0;
    double cur_x=0,    cur_y=0;
    bool   has_sel   = false;
    Box    sel;

    bool   confirmed = false;
    bool   running   = true;

    xkb_context* xkb_ctx  = nullptr;
    xkb_keymap*   xkb_map  = nullptr;
    xkb_state*   xkb_st   = nullptr;

    Box    result;

    std::string format     = "%x,%y %wx%h";
    bool        show_dims  = false;
    bool        crosshair  = false;
    bool        point_mode = false;
    bool        output_mode= false;
    bool        force_stdin= false;
    bool        verbose   = false;

    cairo_font_face_t* ui_font   = nullptr;
    cairo_font_face_t* mono_font = nullptr;


};

static bool renderOutput(Output* out);
static std::string formatResult(const State* st);
static bool isReselectModifierHeld(State* st);

static inline void setSourceColorU32(cairo_t *cr, uint32_t c) {
    cairo_set_source_rgba(cr,
        ((c >> 24) & 0xFF) / 255.0,
        ((c >> 16) & 0xFF) / 255.0,
        ((c >>  8) & 0xFF) / 255.0,
        ((c >>  0) & 0xFF) / 255.0);
}

static std::optional<uint32_t> parseHexColor(const char* s) {
    if (!s) return std::nullopt;
    size_t len = strlen(s);
    if (len != 8) { fprintf(stderr, "cullpp: color must be 8 hex digits RRGGBBAA\n"); return std::nullopt; }
    char* end = nullptr;
    unsigned long v = strtoul(s, &end, 16);
    if (end != s + 8) { fprintf(stderr, "cullpp: invalid hex color '%s'\n", s); return std::nullopt; }
    return static_cast<uint32_t>(v);
}


static bool flushDisplay(State* st) {
    return wl_display_flush(st->display) >= 0 || errno == EAGAIN;
}

static void onFrameDone(void* data, wl_callback* cb, uint32_t) {
    auto* out = static_cast<Output*>(data);
    wl_callback_destroy(cb);
    out->frame_cb = nullptr;
    if (out->needs_draw && renderOutput(out) && !flushDisplay(out->st))
        out->st->running = false;
}
static const wl_callback_listener frame_listener = { .done = onFrameDone };

static bool doesBoxIntersectOutput(const Box& box, const Output* out, double pad = 0.0) {
    if (box.empty()) return false;
    const double left   = box.x - pad;
    const double top    = box.y - pad;
    const double right  = box.x + box.w + pad;
    const double bottom = box.y + box.h + pad;
    return right > out->lx && left < out->lx + out->lw &&
           bottom > out->ly && top < out->ly + out->lh;
}

static const char* outName(const Output* o) {
    return (o && !o->name.empty()) ? o->name.c_str() : "-";
}

static const char* buttonName(uint32_t b, char* buf, size_t n) {
    switch (b) {
    case BTN_LEFT:   return "BTN_LEFT";
    case BTN_RIGHT:  return "BTN_RIGHT";
    case BTN_MIDDLE: return "BTN_MIDDLE";
    default: snprintf(buf, n, "0x%x", b); return buf;
    }
}

static void logHover(size_t i, const SnapRect& sn, bool now_hovered, const char* label) {
    char w[24];
    snprintf(w, sizeof w, "snap %zu", i);
    if (now_hovered)
        dlog::line("hover", w, "%.0f,%.0f %.0fx%.0f  %s",
                   sn.rect.x, sn.rect.y, sn.rect.w, sn.rect.h, label);
    else
        dlog::line("unhover", w, "%s", "");
}

static void logResult(const State* st, const std::string& text) {
    if (dlog::level < 1) return;
    char q[160];
    std::string t = text;
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
    snprintf(q, sizeof q, "\"%s\"", t.c_str());
    dlog::line("out", q, "%.2f,%.2f %.2fx%.2f", st->result.x, st->result.y, st->result.w, st->result.h);
}

static void requestDrawOutput(Output* out) {
    if (out) out->needs_draw = true;
}

static void requestDrawAll(State* st) {
    for (auto* out : st->outputs) out->needs_draw = true;
}

/* renderOutput() works out *what* changed by diffing the
scene against the last committed one, so no dirty rectangles are accumulated here. */
static void requestBoxRedraw(State* st, const Box& box, double pad = 0.0) {
    for (auto* out : st->outputs)
        if (doesBoxIntersectOutput(box, out, pad)) out->needs_draw = true;
}

static double getSelectionDamagePadding() {
    return BorderWidth > 0.0 ? BorderWidth + 2.0 : 2.0;
}

static int32_t toScaledExtent(int value, int scale) {
    if (value <= 0 || scale <= 0) return 0;
    if (value > std::numeric_limits<int32_t>::max() / scale)
        return std::numeric_limits<int32_t>::max();
    return value * scale;
}

static void unmapOutputs(State* st) {
    for (auto* out : st->outputs) {
        if (!out->surface) continue;
        if (out->frame_cb) {
            wl_callback_destroy(out->frame_cb);
            out->frame_cb = nullptr;
        }
        wl_surface_attach(out->surface, nullptr, 0, 0);
        wl_surface_commit(out->surface);
    }
    wl_display_flush(st->display);
}


/* floor  ceil   pad  clamped to the output */
static void addRect(pixman_region32_t* r, const Output* out,
                    double x, double y, double w, double h, int pad) {
    const int x1 = std::max(0, static_cast<int>(std::floor(x)) - pad);
    const int y1 = std::max(0, static_cast<int>(std::floor(y)) - pad);
    const int x2 = std::min(out->surf_w, static_cast<int>(std::ceil(x + w)) + pad);
    const int y2 = std::min(out->surf_h, static_cast<int>(std::ceil(y + h)) + pad);
    if (x2 > x1 && y2 > y1)
        pixman_region32_union_rect(r, r, x1, y1,
            static_cast<unsigned>(x2 - x1), static_cast<unsigned>(y2 - y1));
}

static void addLabelExtents(pixman_region32_t* r, const Output* out, const Label& l) {
    addRect(r, out, l.bx, l.by, l.bw, l.bh, 3);
    addRect(r, out, l.tx + l.te.x_bearing, l.ty + l.te.y_bearing, l.te.width, l.te.height, 3);
}


static Label layoutCrosshairLabel(cairo_t* cr, const State* st, const Output* out,
                                  double cx, double cy) {
    Label l;
    if (st->pressing && st->has_sel) {
        snprintf(l.text.data(), l.text.size(), "%dx%d",
            static_cast<int>(st->sel.w), static_cast<int>(st->sel.h));
    } else if (st->pressing) {
        const int dx = static_cast<int>(std::abs(st->cur_x - st->anchor_x));
        const int dy = static_cast<int>(std::abs(st->cur_y - st->anchor_y));
        snprintf(l.text.data(), l.text.size(), "%d, %d  +%dx%d",
            static_cast<int>(st->cur_x), static_cast<int>(st->cur_y), dx, dy);
    } else {
        snprintf(l.text.data(), l.text.size(), "%d, %d",
            static_cast<int>(st->cur_x), static_cast<int>(st->cur_y));
    }
    cairo_set_font_face(cr, st->mono_font);
    cairo_set_font_size(cr, 11.0);
    cairo_text_extents(cr, l.text.data(), &l.te);
    double tx = cx + 12;
    double ty = cy - 8;
    if (tx + l.te.width + 6 > out->lw) tx = cx - l.te.width - 12;
    if (ty - l.te.height < 0) ty = cy + l.te.height + 14;
    if (tx < 0) tx = cx + 12;
    tx = std::floor(tx + 0.5);   // whole-pixel origin: deterministic glyph raster, clipped or not
    ty = std::floor(ty + 0.5);
    double padw = l.te.width + 6;
    const double padh = l.te.height + 4;
    if (st->pressing) padw += 30;
    l.tx = tx; l.ty = ty;
    l.bx = tx - 3; l.by = ty - l.te.height - 2; l.bw = padw; l.bh = padh;
    return l;
}

static Label layoutDimsLabel(cairo_t* cr, const State* st, const Output* out,
                             double sx, double sy) {
    Label l;
    snprintf(l.text.data(), l.text.size(), "%d × %d",
        static_cast<int>(st->sel.w), static_cast<int>(st->sel.h));
    cairo_set_font_face(cr, st->ui_font);
    cairo_set_font_size(cr, 13.0);
    cairo_text_extents(cr, l.text.data(), &l.te);
    double tx = sx + st->sel.w + 10;
    double ty = sy + st->sel.h + 10 + l.te.height;
    if (tx + l.te.width + 8 > out->lw) tx = sx - l.te.width - 14;
    if (ty + 4 > out->lh)              ty = sy - 8;
    if (tx < 0)                        tx = sx + 4;
    if (ty - l.te.height < 0)          ty = sy + l.te.height + 4;
    tx = std::floor(tx + 0.5);   /* whole-pixel origin [crosshair label] */
    ty = std::floor(ty + 0.5);
    constexpr double lp = 4;
    l.tx = tx; l.ty = ty;
    l.bx = tx - lp; l.by = ty - l.te.height - lp;
    l.bw = l.te.width + lp * 2; l.bh = l.te.height + lp * 2;
    return l;
}

static bool isBorderVisible() { return (BorderColor & 0xFF) != 0; }

static void buildScene(cairo_t* cr, const State* st, const Output* out, SceneSnapshot& s) {
    s.valid = true;

    if (st->has_sel) {
        s.has_sel = true;
        s.sx = st->sel.x - out->lx;
        s.sy = st->sel.y - out->ly;
        s.sw = st->sel.w;
        s.sh = st->sel.h;

        /* Edge band covers that +-1px uncertainty. */
        {
            const int p = isBorderVisible() ? static_cast<int>(std::ceil(BorderWidth / 2.0)) + 2 : 2;
            Region inner;
            addRect(&s.sel_ext.r, out, s.sx, s.sy, s.sw, s.sh, p);
            if (s.sw > 2 * p && s.sh > 2 * p)
                addRect(&inner.r, out, s.sx + p, s.sy + p, s.sw - 2 * p, s.sh - 2 * p, 0);
            pixman_region32_subtract(&s.sel_ext.r, &s.sel_ext.r, &inner.r);
        }
        if (st->show_dims) {
            s.show_dims = true;
            s.dims_label = layoutDimsLabel(cr, st, out, s.sx, s.sy);
            addLabelExtents(&s.sel_ext.r, out, s.dims_label);
        }
    }

    const double cx = st->cur_x - out->lx;
    const double cy = st->cur_y - out->ly;
    if (st->crosshair && cx >= 0 && cx < out->lw && cy >= 0 && cy < out->lh) {
        s.cross = true;
        s.cx = cx;
        s.cy = cy;
        s.cross_label = layoutCrosshairLabel(cr, st, out, cx, cy);
        addRect(&s.cross_ext.r, out, 0, cy - 1, out->lw, 4, 0);
        addRect(&s.cross_ext.r, out, cx - 1, 0, 4, out->lh, 0);
        addLabelExtents(&s.cross_ext.r, out, s.cross_label);
    }

    s.hovered.reserve(st->snaps.size());
    for (const auto& sn : st->snaps) s.hovered.push_back(sn.hovered ? 1 : 0);
}

static bool sameLabel(const Label& a, const Label& b) {
    return strcmp(a.text.data(), b.text.data()) == 0 &&
           a.tx == b.tx && a.ty == b.ty && a.bw == b.bw && a.bh == b.bh;
}


static void computeDamage(const State* st, const Output* out,
                          const SceneSnapshot& prev, const SceneSnapshot& cur, Region& dmg) {
#ifdef CULLPP_ORACLE   // test build: always full repaint with full damage
    const bool force_full = true;
#else
    const bool force_full = !prev.valid;
#endif
    if (force_full || prev.hovered.size() != cur.hovered.size()) {
        pixman_region32_union_rect(&dmg.r, &dmg.r, 0, 0,
            static_cast<unsigned>(out->surf_w), static_cast<unsigned>(out->surf_h));
        return;
    }

    const bool sel_changed = prev.has_sel != cur.has_sel ||
        (cur.has_sel && (prev.sx != cur.sx || prev.sy != cur.sy ||
                         prev.sw != cur.sw || prev.sh != cur.sh));
    if (sel_changed) {
        Region hp, hc, t;
        if (prev.has_sel) addRect(&hp.r, out, prev.sx, prev.sy, prev.sw, prev.sh, 0);
        if (cur.has_sel)  addRect(&hc.r, out, cur.sx, cur.sy, cur.sw, cur.sh, 0);
        pixman_region32_subtract(&t.r, &hp.r, &hc.r);   // hole that closed
        pixman_region32_union(&dmg.r, &dmg.r, &t.r);
        pixman_region32_subtract(&t.r, &hc.r, &hp.r);   // hole that opened
        pixman_region32_union(&dmg.r, &dmg.r, &t.r);
        pixman_region32_union(&dmg.r, &dmg.r, &prev.sel_ext.r);
        pixman_region32_union(&dmg.r, &dmg.r, &cur.sel_ext.r);
    }

    const bool cross_changed = prev.cross != cur.cross ||
        (cur.cross && (prev.cx != cur.cx || prev.cy != cur.cy ||
                       !sameLabel(prev.cross_label, cur.cross_label)));
    if (cross_changed) {
        pixman_region32_union(&dmg.r, &dmg.r, &prev.cross_ext.r);
        pixman_region32_union(&dmg.r, &dmg.r, &cur.cross_ext.r);
    }

    for (size_t i = 0; i < cur.hovered.size(); ++i) {
        if (prev.hovered[i] == cur.hovered[i]) continue;
        const Box& b = st->snaps[i].rect;
        addRect(&dmg.r, out, b.x - out->lx, b.y - out->ly, b.w, b.h, 2);
    }
}

static bool renderOutput(Output* out) {
    State* st = out->st;
    if (!out->configured) [[unlikely]] return false;

    out->needs_draw = false;
    const double t_render = dlog::level >= 1 ? dlog::nowMs() : 0.0;

    ShmBuffer* pb = out->pool.acquireNextBuffer();
    if (!pb) [[unlikely]] {   // all buffers in-flight
        LOG_V("skip", outName(out), "no free buffer");
        out->needs_draw = true;
        return false;
    }

    cairo_t* cr = pb->cr;
    if (!cr) [[unlikely]] {
        pb->busy.store(false, std::memory_order_release);
        out->needs_draw = true;
        return false;
    }

    /* apply HiDPI scale so all drawing coords are logical pixels. */
    cairo_reset_clip(cr);
    cairo_new_path(cr);
    cairo_identity_matrix(cr);
    cairo_scale(cr, static_cast<double>(out->scale), static_cast<double>(out->scale));

    SceneSnapshot cur;
    buildScene(cr, st, out, cur);

    Region dmg;
    computeDamage(st, out, out->prev, cur, dmg);
    if (dmg.isEmpty()) {
        pb->busy.store(false, std::memory_order_release);
        return false;
    }

    /* bound the request count */
    constexpr int kMaxDamageRects = 24;
    if (pixman_region32_n_rects(&dmg.r) > kMaxDamageRects) {
        const pixman_box32_t e = *pixman_region32_extents(&dmg.r);
        dmg.clear();
        pixman_region32_union_rect(&dmg.r, &dmg.r, e.x1, e.y1,
            static_cast<unsigned>(e.x2 - e.x1), static_cast<unsigned>(e.y2 - e.y1));
    }

    Region repaint;
    out->pool.repaintRegionFor(*pb, &dmg.r, out->surf_w, out->surf_h, &repaint.r);

    const pixman_box32_t* ext = pixman_region32_extents(&repaint.r);
    const bool repaint_all = pixman_region32_n_rects(&repaint.r) == 1 &&
        ext->x1 <= 0 && ext->y1 <= 0 && ext->x2 >= out->surf_w && ext->y2 >= out->surf_h;

    if (!repaint_all) {
        int n = 0;
        const pixman_box32_t* rects = pixman_region32_rectangles(&repaint.r, &n);
        for (int i = 0; i < n; ++i)
            cairo_rectangle(cr, rects[i].x1, rects[i].y1,
                rects[i].x2 - rects[i].x1, rects[i].y2 - rects[i].y1);
        cairo_clip(cr);
    }


    cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    setSourceColorU32(cr, BgColor);
    cairo_paint(cr);

    /* draw snap outlines and skip the ones entirely outside the repaint region */
    for (size_t i = 0; i < st->snaps.size(); ++i) {
        const auto& sn = st->snaps[i];
        const double rx = sn.rect.x - out->lx;
        const double ry = sn.rect.y - out->ly;
        const double rw = sn.rect.w;
        const double rh = sn.rect.h;
        if (!repaint_all) {
            const pixman_box32_t box = {
                static_cast<int32_t>(std::floor(rx)) - 2, static_cast<int32_t>(std::floor(ry)) - 2,
                static_cast<int32_t>(std::ceil(rx + rw)) + 2, static_cast<int32_t>(std::ceil(ry + rh)) + 2 };
            if (pixman_region32_contains_rectangle(&repaint.r, &box) == PIXMAN_REGION_OUT) continue;
        }
        if (sn.hovered) {
            cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
            cairo_set_source_rgba(cr, 0.27, 1.0, 0.27, 0.22);
            cairo_rectangle(cr, rx, ry, rw, rh);
            cairo_fill(cr);
        }
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        cairo_set_source_rgba(cr, 0.27, 1.0, 0.27, 0.9);
        cairo_set_line_width(cr, 1.5);
        cairo_rectangle(cr, rx, ry, rw, rh);
        cairo_stroke(cr);
    }

    if (cur.has_sel) {
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
        cairo_rectangle(cr, cur.sx, cur.sy, cur.sw, cur.sh);
        cairo_fill(cr);
        if (isBorderVisible()) {   /* skip the no-op stroke */
            cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
            setSourceColorU32(cr, BorderColor);
            cairo_set_line_width(cr, BorderWidth);
            cairo_rectangle(cr, cur.sx, cur.sy, cur.sw, cur.sh);
            cairo_stroke(cr);
        }

        if (cur.show_dims) {
            const Label& l = cur.dims_label;
            cairo_set_font_face(cr, st->ui_font);
            cairo_set_font_size(cr, 13.0);
            cairo_set_source_rgba(cr, 0, 0, 0, 0.72);
            cairo_rectangle(cr, l.bx, l.by, l.bw, l.bh);
            cairo_fill(cr);
            cairo_set_source_rgba(cr, 1, 1, 1, 1);
            cairo_move_to(cr, l.tx, l.ty);
            cairo_show_text(cr, l.text.data());
        }
    }

    if (cur.cross) {
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.5);
        cairo_set_line_width(cr, 1.0);
        cairo_move_to(cr, 0.5,         cur.cy + 0.5); cairo_line_to(cr, out->lw, cur.cy + 0.5);
        cairo_move_to(cr, cur.cx + 0.5, 0.5);         cairo_line_to(cr, cur.cx + 0.5, out->lh);
        cairo_stroke(cr);

        const Label& l = cur.cross_label;
        cairo_set_font_face(cr, st->mono_font);
        cairo_set_font_size(cr, 11.0);
        cairo_set_source_rgba(cr, 0.1, 0.1, 0.1, 0.85);
        cairo_rectangle(cr, l.bx, l.by, l.bw, l.bh);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, 0.0, 1.0, 0.0, 0.95);
        cairo_move_to(cr, l.tx, l.ty);
        cairo_show_text(cr, l.text.data());
    }

    cairo_surface_flush(pb->cairo_surface);

    /* Only register a new frame callback if one isn't already in-flight. */
    if (!out->frame_cb) {
        out->frame_cb = wl_surface_frame(out->surface);
        wl_callback_add_listener(out->frame_cb, &frame_listener, out);
    }

    wl_surface_attach(out->surface, pb->wl_buffer_handle, 0, 0);

    wl_surface_set_buffer_scale(out->surface, out->scale);

    {
        int n = 0;
        const pixman_box32_t* rects = pixman_region32_rectangles(&dmg.r, &n);
        for (int i = 0; i < n; ++i) {
            const int32_t bx = toScaledExtent(rects[i].x1, out->scale);
            const int32_t by = toScaledExtent(rects[i].y1, out->scale);
            const int32_t bw = toScaledExtent(rects[i].x2 - rects[i].x1, out->scale);
            const int32_t bh = toScaledExtent(rects[i].y2 - rects[i].y1, out->scale);
            wl_surface_damage_buffer(out->surface, bx, by, bw, bh);
        }
    }

    wl_surface_commit(out->surface);
    if (dlog::level >= 1) {
        const pixman_box32_t* de = pixman_region32_extents(&dmg.r);
        const int dn = pixman_region32_n_rects(&dmg.r);
        const bool dmg_full = dn == 1 && de->x1 <= 0 && de->y1 <= 0 &&
                              de->x2 >= out->surf_w && de->y2 >= out->surf_h;
        char what[24];
        if (dmg_full) snprintf(what, sizeof what, "full");
        else          snprintf(what, sizeof what, "%d rect%s", dn, dn == 1 ? "" : "s");
        dlog::line("commit", outName(out), "buf %d  %s%s  <%.6f>", pb->index, what,
                   (repaint_all && !dmg_full) ? "  paint full" : "", dlog::since(t_render));
    }
    out->pool.recordCommit(pb, &dmg.r);
    out->pool.advanceIndex();
    out->prev = cur;
    return true;
}

static bool renderPendingOutputs(State* st) {
    bool rendered_any = false;
    for (auto* out : st->outputs) {
        if (out->needs_draw && !out->frame_cb && out->configured)
            rendered_any |= renderOutput(out);
    }
    return !rendered_any || flushDisplay(st);
}

static void onLayerSurfaceConfigure(void* data, zwlr_layer_surface_v1* ls,
                          uint32_t serial, uint32_t w, uint32_t h) {
    auto* out = static_cast<Output*>(data);
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    LOG_V("configure", outName(out), "%ux%u  serial %u", w, h, serial);

    if (w > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        h > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        fprintf(stderr, "cullpp: layer surface configure is too large (%ux%u)\n", w, h);
        out->st->running = false;
        return;
    }

    out->surf_w = static_cast<int>(w);
    out->surf_h = static_cast<int>(h);
    if (!out->pool.reset(out->st->shm, out->surf_w, out->surf_h, out->scale)) {
        fprintf(stderr, "cullpp: shm pool reset failed (%ux%u scale=%d)\n", w, h, out->scale);
        out->st->running = false;
        return;
    }
    out->prev.valid = false;
    out->configured = true;
    out->needs_draw  = true;
}
static void onLayerSurfaceClosed(void* data, zwlr_layer_surface_v1*) {
    static_cast<Output*>(data)->st->running = false;
}
static const zwlr_layer_surface_v1_listener ls_listener = {
    .configure=onLayerSurfaceConfigure, .closed=onLayerSurfaceClosed };

static void createOutputSurface(Output* out) {
    State* st = out->st;
    out->surface = wl_compositor_create_surface(st->compositor);
    out->layer_surf = zwlr_layer_shell_v1_get_layer_surface(
        st->shell, out->surface, out->wl_out,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "cullpp");
    zwlr_layer_surface_v1_set_size(out->layer_surf, 0, 0);
    zwlr_layer_surface_v1_set_anchor(out->layer_surf,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(out->layer_surf, -1);

		zwlr_layer_surface_v1_set_keyboard_interactivity(out->layer_surf, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
    zwlr_layer_surface_v1_add_listener(out->layer_surf, &ls_listener, out);
    wl_surface_commit(out->surface);
}

struct Seat {
    State*   st      = nullptr;
    wl_seat* wl_s    = nullptr;
    wl_pointer* ptr  = nullptr;
    wl_keyboard* kbd = nullptr;
    Output*  ptr_out = nullptr;
    uint32_t ptr_ser = 0;
    uint32_t global_name = 0;
    wp_cursor_shape_device_v1* shape_dev = nullptr;
    wl_cursor_theme* cur_theme = nullptr;
    wl_cursor*       cur       = nullptr;
    wl_surface*      cur_surf  = nullptr;
};
static std::vector<Seat*> g_seats;

static void releaseWlOutput(wl_output*& output) {
    if (!output) return;
    if (wl_output_get_version(output) >= 3)
        wl_output_release(output);
    else
        wl_output_destroy(output);
    output = nullptr;
}

static void releasePointer(wl_pointer*& ptr) {
    if (!ptr) return;
    if (wl_pointer_get_version(ptr) >= 3)
        wl_pointer_release(ptr);
    else
        wl_pointer_destroy(ptr);
    ptr = nullptr;
}

static void releaseKeyboard(wl_keyboard*& kbd) {
    if (!kbd) return;
    if (wl_keyboard_get_version(kbd) >= 3)
        wl_keyboard_release(kbd);
    else
        wl_keyboard_destroy(kbd);
    kbd = nullptr;
}

static void releaseSeat(wl_seat*& seat) {
    if (!seat) return;
    if (wl_seat_get_version(seat) >= 5)
        wl_seat_release(seat);
    else
        wl_seat_destroy(seat);
    seat = nullptr;
}

static void destroyOutput(Output* out) {
    if (!out) return;

    for (auto* seat : g_seats)
        if (seat->ptr_out == out)
            seat->ptr_out = nullptr;

    if (out->frame_cb) {
        wl_callback_destroy(out->frame_cb);
        out->frame_cb = nullptr;
    }
    out->pool.destroy();
    if (out->layer_surf) {
        zwlr_layer_surface_v1_destroy(out->layer_surf);
        out->layer_surf = nullptr;
    }
    if (out->surface) {
        wl_surface_destroy(out->surface);
        out->surface = nullptr;
    }
    if (out->xdg_out) {
        zxdg_output_v1_destroy(out->xdg_out);
        out->xdg_out = nullptr;
    }
    releaseWlOutput(out->wl_out);
    delete out;
}

static void destroyPointer(Seat* seat) {
    if (!seat) return;

    if (seat->shape_dev) {
        wp_cursor_shape_device_v1_destroy(seat->shape_dev);
        seat->shape_dev = nullptr;
    }
    if (seat->cur_surf) {
        wl_surface_destroy(seat->cur_surf);
        seat->cur_surf = nullptr;
    }
    if (seat->cur_theme) {
        wl_cursor_theme_destroy(seat->cur_theme);
        seat->cur_theme = nullptr;
        seat->cur = nullptr;
    }
    releasePointer(seat->ptr);
    seat->ptr_out = nullptr;
    seat->ptr_ser = 0;
}

static void destroySeat(Seat* seat) {
    if (!seat) return;

    destroyPointer(seat);
    releaseKeyboard(seat->kbd);
    releaseSeat(seat->wl_s);
    delete seat;
}

static void setPointerCursor(Seat* seat, uint32_t serial) {
    if (seat->shape_dev)
        wp_cursor_shape_device_v1_set_shape(seat->shape_dev, serial,
            WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR);
    else if (seat->cur && seat->cur_surf) {
        wl_cursor_image* img = seat->cur->images[0];
        wl_buffer* cbuf = wl_cursor_image_get_buffer(img);
        if (cbuf) {
            int32_t scale = seat->ptr_out ? seat->ptr_out->scale : 1;
            uint32_t uscale = static_cast<uint32_t>(scale);
            wl_surface_set_buffer_scale(seat->cur_surf, scale);
            wl_surface_attach(seat->cur_surf, cbuf, 0, 0);
            wl_surface_damage_buffer(seat->cur_surf, 0, 0,
                static_cast<int32_t>(img->width),
                static_cast<int32_t>(img->height));
            wl_surface_commit(seat->cur_surf);

            wl_pointer_set_cursor(seat->ptr, serial, seat->cur_surf,
                static_cast<int32_t>(img->hotspot_x / uscale),
                static_cast<int32_t>(img->hotspot_y / uscale));
        }
    }
}

static void onPointerEnter(void* d, wl_pointer*, uint32_t ser,
                       wl_surface* surf, wl_fixed_t sx, wl_fixed_t sy) {
    auto* s = static_cast<Seat*>(d);
    State* st=s->st;
    s->ptr_ser=ser; s->ptr_out=nullptr;
    for (auto* out:st->outputs) {
        if (out->surface==surf) {
            s->ptr_out=out;




            const double gx = out->lx + wl_fixed_to_double(sx);
            const double gy = out->ly + wl_fixed_to_double(sy);
            st->cur_x = gx; st->cur_y = gy;
            LOG_V("enter", outName(out), "%.2f %.2f", gx, gy);
            for (size_t i = 0; i < st->snaps.size(); ++i) {
                auto& sn = st->snaps[i];
                const bool h = isPointInBox(sn.rect,gx,gy);
                if (h != sn.hovered) {
                    requestBoxRedraw(st, sn.rect);
                    if (dlog::level >= 1) logHover(i, sn, h, sn.label.c_str());
                }
                sn.hovered = h;
            }
            break;
        }
    }
    if (st->crosshair) requestDrawAll(st);
    setPointerCursor(s, ser);
}
static void onPointerLeave(void* d, wl_pointer*, uint32_t, wl_surface*) {
    auto* seat = static_cast<Seat*>(d);
    State* st=seat->st;
    Output* leaving = seat->ptr_out;
    seat->ptr_out = nullptr;
    LOG_V("leave", outName(leaving), "%s", "");

    for (size_t i = 0; i < st->snaps.size(); ++i) {
        auto& sn = st->snaps[i];
        if (sn.hovered) {
            sn.hovered=false;
            requestBoxRedraw(st, sn.rect);
            if (dlog::level >= 1) logHover(i, sn, false, "");
        }
    }

    if (st->crosshair && leaving)
        requestDrawAll(st);
}
static void onPointerMotion(void* d, wl_pointer*, uint32_t,
                        wl_fixed_t sx, wl_fixed_t sy) {
    auto* seat = static_cast<Seat*>(d);
    State* st=seat->st;
    if (!seat->ptr_out) return;
    const double prev_x = st->cur_x;
    const double prev_y = st->cur_y;
    const Box prev_sel = st->sel;
    const bool prev_has_sel = st->has_sel;
    double gx = seat->ptr_out->lx + wl_fixed_to_double(sx);
    double gy = seat->ptr_out->ly + wl_fixed_to_double(sy);
    st->cur_x=gx; st->cur_y=gy;
    bool selection_changed = false;
    if (st->pressing) {
        st->sel.x=std::min(st->anchor_x,gx); st->sel.y=std::min(st->anchor_y,gy);
        st->sel.w=std::abs(gx-st->anchor_x); st->sel.h=std::abs(gy-st->anchor_y);
        st->has_sel = st->sel.w>0 && st->sel.h>0;
        selection_changed =
            st->sel.x != prev_sel.x || st->sel.y != prev_sel.y ||
            st->sel.w != prev_sel.w || st->sel.h != prev_sel.h ||
            st->has_sel != prev_has_sel;
    }
    if (dlog::level >= 1) {
        if (st->pressing && st->has_sel)
            dlog::line("motion", outName(seat->ptr_out), "%.2f %.2f   sel %.2f %.2f %.2f %.2f",
                       gx, gy, st->sel.x, st->sel.y, st->sel.w, st->sel.h);
        else
            dlog::line("motion", outName(seat->ptr_out), "%.2f %.2f", gx, gy);
    }
    bool hover_changed = false;
    for (size_t i = 0; i < st->snaps.size(); ++i) {
        auto& sn = st->snaps[i];
        const bool hovered = isPointInBox(sn.rect,gx,gy);
        const bool changed = sn.hovered != hovered;
        if (changed) {
            requestBoxRedraw(st, sn.rect);
            if (dlog::level >= 1) logHover(i, sn, hovered, sn.label.c_str());
        }
        hover_changed |= changed;
        sn.hovered = hovered;
    }
    const bool cursor_changed = gx != prev_x || gy != prev_y;
    if (selection_changed) {
        const double pad = getSelectionDamagePadding();
        if (prev_has_sel) requestBoxRedraw(st, prev_sel, pad);
        if (st->has_sel) requestBoxRedraw(st, st->sel, pad);
        if (st->show_dims || st->crosshair)
            requestDrawAll(st);
    } else if (hover_changed || (st->crosshair && cursor_changed)) {
        if (st->crosshair)
            requestDrawAll(st);
    }
}
static bool emitResultAndContinue(State* st) {
    st->confirmed = true;
    if (!isReselectModifierHeld(st)) { st->running = false; return false; }
    {
        const std::string text = formatResult(st);
        logResult(st, text);
        std::cout << text << std::flush;
    }
    st->confirmed = false;
    st->has_sel = false;
    st->sel = {0, 0, 0, 0};
    for (auto* o : st->outputs) requestDrawOutput(o);
    return true;
}

static void onPointerButton(void* d, wl_pointer*, uint32_t, uint32_t,
                        uint32_t btn, uint32_t state_val) {
    auto* seat = static_cast<Seat*>(d);
    State* st=seat->st;
    if (dlog::level >= 1) {
        char nb[16];
        dlog::line("button", buttonName(btn, nb, sizeof nb), "%-8s  %.2f %.2f",
                   state_val == WL_POINTER_BUTTON_STATE_PRESSED ? "PRESSED" : "RELEASED",
                   st->cur_x, st->cur_y);
    }
    if (btn==BTN_RIGHT && state_val==WL_POINTER_BUTTON_STATE_PRESSED) {
        st->running=false; return;
    }
    if (btn!=BTN_LEFT) return;
    if (state_val==WL_POINTER_BUTTON_STATE_PRESSED) {
        st->anchor_x=st->cur_x; st->anchor_y=st->cur_y;
        st->pressing=true; st->has_sel=false;
        requestDrawOutput(seat->ptr_out);
    } else {
        const Box cleared_sel = st->sel;
        st->pressing=false;
        if (st->point_mode) {
            st->result={st->cur_x,st->cur_y,1,1};
            if (!emitResultAndContinue(st)) return;
            return;
         }
         if ( st->has_sel && st->sel.w>0 && st->sel.h>0 ) {
            st->result = st->sel;
            if (!emitResultAndContinue(st)) return;
            return;
         } else {
            bool has_hovered = false;
             for (const auto& sn:st->snaps) if(sn.hovered){
                st->result = sn.rect;
                if (!emitResultAndContinue(st)) return;
                has_hovered = true; break;
             }
            if (has_hovered) return;

            st->has_sel=false;
            st->result={st->cur_x,st->cur_y,1,1};
            st->confirmed=true; st->running=false;
            requestBoxRedraw(st, cleared_sel, getSelectionDamagePadding());
            requestDrawOutput(seat->ptr_out);
            return;
        }
    }
}

static void onPointerAxis                 (void*,wl_pointer*,uint32_t,uint32_t,wl_fixed_t){}
static void onPointerFrame                (void*,wl_pointer*){}
static void onPointerAxisSource           (void*,wl_pointer*,uint32_t){}
static void onPointerAxisStop             (void*,wl_pointer*,uint32_t,uint32_t){}
static void onPointerAxisDiscrete         (void*,wl_pointer*,uint32_t,int32_t){}
static void onPointerAxisValue120         (void*,wl_pointer*,uint32_t,int32_t){}
static void onPointerAxisRelativeDirection(void*,wl_pointer*,uint32_t,uint32_t){}
static const wl_pointer_listener ptr_listener = {
    .enter=onPointerEnter,.leave=onPointerLeave,.motion=onPointerMotion,.button=onPointerButton,
    .axis=onPointerAxis,.frame=onPointerFrame,.axis_source=onPointerAxisSource,
    .axis_stop=onPointerAxisStop,.axis_discrete=onPointerAxisDiscrete,
    .axis_value120=onPointerAxisValue120,.axis_relative_direction=onPointerAxisRelativeDirection,
};


static void onKeyboardKeymap(void* data, wl_keyboard*, uint32_t format, int32_t fd, uint32_t size) {
    auto* st = static_cast<State*>(data);
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }
    void* buf = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (buf == MAP_FAILED) return;
    xkb_keymap* map = xkb_keymap_new_from_string(
        st->xkb_ctx, static_cast<const char*>(buf),
        XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(buf, size);
    if (!map) return;
    xkb_state* xst = xkb_state_new(map);
    if (!xst) { xkb_keymap_unref(map); return; }
    if (st->xkb_st)  xkb_state_unref(st->xkb_st);
    if (st->xkb_map) xkb_keymap_unref(st->xkb_map);
    st->xkb_map = map;
    st->xkb_st  = xst;
}
static bool isReselectModifierHeld(State* st) {
    return st->xkb_st &&
        xkb_state_mod_name_is_active(st->xkb_st, ReselectMod,
            XKB_STATE_MODS_EFFECTIVE) > 0;
}

static void onKeyboardKey(void* data, wl_keyboard*, uint32_t, uint32_t, uint32_t keycode, uint32_t key_state) {
    auto* st = static_cast<State*>(data);
    if (key_state != WL_KEYBOARD_KEY_STATE_PRESSED) return;
    if (!st->xkb_st) { LOG_V("key", "?", "%u  PRESSED  no keymap", keycode); return; }
    xkb_keycode_t xkb_kc = keycode + 8;
    const xkb_keysym_t* syms;
    int nsyms = xkb_state_key_get_syms(st->xkb_st, xkb_kc, &syms);
    for (int i = 0; i < nsyms; ++i) {
        if (dlog::level >= 1) {
            char nm[64];
            xkb_keysym_get_name(syms[i], nm, sizeof nm);
            dlog::line("key", nm, "PRESSED");
        }
        if (syms[i] == XKB_KEY_Escape) {
            st->running = false;
        }
        if (syms[i] == XKB_KEY_c || syms[i] == XKB_KEY_C) {
            if (xkb_state_mod_name_is_active(st->xkb_st, XKB_MOD_NAME_CTRL,
                    XKB_STATE_MODS_EFFECTIVE)) {
                st->running = false;
            }
        }
    }
}
static void onKeyboardModifiers(void* data, wl_keyboard*, uint32_t, uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group) {
    auto* st = static_cast<State*>(data);
    if (st->xkb_st)
        xkb_state_update_mask(st->xkb_st, mods_depressed, mods_latched, mods_locked, 0, 0, group);
}
static void onKeyboardEnter     (void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*) {}
static void onKeyboardLeave     (void*, wl_keyboard*, uint32_t, wl_surface*) {}
static void onKeyboardRepeatInfo(void*, wl_keyboard*, int32_t, int32_t) {}
static const wl_keyboard_listener kbd_listener = {
    .keymap = onKeyboardKeymap,
    .enter = onKeyboardEnter,
    .leave = onKeyboardLeave,
    .key = onKeyboardKey,
    .modifiers = onKeyboardModifiers,
    .repeat_info = onKeyboardRepeatInfo,
};


static void onSeatCapabilities(void* d, wl_seat* ws, uint32_t caps) {
    auto* seat=static_cast<Seat*>(d); State* st=seat->st;
    if ((caps&WL_SEAT_CAPABILITY_POINTER) && !seat->ptr) {
        seat->ptr = wl_seat_get_pointer(ws);
        wl_pointer_add_listener(seat->ptr, &ptr_listener, seat);
        if (st->cursor_mgr)
            seat->shape_dev = wp_cursor_shape_manager_v1_get_pointer(
                st->cursor_mgr, seat->ptr);
        else {
            const char* th=getenv("XCURSOR_THEME");
            int sz=24;
            const char* ss=getenv("XCURSOR_SIZE");
            if (ss) {
                char* end = nullptr;
                long v = strtol(ss, &end, 10);
                if (end != ss && v > 0 && v <= 256) sz = static_cast<int>(v);
            }
            seat->cur_theme = wl_cursor_theme_load(th,sz,st->shm);
            if (seat->cur_theme) {
                seat->cur = wl_cursor_theme_get_cursor(seat->cur_theme,"crosshair");
                if(!seat->cur) seat->cur=wl_cursor_theme_get_cursor(seat->cur_theme,"cross");
            }
            if (seat->cur) seat->cur_surf=wl_compositor_create_surface(st->compositor);
        }
    }
    if (!(caps & WL_SEAT_CAPABILITY_POINTER) && seat->ptr)
        destroyPointer(seat);

    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !seat->kbd) {
        if (!st->xkb_ctx)
            st->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        seat->kbd = wl_seat_get_keyboard(ws);
        wl_keyboard_add_listener(seat->kbd, &kbd_listener, st);
    }
    if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && seat->kbd)
        releaseKeyboard(seat->kbd);
}
static void onSeatName(void*,wl_seat*,const char*){}
static const wl_seat_listener seat_listener={.capabilities=onSeatCapabilities,.name=onSeatName};

static void onXdgOutputLogicalPosition (void*d,zxdg_output_v1*,int32_t x,int32_t y){auto*o=static_cast<Output*>(d);o->lx=x;o->ly=y;}
static void onXdgOutputLogicalSize(void*d,zxdg_output_v1*,int32_t w,int32_t h){auto*o=static_cast<Output*>(d);o->lw=w;o->lh=h;}
static void onXdgOutputDone(void*d,zxdg_output_v1*){
    auto* o=static_cast<Output*>(d);
    if (o->st->verbose) fprintf(stderr,
        "XDG_OUTPUT_DONE: name=%s logical=(%d,%d %dx%d)\n",
        o->name.c_str(), o->lx, o->ly, o->lw, o->lh);
}
static void onXdgOutputName(void*d,zxdg_output_v1*,const char*n){if(n)static_cast<Output*>(d)->name=n;}
static void onXdgOutputDescription(void*,zxdg_output_v1*,const char*){}
static const zxdg_output_v1_listener xo_listener={
    .logical_position=onXdgOutputLogicalPosition,.logical_size=onXdgOutputLogicalSize,
    .done=onXdgOutputDone,.name=onXdgOutputName,.description=onXdgOutputDescription};

static void onWlOutputGeometry(void*d,wl_output*,int32_t x,int32_t y,int32_t,int32_t,int32_t,const char*,const char*,int32_t){auto*o=static_cast<Output*>(d);o->px=x;o->py=y;}
static void onWlOutputMode(void*d,wl_output*,uint32_t f,int32_t w,int32_t h,int32_t){if(f&WL_OUTPUT_MODE_CURRENT){auto*o=static_cast<Output*>(d);o->pw=w;o->ph=h;}}
static void onWlOutputDone(void*d,wl_output*){
    auto*o=static_cast<Output*>(d);
    const bool used_fallback = (o->lw==0 && o->pw>0);
    if (used_fallback) {o->lx=o->px/o->scale; o->ly=o->py/o->scale; o->lw=o->pw/o->scale; o->lh=o->ph/o->scale;}
    if (o->st->verbose) fprintf(stderr,
        "WL_OUTPUT_DONE: name=%s physical=(%d,%d %dx%d) scale=%d logical=(%d,%d %dx%d) used_fallback=%d\n",
        o->name.c_str(), o->px, o->py, o->pw, o->ph, o->scale, o->lx, o->ly, o->lw, o->lh, used_fallback);
}
static void onWlOutputScale(void*d,wl_output*,int32_t f){
    auto* o=static_cast<Output*>(d);
    if (f <= 0 || o->scale == f) return;

    o->scale=f;
    if (!o->configured || o->surf_w <= 0 || o->surf_h <= 0) return;

    if (!o->pool.reset(o->st->shm, o->surf_w, o->surf_h, o->scale)) {
        fprintf(stderr, "cullpp: shm pool reset failed (%dx%d scale=%d)\n",
            o->surf_w, o->surf_h, o->scale);
        o->st->running = false;
        return;
    }
    o->prev.valid = false;
    requestDrawOutput(o);
}
static void onWlOutputName(void*d,wl_output*,const char*n){auto*o=static_cast<Output*>(d);if(o->name.empty()&&n)o->name=n;}
static void onWlOutputDescription(void*,wl_output*,const char*){}
static const wl_output_listener wo_listener={.geometry=onWlOutputGeometry,.mode=onWlOutputMode,.done=onWlOutputDone,.scale=onWlOutputScale,.name=onWlOutputName,.description=onWlOutputDescription};

static void onRegistryGlobal(void* data, wl_registry* reg,
                        uint32_t name, const char* iface, uint32_t version) {
    auto* st=static_cast<State*>(data);
    if      (!strcmp(iface,wl_compositor_interface.name))
        st->compositor=static_cast<wl_compositor*>(wl_registry_bind(reg,name,&wl_compositor_interface,
            std::min(version, 4u)));
    else if (!strcmp(iface,wl_shm_interface.name))
        st->shm=static_cast<wl_shm*>(wl_registry_bind(reg,name,&wl_shm_interface,1));
    else if (!strcmp(iface,zwlr_layer_shell_v1_interface.name))
        st->shell=static_cast<zwlr_layer_shell_v1*>(wl_registry_bind(reg,name,&zwlr_layer_shell_v1_interface,
            std::min(version, static_cast<uint32_t>(zwlr_layer_shell_v1_interface.version))));
    else if (!strcmp(iface,zxdg_output_manager_v1_interface.name))
        st->xdg_mgr=static_cast<zxdg_output_manager_v1*>(wl_registry_bind(reg,name,&zxdg_output_manager_v1_interface,
            std::min(version, static_cast<uint32_t>(zxdg_output_manager_v1_interface.version))));
    else if (!strcmp(iface,wp_cursor_shape_manager_v1_interface.name))
        st->cursor_mgr=static_cast<wp_cursor_shape_manager_v1*>(wl_registry_bind(reg,name,&wp_cursor_shape_manager_v1_interface,
            std::min(version, static_cast<uint32_t>(wp_cursor_shape_manager_v1_interface.version))));
    else if (!strcmp(iface,wl_output_interface.name)) {
        Output* out=new Output(); out->st=st;
        out->global_name=name;
        out->wl_out=static_cast<wl_output*>(wl_registry_bind(reg,name,&wl_output_interface,std::min(version, 4u)));
        wl_output_add_listener(out->wl_out,&wo_listener,out);
        st->outputs.push_back(out);
    }
    else if (!strcmp(iface,wl_seat_interface.name)) {
        Seat* seat=new Seat(); seat->st=st;
        seat->global_name=name;
        seat->wl_s=static_cast<wl_seat*>(wl_registry_bind(reg,name,&wl_seat_interface,std::min(version, 7u)));
        wl_seat_add_listener(seat->wl_s,&seat_listener,seat);
        g_seats.push_back(seat);
    }
}
static void onRegistryGlobalRemove(void* data,wl_registry*,uint32_t name){
    auto* st=static_cast<State*>(data);

    auto out_it = std::find_if(st->outputs.begin(), st->outputs.end(),
        [name](const Output* out) { return out->global_name == name; });
    if (out_it != st->outputs.end()) {
        Output* out = *out_it;
        st->outputs.erase(out_it);
        destroyOutput(out);
        if (st->outputs.empty())
            st->running = false;
        return;
    }

    auto seat_it = std::find_if(g_seats.begin(), g_seats.end(),
        [name](const Seat* seat) { return seat->global_name == name; });
    if (seat_it != g_seats.end()) {
        Seat* seat = *seat_it;
        g_seats.erase(seat_it);
        destroySeat(seat);
    }
}
static const wl_registry_listener reg_listener={.global=onRegistryGlobal,.global_remove=onRegistryGlobalRemove};

static void parseStdin(State* st) {
    if (!st->force_stdin && isatty(STDIN_FILENO)) return;
    std::string line;
    int lineno = 0, rejected = 0;
    while (std::getline(std::cin, line)) {
        ++lineno;
        double x,y,w,h;
        if (sscanf(line.c_str(), "%lf,%lf %lfx%lf", &x, &y, &w, &h) != 4) {
            if (line.find_first_not_of(" \t\r") != std::string::npos) {
                ++rejected;
                LOG_V("stdin", "", "line %d: bad rect \"%s\"", lineno, line.c_str());
            }
            continue;
        }
        SnapRect sn; sn.rect={x,y,w,h};
        auto first = line.find(' ');
        if (first != std::string::npos) {
            auto second = line.find(' ', first + 1);
            if (second != std::string::npos) {
                sn.label = line.substr(second + 1);
            }
        }
        st->snaps.push_back(sn);
    }
    LOG_V("stdin", "", "%zu rects, %d rejected", st->snaps.size(), rejected);
}

static std::string formatResult(const State* st) {
    const Box& r=st->result;
    const Output* best_out=nullptr; double best=-1;
    for (const auto* o:st->outputs) {
        double ix=std::max(r.x,static_cast<double>(o->lx)), iy=std::max(r.y,static_cast<double>(o->ly));
        double ix2=std::min(r.x+r.w,static_cast<double>(o->lx+o->lw));
        double iy2=std::min(r.y+r.h,static_cast<double>(o->ly+o->lh));
        double a=(ix2>ix&&iy2>iy)?(ix2-ix)*(iy2-iy):0;
        if (st->verbose) fprintf(stderr,
            "RESULT_CHECK: output=%s logical=%d,%d %dx%d scale=%d overlapArea=%.0f\n",
            o->name.c_str(), o->lx, o->ly, o->lw, o->lh, o->scale, a);
        if(a>best){best=a;best_out=o;}
    }
    if (st->verbose) fprintf(stderr,
        "RESULT_CHECK: final selection=%.0f,%.0f %.0fx%.0f chosenOutput=%s\n",
        r.x, r.y, r.w, r.h, best_out ? best_out->name.c_str() : "(none)");
    const SnapRect* snap=nullptr;
    {
        /* half-pixel epsilon: int-cast fails for e.g. 1.9 vs 2.0 (floors differ
         * but should match) and allows 100.4 vs 100.6 (same floor, shouldn't match). */
        auto feq=[](double a,double b){ return std::abs(a-b)<0.5; };
        for (const auto& sn:st->snaps)
            if(feq(sn.rect.x,r.x)&&feq(sn.rect.y,r.y)&&
               feq(sn.rect.w,r.w)&&feq(sn.rect.h,r.h)){snap=&sn;break;}
    }

    std::string out;
    for (const char* p=st->format.c_str(); *p; ++p) {
        if (*p=='%'&&*(p+1)) {
            switch(*++p) {
            case 'x':out += std::to_string(static_cast<int>(r.x));break;
            case 'y':out += std::to_string(static_cast<int>(r.y));break;
            case 'w':out += std::to_string(static_cast<int>(r.w));break;
            case 'h':out += std::to_string(static_cast<int>(r.h));break;
            case 'X':out += std::to_string(static_cast<int>(r.x+r.w));break;
            case 'Y':out += std::to_string(static_cast<int>(r.y+r.h));break;
            case 'o':out += best_out ? best_out->name : "";break;
            case 'l':out += snap ? snap->label : "";break;
            case '%':out += '%';break;
            default: out += '%'; out += *p;break;
            }
        } else out += *p;
    }
    out += '\n';
    return out;
}

int main(int argc, char* argv[]) {
    dlog::start();

	  //env
    const char* env_mod = getenv("CULLPP_RESELECT_MOD");
    ReselectMod = env_mod ? env_mod : DefaultReselectMod;

		const bool useColor = isatty(fileno(stderr));
		const char* errPrefix = useColor ? "\033[1merr:\033[0m" : "err:";
    struct sigaction sa{};
    sa.sa_handler=onSignal; sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,&sa,nullptr); sigaction(SIGTERM,&sa,nullptr);

    struct sigaction bus{};
    bus.sa_sigaction=onBusError; sigemptyset(&bus.sa_mask); bus.sa_flags=SA_SIGINFO;
    sigaction(SIGBUS,&bus,nullptr);

    State st;
    for (int i=1;i<argc;i++) {
        if      (!strcmp(argv[i],"-f")&&i+1<argc) st.format=argv[++i];
        else if (!strcmp(argv[i],"-d")) st.show_dims  =true;
        else if (!strcmp(argv[i],"-D")) st.crosshair  =true;
        else if (!strcmp(argv[i],"-p")) st.point_mode =true;
        else if (!strcmp(argv[i],"-o")) st.output_mode=true;
        else if (!strcmp(argv[i],"-r")) st.force_stdin=true;
        else if (!strcmp(argv[i],"-v")) { st.verbose =false; dlog::level = 1; }

				else if (!strcmp(argv[i],"--bg") && i+1<argc) {
            auto c = parseHexColor(argv[++i]); if (!c.has_value()) return 1; BgColor = c.value();
        }
        else if (!strcmp(argv[i],"--border") && i+1<argc) {
            auto c = parseHexColor(argv[++i]); if (!c.has_value()) return 1; BorderColor = c.value();
        }
        else if (!strcmp(argv[i],"--border-width") && i+1<argc) { char* e=nullptr; double v=strtod(argv[++i],&e); if(e!=argv[i]&&v>0) BorderWidth=v; }
        else if (!strcmp(argv[i],"-h")||!strcmp(argv[i],"--help")) {
            fprintf(stderr,


                "Usage: cullpp " CULLPP_VERSION " [options...]\n"
                "\n"
                "  -f FORMAT  output format  %%x %%y %%w %%h %%X %%Y %%o %%l\n"
                "  -d         dimension overlay\n"
                "  -D         crosshair\n"
                "  -p         point mode\n"
                "  -o         outputs as snap rects\n"
                "  -r         read stdin even if tty\n"
                "  -v         log events to stderr\n"
								"  --bg RRGGBBAA      background color\n"
                "  --border RRGGBBAA  border color\n"
                "  --border-width N   border width in pixels\n"
								"\n"
								"env:\n"
								"    CULLPP_RESELECT_MOD  reselect modifier def: " XKB_MOD_NAME_SHIFT "\n"
								"\n"

								);


            return 0;
        }
        else { fprintf(stderr,"%s unknown option '%s'\n", errPrefix, argv[i]); return 1; }
    }




    st.display = wl_display_connect(nullptr);
    if (!st.display) { fprintf(stderr,"%s cannot connect to Wayland\n", errPrefix); return 1; }

    st.registry = wl_display_get_registry(st.display);
    wl_registry_add_listener(st.registry,&reg_listener,&st);
    wl_display_roundtrip(st.display);

    if (!st.shell||!st.compositor||!st.shm||st.outputs.empty()) {
        fprintf(stderr,"%s missing required Wayland globals\n", errPrefix); return 1;
    }

    st.ui_font = cairo_toy_font_face_create("sans-serif",
        CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    st.mono_font = cairo_toy_font_face_create("monospace",
        CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);

    if (st.xdg_mgr)
        for (auto* out:st.outputs) {
            out->xdg_out=zxdg_output_manager_v1_get_xdg_output(st.xdg_mgr,out->wl_out);
            zxdg_output_v1_add_listener(out->xdg_out,&xo_listener,out);
        }

    wl_display_roundtrip(st.display);

    parseStdin(&st);
    if (st.output_mode)
        for (const auto* o:st.outputs)
            if(o->lw>0)
                st.snaps.push_back({{static_cast<double>(o->lx),static_cast<double>(o->ly),
                                     static_cast<double>(o->lw),static_cast<double>(o->lh)},o->name});

    for (const auto* out:st.outputs)
        LOG_V("output", outName(out), "%p  wl_output@%u  %d,%d %dx%d @%d",
              static_cast<const void*>(out), wl_proxy_get_id(reinterpret_cast<wl_proxy*>(out->wl_out)),
              out->lx, out->ly, out->lw, out->lh, out->scale);

    for (auto* out:st.outputs) createOutputSurface(out);
    wl_display_roundtrip(st.display);
    for (auto* out:st.outputs) {
        out->needs_draw = true;
        if (out->configured) renderOutput(out);
    }

    int fd = wl_display_get_fd(st.display);
    while (st.running && !g_quit) {
        if (wl_display_dispatch_pending(st.display) < 0) break;
        if (!st.running || g_quit) break;



        if (!renderPendingOutputs(&st)) break;
        if (!st.running || g_quit) break;

        while (wl_display_prepare_read(st.display) != 0) {
            if (wl_display_dispatch_pending(st.display) < 0) goto done;
        }
        if (wl_display_flush(st.display) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(st.display);
            break;
        }

        struct pollfd pfd = { fd, POLLIN, 0 };
        int r = poll(&pfd, 1, -1);
        if (r < 0) {
            wl_display_cancel_read(st.display);
            if (errno==EINTR) continue;
            break;
        }
        if (pfd.revents & (POLLERR | POLLHUP)) {
            wl_display_cancel_read(st.display);
            break;
        }
        if (pfd.revents & POLLIN) {
            if (wl_display_read_events(st.display) < 0) break;


            if (wl_display_dispatch_pending(st.display) < 0) break;
            if (!st.running || g_quit) break;
            if (!renderPendingOutputs(&st)) break;
        } else {
            wl_display_cancel_read(st.display);
        }
    }
done:

    int rc = 1;
    std::string result_text;
    if (st.confirmed) {
        result_text = formatResult(&st);
        logResult(&st, result_text);
        rc=0;
    }

    if (st.confirmed || g_quit) unmapOutputs(&st);
    for (auto* out:st.outputs) {
        destroyOutput(out);
    }
    st.outputs.clear();
    for (auto* s:g_seats) {
        destroySeat(s);
    }
    g_seats.clear();

    if (st.confirmed || g_quit) wl_display_roundtrip(st.display);

    if (st.xkb_st)  xkb_state_unref(st.xkb_st);
    if (st.xkb_map) xkb_keymap_unref(st.xkb_map);
    if (st.xkb_ctx) xkb_context_unref(st.xkb_ctx);
    if (st.cursor_mgr) wp_cursor_shape_manager_v1_destroy(st.cursor_mgr);
    if (st.xdg_mgr)    zxdg_output_manager_v1_destroy(st.xdg_mgr);
    if (st.shell)      zwlr_layer_shell_v1_destroy(st.shell);
    if (st.shm)        wl_shm_destroy(st.shm);
    if (st.compositor) wl_compositor_destroy(st.compositor);
    if (st.registry)   wl_registry_destroy(st.registry);
    if (st.ui_font)    cairo_font_face_destroy(st.ui_font);
    if (st.mono_font)  cairo_font_face_destroy(st.mono_font);
    wl_display_disconnect(st.display);
    if (!result_text.empty()) std::cout << result_text;
    LOG_V("exit", "", "%d", rc);
    return rc;
}

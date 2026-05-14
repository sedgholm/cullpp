
#include <cstdint>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <cairo/cairo.h>
#include <xkbcommon/xkbcommon.h>
#include "shm_pool.hpp"
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/memfd.h>
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


static uint32_t BgColor = 0x4A4A4A57; //0x25214257
static uint32_t BorderColor = 0x4D73FFFF;
static double BorderWidth = 1.0;

//override at runtime via CULLPP_RESELECT_MOD env var.
static constexpr const char* DefaultReselectMod = XKB_MOD_NAME_SHIFT; // ALT, LOGO, CRTL
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
static void on_signal(int) { g_quit = 1; }

struct State;

struct Box { double x=0,y=0,w=0,h=0; bool empty()const{return w<=0||h<=0;} };
static bool inbox(const Box&b,double px,double py){
    return px>=b.x&&px<b.x+b.w&&py>=b.y&&py<b.y+b.h;
}

struct SnapRect { Box rect; std::string label; bool hovered=false; };

struct Output {
    State*                  st         = nullptr;
    wl_output*              wl_out     = nullptr;
    zxdg_output_v1*         xdg_out    = nullptr;
    zwlr_layer_surface_v1*  layer_surf = nullptr;
    wl_surface*             surface    = nullptr;
    wl_callback*            frame_cb   = nullptr;

    int lx=0,ly=0,lw=0,lh=0;
    int px=0,py=0,pw=0,ph=0;
    int scale=1;
    std::string name;

    Pool pool;
    bool configured= false;
    bool needs_draw= false;
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

		wl_keyboard*  kbd      = nullptr;
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

static void render(Output* out);
static std::string format_result(const State* st);
static bool reselect_held(State* st);

static inline void set_source_u32(cairo_t *cr, uint32_t c) {
    cairo_set_source_rgba(cr,
        ((c >> 24) & 0xFF) / 255.0,
        ((c >> 16) & 0xFF) / 255.0,
        ((c >>  8) & 0xFF) / 255.0,
        ((c >>  0) & 0xFF) / 255.0);
}


static uint32_t parse_hex_color(const char* s) {
    if (!s) return 0;
    size_t len = strlen(s);
    if (len != 8) { fprintf(stderr, "cullpp: color must be 8 hex digits RRGGBBAA\n"); return 0; }
    char* end = nullptr;
    unsigned long v = strtoul(s, &end, 16);
    if (end != s + 8) { fprintf(stderr, "cullpp: invalid hex color '%s'\n", s); return 0; }

		// this kinda insigtful BUG dbl shdrs when ./cullpp --border 22222299
    // return (uint32_t)(((v & 0xFF) << 24) | (((v >> 24) & 0xFF) << 16) |
    //                   (((v >> 16) & 0xFF) << 8) | ((v >> 8) & 0xFF));
		return(uint32_t)v;

}


static void frame_done(void* data, wl_callback* cb, uint32_t) {
    Output* out = (Output*)data;
    wl_callback_destroy(cb);
    out->frame_cb = nullptr;
    if (out->needs_draw) render(out);
}
static const wl_callback_listener frame_listener = { .done = frame_done };

static bool box_intersects_output(const Box& box, const Output* out, double pad = 0.0) {
    if (box.empty()) return false;
    const double left   = box.x - pad;
    const double top    = box.y - pad;
    const double right  = box.x + box.w + pad;
    const double bottom = box.y + box.h + pad;
    return right > out->lx && left < out->lx + out->lw &&
           bottom > out->ly && top < out->ly + out->lh;
}

static bool point_in_output(const Output* out, double x, double y) {
    return x >= out->lx && x < out->lx + out->lw &&
           y >= out->ly && y < out->ly + out->lh;
}

static void request_draw_output(Output* out) {
    if (out) out->needs_draw = true;
}

static void request_draw_box(State* st, const Box& box, double pad = 0.0) {
    (void)pad;
    for (auto* out : st->outputs)
        if (box_intersects_output(box, out, 0.0)) out->needs_draw = true;
}

static void unmap_outputs(State* st) {
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

static void render(Output* out) {
    State* st = out->st;
    if (!out->configured) return;

    out->needs_draw = false;
    if (st->verbose) fprintf(stderr, "RENDER: output %p geo=(%d,%d %dx%d) sel=%d pressing=%d\n",
        (void*)out, out->lx, out->ly, out->lw, out->lh, st->has_sel, st->pressing);

    PoolBuf* pb = pool_next_buf(out->pool);
    if (!pb) { out->needs_draw = true; return; }  // both buffers in-flight

    cairo_t* cr = cairo_create(pb->cs);

    // apply HiDPI scale so all drawing coords are logical pixels.
    cairo_identity_matrix(cr);
    cairo_scale(cr, (double)out->scale, (double)out->scale);


    cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    set_source_u32(cr, BgColor);
    cairo_paint(cr);

    for (const auto& sn : st->snaps) {
        const double rx = sn.rect.x - out->lx;
        const double ry = sn.rect.y - out->ly;
        const double rw = sn.rect.w;
        const double rh = sn.rect.h;
        if (sn.hovered) {
            cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
            cairo_set_source_rgba(cr, 0.27, 1.0, 0.27, 0.22);
            cairo_rectangle(cr, rx, ry, rw, rh);
            cairo_fill(cr);
        }
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        cairo_set_source_rgba(cr, 0.27, 1.0, 0.27, 0.9);
        cairo_set_line_width(cr, 1.5);
        cairo_rectangle(cr, rx+0.75, ry+0.75, rw-1.5, rh-1.5);
        cairo_stroke(cr);
    }

    if (st->has_sel) {
        const double rx = st->sel.x - out->lx;
        const double ry = st->sel.y - out->ly;
        const double rw = st->sel.w;
        const double rh = st->sel.h;

        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.000);
        cairo_rectangle(cr, rx, ry, rw, rh);
        cairo_fill(cr);

        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
				set_source_u32(cr, BorderColor);

        cairo_set_line_width(cr, BorderWidth);
		// cairo_rectangle(cr, rx+0.625, ry+0.625, rw-1.25, rh-1.25);
		cairo_rectangle(cr, rx, ry, rw, rh);
    cairo_stroke(cr);


				/* white corners */
        // cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.82);
        // const double hs = 3.0;
        // const double gcx[4] = { rx, rx+rw-hs, rx, rx+rw-hs };
        // const double gcy[4] = { ry, ry, ry+rh-hs, ry+rh-hs };
        // for (int i = 0; i < 4; i++) {
        //     cairo_rectangle(cr, gcx[i], gcy[i], hs, hs);
        //     cairo_fill(cr);
        // }

        if (st->show_dims) {
            char s[64];
            snprintf(s, sizeof(s), "%d × %d", (int)st->sel.w, (int)st->sel.h);
            cairo_set_font_face(cr, st->ui_font);
            cairo_set_font_size(cr, 13.0);
            cairo_text_extents_t te;
            cairo_text_extents(cr, s, &te);
            double tx = rx + rw + 10;
            double ty = ry + rh + 10 + te.height;
            if (tx + te.width + 8 > out->lw) tx = rx - te.width - 14;
            if (ty + 4 > out->lh)             ty = ry - 8;
            if (tx < 0)                       tx = rx + 4;
            if (ty - te.height < 0)           ty = ry + te.height + 4;
            cairo_set_source_rgba(cr, 0, 0, 0, 0.72);
            const double lp=4;
            cairo_rectangle(cr, tx-lp, ty-te.height-lp, te.width+lp*2, te.height+lp*2);
            cairo_fill(cr);
            cairo_set_source_rgba(cr, 1, 1, 1, 1);
            cairo_move_to(cr, tx, ty);
            cairo_show_text(cr, s);
        }
    }

    if (st->crosshair) {
        double cx = st->cur_x - out->lx;
        double cy = st->cur_y - out->ly;
        if (cx>=0&&cx<out->lw&&cy>=0&&cy<out->lh) {
            cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
            cairo_set_source_rgba(cr, 1, 1, 1, 0.5);
            cairo_set_line_width(cr, 1.0);
            cairo_move_to(cr, 0.5,   cy+0.5); cairo_line_to(cr, out->lw, cy+0.5);
            cairo_move_to(cr, cx+0.5, 0.5);   cairo_line_to(cr, cx+0.5, out->lh);
            cairo_stroke(cr);

            char s[128];
            if (st->pressing && st->has_sel) {
                snprintf(s, sizeof(s), "%dx%d",
                    (int)st->sel.w, (int)st->sel.h);
            } else if (st->pressing) {
                int dx = (int)std::abs(st->cur_x - st->anchor_x);
                int dy = (int)std::abs(st->cur_y - st->anchor_y);
                snprintf(s, sizeof(s), "%d, %d  +%dx%d",
                    (int)st->cur_x, (int)st->cur_y, dx, dy);
            } else {
                snprintf(s, sizeof(s), "%d, %d", (int)st->cur_x, (int)st->cur_y);
            }
            cairo_set_font_face(cr, st->mono_font);
            cairo_set_font_size(cr, 11.0);
            cairo_text_extents_t te;
            cairo_text_extents(cr, s, &te);
            double tx = cx + 12;
            double ty = cy - 8;
            if (tx + te.width + 6 > out->lw) tx = cx - te.width - 12;
            if (ty - te.height < 0) ty = cy + te.height + 14;
            if (tx < 0) tx = cx + 12;
            cairo_set_source_rgba(cr, 0.1, 0.1, 0.1, 0.85);
            double padw = te.width + 6;
            double padh = te.height + 4;
            if (st->pressing) padw += 30;
            cairo_rectangle(cr, tx-3, ty-te.height-2, padw, padh);
            cairo_fill(cr);
            cairo_set_source_rgba(cr, 0.0, 1.0, 0.0, 0.95);
            cairo_move_to(cr, tx, ty);
            cairo_show_text(cr, s);
        }
    }

    cairo_destroy(cr);
    cairo_surface_flush(pb->cs);

    // Only register a new frame callback if one isn't already in-flight.
    if (!out->frame_cb) {
        out->frame_cb = wl_surface_frame(out->surface);
        wl_callback_add_listener(out->frame_cb, &frame_listener, out);
    }

    wl_surface_attach(out->surface, pb->wl, 0, 0);

    wl_surface_damage(out->surface, 0, 0, out->lw, out->lh);
    wl_surface_set_buffer_scale(out->surface, out->scale);
    wl_surface_commit(out->surface);
    // pool_next_buf already marked buffer busy atomically
    out->pool.idx ^= 1;
}

static void ls_configure(void* data, zwlr_layer_surface_v1* ls,
                          uint32_t serial, uint32_t w, uint32_t h) {
    Output* out = (Output*)data;
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    if (!pool_init(out->pool, out->st->shm, (int)w, (int)h, out->scale)) {
        fprintf(stderr, "cullpp: pool_init failed (%ux%u scale=%d)\n", w, h, out->scale);
        out->st->running = false;
        return;
    }
    out->configured = true;
    out->needs_draw  = true;
}
static void ls_closed(void* data, zwlr_layer_surface_v1*) {
    ((Output*)data)->st->running = false;
}
static const zwlr_layer_surface_v1_listener ls_listener = {
    .configure=ls_configure, .closed=ls_closed };

static void output_create_surface(Output* out) {
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
    Output*  ptr_out = nullptr;
    uint32_t ptr_ser = 0;
    wp_cursor_shape_device_v1* shape_dev = nullptr;
    wl_cursor_theme* cur_theme = nullptr;
    wl_cursor*       cur       = nullptr;
    wl_surface*      cur_surf  = nullptr;
};
static std::vector<Seat*> g_seats;

static void set_cursor(Seat* seat, uint32_t serial) {
    if (seat->shape_dev)
        wp_cursor_shape_device_v1_set_shape(seat->shape_dev, serial,
            WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR);
    else if (seat->cur && seat->cur_surf) {
        wl_cursor_image* img = seat->cur->images[0];
        wl_buffer* cbuf = wl_cursor_image_get_buffer(img);
        if (cbuf) {
					  int32_t scale = seat->ptr_out ? seat->ptr_out->scale : 1;
            wl_surface_set_buffer_scale(seat->cur_surf, scale);
            wl_surface_attach(seat->cur_surf, cbuf, 0, 0);
            wl_surface_damage_buffer(seat->cur_surf,0,0,img->width,img->height);
            wl_surface_commit(seat->cur_surf);

						wl_pointer_set_cursor(seat->ptr, serial, seat->cur_surf,
                img->hotspot_x / scale, img->hotspot_y / scale);
        }
    }
}

static void ptr_enter(void* d, wl_pointer*, uint32_t ser,
                       wl_surface* surf, wl_fixed_t sx, wl_fixed_t sy) {
    Seat* s=(Seat*)d;
    State* st=s->st;
    s->ptr_ser=ser; s->ptr_out=nullptr;
    for (auto* out:st->outputs) {
        if (out->surface==surf) {
            s->ptr_out=out;




            const double gx = out->lx + wl_fixed_to_double(sx);
            const double gy = out->ly + wl_fixed_to_double(sy);
            st->cur_x = gx; st->cur_y = gy;
            for (auto& sn:st->snaps) {
                const bool h = inbox(sn.rect,gx,gy);
                if (h != sn.hovered)
                    request_draw_box(st, sn.rect, 4.0);
                sn.hovered = h;
            }
            break;
        }
    }
    set_cursor(s, ser);
}
static void ptr_leave(void* d, wl_pointer*, uint32_t, wl_surface*) {
    Seat* seat=(Seat*)d;
    State* st=seat->st;
    seat->ptr_out = nullptr;

    for (auto& sn:st->snaps) {
        if (sn.hovered) {
            sn.hovered=false;
            request_draw_box(st, sn.rect, 4.0);
        }
    }
}
static void ptr_motion(void* d, wl_pointer*, uint32_t,
                        wl_fixed_t sx, wl_fixed_t sy) {
    Seat*  seat=(Seat*)d;
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
        st->has_sel = st->sel.w>3 && st->sel.h>3;
        selection_changed =
            st->sel.x != prev_sel.x || st->sel.y != prev_sel.y ||
            st->sel.w != prev_sel.w || st->sel.h != prev_sel.h ||
            st->has_sel != prev_has_sel;
    }
    bool hover_changed = false;
    for (auto& sn:st->snaps) {
        const bool hovered = inbox(sn.rect,gx,gy);
        hover_changed |= sn.hovered != hovered;
        sn.hovered = hovered;
    }
    const bool cursor_changed = gx != prev_x || gy != prev_y;
    if (selection_changed) {
        if (prev_has_sel) request_draw_box(st, prev_sel, 4.0);
        if (st->has_sel) request_draw_box(st, st->sel, 4.0);
        request_draw_output(seat->ptr_out);
        if (st->crosshair) {
            for (auto* out : st->outputs)
                if (point_in_output(out, prev_x, prev_y)) request_draw_output(out);
        }
        if (st->verbose) fprintf(stderr, "MOTION: sel=(%d,%d %dx%d)\n",
            (int)st->sel.x, (int)st->sel.y, (int)st->sel.w, (int)st->sel.h);
    } else if (hover_changed || (st->crosshair && cursor_changed)) {
        request_draw_output(seat->ptr_out);
        if (st->crosshair) {
            for (auto* out : st->outputs)
                if (point_in_output(out, prev_x, prev_y)) request_draw_output(out);
        }
        if (st->verbose) fprintf(stderr, "MOTION: cursor=(%d,%d) hover_changed=%d\n",
            (int)st->cur_x, (int)st->cur_y, hover_changed);
    }
}
static void ptr_button(void* d, wl_pointer*, uint32_t, uint32_t,
                        uint32_t btn, uint32_t state_val) {
    Seat*  seat=(Seat*)d;
    State* st=seat->st;
    if (btn==BTN_RIGHT && state_val==WL_POINTER_BUTTON_STATE_PRESSED) {
        st->running=false; return;
    }
    if (btn!=BTN_LEFT) return;
    if (state_val==WL_POINTER_BUTTON_STATE_PRESSED) {
        st->anchor_x=st->cur_x; st->anchor_y=st->cur_y;
        st->pressing=true; st->has_sel=false;
        request_draw_output(seat->ptr_out);
        if (st->verbose) fprintf(stderr, "BUTTON: LMB DOWN at (%d,%d)\n",
            (int)st->cur_x, (int)st->cur_y);
    } else {
        const Box cleared_sel = st->sel;
        st->pressing=false;
        if (st->point_mode) {
            st->result={st->cur_x,st->cur_y,1,1};
        // st->confirmed=true; st->running=false; return;
        // }
        // if (st->has_sel && st->sel.w>1 && st->sel.h>1) {
        //     st->result=st->sel; st->confirmed=true; st->running=false;
        //     return;
        // } else {
        //     for (const auto& sn:st->snaps) if(sn.hovered){
        //         st->result=sn.rect; st->confirmed=true; st->running=false; return;
        //     }



				    st->confirmed=true;
            if (!reselect_held(st)) { st->running=false; return; }
            fputs(format_result(st).c_str(), stdout); fflush(stdout);
            st->confirmed=false; st->has_sel=false; st->sel={0,0,0,0};
            for (auto* o : st->outputs) request_draw_output(o);
            return;
         }
         if (st->has_sel && st->sel.w>1 && st->sel.h>1) {
            st->result=st->sel; st->confirmed=true;
            if (!reselect_held(st)) { st->running=false; return; }
            fputs(format_result(st).c_str(), stdout); fflush(stdout);
            st->confirmed=false; st->has_sel=false; st->sel={0,0,0,0};
            for (auto* o : st->outputs) request_draw_output(o);
             return;
         } else {
            bool has_hovered = false;
             for (const auto& sn:st->snaps) if(sn.hovered){
                st->result=sn.rect; st->confirmed=true;
                if (!reselect_held(st)) { st->running=false; return; }
                fputs(format_result(st).c_str(), stdout); fflush(stdout);
                st->confirmed=false; st->has_sel=false; st->sel={0,0,0,0};
                for (auto* o : st->outputs) request_draw_output(o);
                has_hovered = true; break;
             }
            if (has_hovered) return;

            st->has_sel=false;
            st->result={st->cur_x,st->cur_y,1,1};
            st->confirmed=true; st->running=false;
            request_draw_box(st, cleared_sel, 4.0);
            request_draw_output(seat->ptr_out);
            return;
        }
    }
}

static void ptr_axis        (void*,wl_pointer*,uint32_t,uint32_t,wl_fixed_t){}
static void ptr_frame       (void*,wl_pointer*){}
static void ptr_axis_source (void*,wl_pointer*,uint32_t){}
static void ptr_axis_stop   (void*,wl_pointer*,uint32_t,uint32_t){}
static void ptr_axis_disc   (void*,wl_pointer*,uint32_t,int32_t){}
static void ptr_axis_v120   (void*,wl_pointer*,uint32_t,int32_t){}
static void ptr_axis_rel    (void*,wl_pointer*,uint32_t,uint32_t){}
static const wl_pointer_listener ptr_listener = {
    .enter=ptr_enter,.leave=ptr_leave,.motion=ptr_motion,.button=ptr_button,
    .axis=ptr_axis,.frame=ptr_frame,.axis_source=ptr_axis_source,
    .axis_stop=ptr_axis_stop,.axis_discrete=ptr_axis_disc,
    .axis_value120=ptr_axis_v120,.axis_relative_direction=ptr_axis_rel,
};


static void kbd_keymap(void* data, wl_keyboard*, uint32_t format, int32_t fd, uint32_t size) {
    State* st = (State*)data;
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
static bool reselect_held(State* st) {
    return st->xkb_st &&
        xkb_state_mod_name_is_active(st->xkb_st, ReselectMod,
            XKB_STATE_MODS_EFFECTIVE) > 0;
}

static void kbd_key(void* data, wl_keyboard*, uint32_t, uint32_t, uint32_t keycode, uint32_t key_state) {
    State* st = (State*)data;
    if (key_state != WL_KEYBOARD_KEY_STATE_PRESSED) return;
    if (!st->xkb_st) return;
    xkb_keycode_t xkb_kc = keycode + 8;
    const xkb_keysym_t* syms;
    int nsyms = xkb_state_key_get_syms(st->xkb_st, xkb_kc, &syms);
    for (int i = 0; i < nsyms; ++i) {
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
static void kbd_modifiers(void* data, wl_keyboard*, uint32_t, uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group) {
    State* st = (State*)data;
    if (st->xkb_st)
        xkb_state_update_mask(st->xkb_st, mods_depressed, mods_latched, mods_locked, 0, 0, group);
}
static void kbd_enter     (void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*) {}
static void kbd_leave     (void*, wl_keyboard*, uint32_t, wl_surface*) {}
static void kbd_repeat_info(void*, wl_keyboard*, int32_t, int32_t) {}
static const wl_keyboard_listener kbd_listener = {
    .keymap = kbd_keymap,
    .enter = kbd_enter,
    .leave = kbd_leave,
    .key = kbd_key,
    .modifiers = kbd_modifiers,
    .repeat_info = kbd_repeat_info,
};


static void seat_caps(void* d, wl_seat* ws, uint32_t caps) {
    Seat* seat=(Seat*)d; State* st=seat->st;
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
                if (end != ss && v > 0 && v <= 256) sz = (int)v;
            }
            seat->cur_theme = wl_cursor_theme_load(th,sz,st->shm);
            if (seat->cur_theme) {
                seat->cur = wl_cursor_theme_get_cursor(seat->cur_theme,"crosshair");
                if(!seat->cur) seat->cur=wl_cursor_theme_get_cursor(seat->cur_theme,"cross");
            }
            if (seat->cur) seat->cur_surf=wl_compositor_create_surface(st->compositor);
        }
    }

		    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !st->kbd) {
        if (!st->xkb_ctx)
            st->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        st->kbd = wl_seat_get_keyboard(ws);
        wl_keyboard_add_listener(st->kbd, &kbd_listener, st);
    }
}
static void seat_name(void*,wl_seat*,const char*){}
static const wl_seat_listener seat_listener={.capabilities=seat_caps,.name=seat_name};

static void xo_pos (void*d,zxdg_output_v1*,int32_t x,int32_t y){Output*o=(Output*)d;o->lx=x;o->ly=y;}
static void xo_size(void*d,zxdg_output_v1*,int32_t w,int32_t h){Output*o=(Output*)d;o->lw=w;o->lh=h;}
static void xo_done(void*,zxdg_output_v1*){}
static void xo_name(void*d,zxdg_output_v1*,const char*n){if(n)((Output*)d)->name=n;}
static void xo_desc(void*,zxdg_output_v1*,const char*){}
static const zxdg_output_v1_listener xo_listener={
    .logical_position=xo_pos,.logical_size=xo_size,
    .done=xo_done,.name=xo_name,.description=xo_desc};

static void wo_geom(void*d,wl_output*,int32_t x,int32_t y,int32_t,int32_t,int32_t,const char*,const char*,int32_t){Output*o=(Output*)d;o->px=x;o->py=y;}
static void wo_mode(void*d,wl_output*,uint32_t f,int32_t w,int32_t h,int32_t){if(f&WL_OUTPUT_MODE_CURRENT){Output*o=(Output*)d;o->pw=w;o->ph=h;}}
static void wo_done(void*d,wl_output*){Output*o=(Output*)d;if(o->lw==0&&o->pw>0){o->lx=o->px/o->scale;o->ly=o->py/o->scale;o->lw=o->pw/o->scale;o->lh=o->ph/o->scale;}}
static void wo_scale(void*d,wl_output*,int32_t f){((Output*)d)->scale=f;}
static void wo_name(void*d,wl_output*,const char*n){Output*o=(Output*)d;if(o->name.empty()&&n)o->name=n;}
static void wo_desc(void*,wl_output*,const char*){}
static const wl_output_listener wo_listener={.geometry=wo_geom,.mode=wo_mode,.done=wo_done,.scale=wo_scale,.name=wo_name,.description=wo_desc};

static void reg_global(void* data, wl_registry* reg,
                        uint32_t name, const char* iface, uint32_t version) {
    State* st=(State*)data;
    if      (!strcmp(iface,wl_compositor_interface.name))
        st->compositor=(wl_compositor*)wl_registry_bind(reg,name,&wl_compositor_interface,
            std::min(version, 4u));
    else if (!strcmp(iface,wl_shm_interface.name))
        st->shm=(wl_shm*)wl_registry_bind(reg,name,&wl_shm_interface,1);
    else if (!strcmp(iface,zwlr_layer_shell_v1_interface.name))
        st->shell=(zwlr_layer_shell_v1*)wl_registry_bind(reg,name,&zwlr_layer_shell_v1_interface,
            std::min(version, (uint32_t)zwlr_layer_shell_v1_interface.version));
    else if (!strcmp(iface,zxdg_output_manager_v1_interface.name))
        st->xdg_mgr=(zxdg_output_manager_v1*)wl_registry_bind(reg,name,&zxdg_output_manager_v1_interface,
            std::min(version, (uint32_t)zxdg_output_manager_v1_interface.version));
    else if (!strcmp(iface,wp_cursor_shape_manager_v1_interface.name))
        st->cursor_mgr=(wp_cursor_shape_manager_v1*)wl_registry_bind(reg,name,&wp_cursor_shape_manager_v1_interface,
            std::min(version, (uint32_t)wp_cursor_shape_manager_v1_interface.version));
    else if (!strcmp(iface,wl_output_interface.name)) {
        Output* out=new Output(); out->st=st;
        out->wl_out=(wl_output*)wl_registry_bind(reg,name,&wl_output_interface,std::min(version, 4u));
        wl_output_add_listener(out->wl_out,&wo_listener,out);
        st->outputs.push_back(out);
    }
    else if (!strcmp(iface,wl_seat_interface.name)) {
        Seat* seat=new Seat(); seat->st=st;
        seat->wl_s=(wl_seat*)wl_registry_bind(reg,name,&wl_seat_interface,std::min(version, 7u));
        wl_seat_add_listener(seat->wl_s,&seat_listener,seat);
        g_seats.push_back(seat);
    }
}
static void reg_remove(void*,wl_registry*,uint32_t){}
static const wl_registry_listener reg_listener={.global=reg_global,.global_remove=reg_remove};

static void parse_stdin(State* st) {
    if (!st->force_stdin && isatty(STDIN_FILENO)) return;
    char line[1024];
    while (fgets(line,sizeof(line),stdin)) {
        double x,y,w,h;
        if (sscanf(line,"%lf,%lf %lfx%lf",&x,&y,&w,&h)!=4) continue;
        SnapRect sn; sn.rect={x,y,w,h};
        char* p=strchr(line,' '); if(p) p=strchr(p+1,' ');
        if (p) {
            p++;
            size_t len=strlen(p);
            while(len>0&&(p[len-1]=='\n'||p[len-1]=='\r'))--len;
            if(len>0) sn.label=std::string(p,len);
        }
        st->snaps.push_back(sn);
    }
}

static std::string format_result(const State* st) {
    const Box& r=st->result;
    const Output* best_out=nullptr; double best=-1;
    for (const auto* o:st->outputs) {
        double ix=std::max(r.x,(double)o->lx), iy=std::max(r.y,(double)o->ly);
        double ix2=std::min(r.x+r.w,(double)(o->lx+o->lw));
        double iy2=std::min(r.y+r.h,(double)(o->ly+o->lh));
        double a=(ix2>ix&&iy2>iy)?(ix2-ix)*(iy2-iy):0;
        if(a>best){best=a;best_out=o;}
    }
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
            case 'x':out += std::to_string((int)r.x);break;
            case 'y':out += std::to_string((int)r.y);break;
            case 'w':out += std::to_string((int)r.w);break;
            case 'h':out += std::to_string((int)r.h);break;
            case 'X':out += std::to_string((int)(r.x+r.w));break;
            case 'Y':out += std::to_string((int)(r.y+r.h));break;
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

	  //env
    const char* env_mod = getenv("CULLPP_RESELECT_MOD");
    ReselectMod = env_mod ? env_mod : DefaultReselectMod;

    struct sigaction sa{};
    sa.sa_handler=on_signal; sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,&sa,nullptr); sigaction(SIGTERM,&sa,nullptr);

    State st;
    st.ui_font = cairo_toy_font_face_create("sans-serif",
        CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    st.mono_font = cairo_toy_font_face_create("monospace",
        CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    for (int i=1;i<argc;i++) {
        if      (!strcmp(argv[i],"-f")&&i+1<argc) st.format=argv[++i];
        else if (!strcmp(argv[i],"-d")) st.show_dims  =true;
        else if (!strcmp(argv[i],"-D")) st.crosshair  =true;
        else if (!strcmp(argv[i],"-p")) st.point_mode =true;
        else if (!strcmp(argv[i],"-o")) st.output_mode=true;
        else if (!strcmp(argv[i],"-r")) st.force_stdin=true;
        else if (!strcmp(argv[i],"-v")) st.verbose=true;

				else if (!strcmp(argv[i],"--bg") && i+1<argc) BgColor = parse_hex_color(argv[++i]);
        else if (!strcmp(argv[i],"--border") && i+1<argc) BorderColor = parse_hex_color(argv[++i]);
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
                "  -v         verbose debug output\n"
								"  --bg RRGGBBAA      background color\n"
                "  --border RRGGBBAA  border color\n"
                "  --border-width N   border width in pixels\n"
								"env:\n"
								"    CULLPP_RESELECT_MOD  reselect modifier def: " XKB_MOD_NAME_SHIFT "\n"

								);


            return 0;
        }
        else { fprintf(stderr,"cullpp: unknown option '%s'\n",argv[i]); return 1; }
    }

    st.display = wl_display_connect(nullptr);
    if (!st.display) { fprintf(stderr,"cullpp: cannot connect to Wayland\n"); return 1; }

    st.registry = wl_display_get_registry(st.display);
    wl_registry_add_listener(st.registry,&reg_listener,&st);
    wl_display_roundtrip(st.display);

    if (!st.shell||!st.compositor||!st.shm||st.outputs.empty()) {
        fprintf(stderr,"cullpp: missing required Wayland globals\n"); return 1;
    }

    if (st.xdg_mgr)
        for (auto* out:st.outputs) {
            out->xdg_out=zxdg_output_manager_v1_get_xdg_output(st.xdg_mgr,out->wl_out);
            zxdg_output_v1_add_listener(out->xdg_out,&xo_listener,out);
        }

    wl_display_roundtrip(st.display);
    wl_display_roundtrip(st.display);

    parse_stdin(&st);
    if (st.output_mode)
        for (const auto* o:st.outputs)
            if(o->lw>0)
                st.snaps.push_back({{(double)o->lx,(double)o->ly,
                                     (double)o->lw,(double)o->lh},o->name});

    for (auto* out:st.outputs) output_create_surface(out);
    wl_display_roundtrip(st.display);
    for (auto* out:st.outputs) {
        out->needs_draw = true;
        if (out->configured) render(out);
    }

    int fd = wl_display_get_fd(st.display);
    while (st.running && !g_quit) {
        if (wl_display_dispatch_pending(st.display) < 0) break;
        if (!st.running || g_quit) break;



        for (auto* out:st.outputs)
            if (out->needs_draw && !out->frame_cb && out->configured)
                render(out);
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
            for (auto* out : st.outputs)
                if (out->needs_draw && !out->frame_cb && out->configured)
                    render(out);
            if (wl_display_flush(st.display) < 0 && errno != EAGAIN) break;
        } else {
            wl_display_cancel_read(st.display);
        }
    }
done:

    int rc = 1;
    std::string result_text;
    if (st.confirmed) {
        result_text = format_result(&st);
        rc=0;
    }

    if (st.confirmed || g_quit) unmap_outputs(&st);
    for (auto* out:st.outputs) {
        if (out->frame_cb) { wl_callback_destroy(out->frame_cb); out->frame_cb=nullptr; }
        pool_destroy(out->pool);
        if (out->layer_surf) zwlr_layer_surface_v1_destroy(out->layer_surf);
        if (out->surface)    wl_surface_destroy(out->surface);
        if (out->xdg_out)    zxdg_output_v1_destroy(out->xdg_out);
        if (out->wl_out)     wl_output_release(out->wl_out);
        delete out;
    }
    for (auto* s:g_seats) {
        if (s->shape_dev)  wp_cursor_shape_device_v1_destroy(s->shape_dev);
        if (s->cur_surf)   wl_surface_destroy(s->cur_surf);
        if (s->cur_theme)  wl_cursor_theme_destroy(s->cur_theme);
        if (s->ptr)        wl_pointer_release(s->ptr);
        if (s->wl_s)       wl_seat_release(s->wl_s);
        delete s;
    }

    if (st.confirmed || g_quit) wl_display_roundtrip(st.display);

		if (st.kbd)     wl_keyboard_destroy(st.kbd);
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
    if (!result_text.empty()) fputs(result_text.c_str(), stdout);
    return rc;
}

/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 George
 */
/* og_plat_x11.c - Linux backend: X11 + XRandR + XRender + XScreenSaver.
 *
 * Capture strategy: rather than pulling a whole 4K framebuffer across the
 * wire every sample, the root picture is scaled down to the 32x18 signature
 * grid server-side with XRender and only 576 pixels are read back. On a
 * typical GPU that is well under a millisecond.
 *
 * Wayland: a native Wayland session gives an X client neither screen capture
 * nor reliable always-on-top windows. og_plat_capture_available() reports 0
 * there and the core degrades to inhibitor-only detection. See README.
 */
#include "oledguard/og_platform.h"
#include "oledguard/og_log.h"
#include "og_linux_internal.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/scrnsaver.h>
#include <X11/extensions/shape.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */

typedef struct {
    Window   win;
    int      mapped;
    long     fade_start_ms;
    int      fade_ms;
    double   target_alpha;   /* what the fade climbs to, not always 1.0 */
} og_overlay_x11;

static struct {
    Display *dpy;
    int      screen;
    Window   root;

    int has_randr, randr_event_base, randr_error_base;
    int has_xss,   xss_event_base,   xss_error_base;
    int has_render;
    int has_shape, shape_event_base, shape_error_base;

    int displays_dirty;
    int is_wayland;

    /* fullscreen-window cache */
    long fs_cache_ms;
    og_rect fs_rect[16];
    int  fs_count;

    /* signature scratch */
    Pixmap  sig_pix;
    Picture sig_dst;
    Picture root_src;

    og_overlay_x11 overlays[OG_MAX_MONITORS];
    int overlay_count;

    Atom a_net_client_list, a_net_wm_state, a_net_wm_state_fullscreen;
    Atom a_net_wm_window_type, a_net_wm_window_type_dock;
    Atom a_net_wm_state_above, a_net_wm_opacity, a_edid;
} G;

/* X errors from racing with window destruction are routine here. */
static int og_x_error(Display *d, XErrorEvent *e)
{
    char buf[128];
    XGetErrorText(d, e->error_code, buf, sizeof buf);
    og_log(OG_LOG_DEBUG, "X error (ignored): %s (req %d)", buf, e->request_code);
    return 0;
}

/* ------------------------------------------------------------------ */

long og_plat_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

const char *og_plat_name(void)
{
    return G.is_wayland ? "Linux (XWayland, degraded)" : "Linux (X11)";
}

int og_plat_init(void)
{
    const char *wl = getenv("WAYLAND_DISPLAY");
    const char *st = getenv("XDG_SESSION_TYPE");

    memset(&G, 0, sizeof G);
    G.is_wayland = (wl && *wl) || (st && strcmp(st, "wayland") == 0);

    XSetErrorHandler(og_x_error);
    G.dpy = XOpenDisplay(NULL);
    if (!G.dpy) {
        og_log(OG_LOG_ERROR, "cannot open X display (is DISPLAY set?)");
        return 0;
    }
    G.screen = DefaultScreen(G.dpy);
    G.root   = RootWindow(G.dpy, G.screen);

    G.has_randr  = XRRQueryExtension(G.dpy, &G.randr_event_base, &G.randr_error_base);
    G.has_xss    = XScreenSaverQueryExtension(G.dpy, &G.xss_event_base, &G.xss_error_base);
    { int ma, mi; G.has_render = XRenderQueryExtension(G.dpy, &ma, &mi); }
    G.has_shape  = XShapeQueryExtension(G.dpy, &G.shape_event_base, &G.shape_error_base);

    if (G.has_randr)
        XRRSelectInput(G.dpy, G.root, RRScreenChangeNotifyMask | RROutputChangeNotifyMask);

    G.a_net_client_list = XInternAtom(G.dpy, "_NET_CLIENT_LIST", False);
    G.a_net_wm_state    = XInternAtom(G.dpy, "_NET_WM_STATE", False);
    G.a_net_wm_state_fullscreen = XInternAtom(G.dpy, "_NET_WM_STATE_FULLSCREEN", False);
    G.a_net_wm_window_type      = XInternAtom(G.dpy, "_NET_WM_WINDOW_TYPE", False);
    G.a_net_wm_window_type_dock = XInternAtom(G.dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    G.a_net_wm_state_above      = XInternAtom(G.dpy, "_NET_WM_STATE_ABOVE", False);
    G.a_net_wm_opacity  = XInternAtom(G.dpy, "_NET_WM_WINDOW_OPACITY", False);
    G.a_edid            = XInternAtom(G.dpy, "EDID", False);

    if (!G.has_randr) og_log(OG_LOG_WARN, "no XRandR: treating the X screen as one monitor");
    if (!G.has_xss)   og_log(OG_LOG_WARN, "no XScreenSaver extension: keyboard idle unavailable");
    if (!G.has_render)og_log(OG_LOG_WARN, "no XRender: content sampling disabled");
    if (G.is_wayland) og_log(OG_LOG_WARN,
        "Wayland session detected; running through XWayland. Screen sampling "
        "and always-on-top overlays are unreliable here.");

    og_linux_dbus_init();
    G.displays_dirty = 0;
    return 1;
}

void og_plat_shutdown(void)
{
    int i;
    if (!G.dpy) return;
    for (i = 0; i < OG_MAX_MONITORS; ++i) {
        if (G.overlays[i].win) {
            XDestroyWindow(G.dpy, G.overlays[i].win);
            G.overlays[i].win = 0;
        }
    }
    if (G.sig_dst)  { XRenderFreePicture(G.dpy, G.sig_dst);  G.sig_dst = 0; }
    if (G.root_src) { XRenderFreePicture(G.dpy, G.root_src); G.root_src = 0; }
    if (G.sig_pix)  { XFreePixmap(G.dpy, G.sig_pix);         G.sig_pix = 0; }
    og_linux_dbus_shutdown();
    XCloseDisplay(G.dpy);
    G.dpy = NULL;
}

/* ------------------------------------------------------------------ */
/* Monitor enumeration                                                  */
/* ------------------------------------------------------------------ */

/* EDID bytes 8..17 are manufacturer id, product code and serial number:
 * stable for the life of the panel and unchanged by a replug on a different
 * port. That is exactly the identity we want to key settings on. */
static int edid_id_for_output(RROutput out, char *dst, size_t dlen)
{
    Atom actual_type;
    int actual_format;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *prop = NULL;
    int ok = 0;

    if (XRRGetOutputProperty(G.dpy, out, G.a_edid, 0, 32, False, False,
                             AnyPropertyType, &actual_type, &actual_format,
                             &nitems, &bytes_after, &prop) == Success) {
        if (prop && nitems >= 18) {
            snprintf(dst, dlen,
                     "edid-%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
                     prop[8], prop[9], prop[10], prop[11], prop[12],
                     prop[13], prop[14], prop[15], prop[16], prop[17]);
            ok = 1;
        }
    }
    if (prop) XFree(prop);
    return ok;
}

int og_plat_enum_monitors(og_monitor *out, int max)
{
    int n = 0;

    if (!G.dpy || !out || max <= 0) return 0;

    if (G.has_randr) {
        int nmon = 0, i;
        XRRMonitorInfo *mons = XRRGetMonitors(G.dpy, G.root, True, &nmon);
        if (mons && nmon > 0) {
            for (i = 0; i < nmon && n < max; ++i) {
                og_monitor *m = &out[n];
                char *aname = XGetAtomName(G.dpy, mons[i].name);
                memset(m, 0, sizeof(*m));
                m->bounds.x = mons[i].x;
                m->bounds.y = mons[i].y;
                m->bounds.w = mons[i].width;
                m->bounds.h = mons[i].height;
                m->primary  = mons[i].primary ? 1 : 0;

                m->id[0] = '\0';
                if (mons[i].noutput > 0)
                    edid_id_for_output(mons[i].outputs[0], m->id, OG_ID_LEN);
                if (!m->id[0])
                    snprintf(m->id, OG_ID_LEN, "conn-%s", aname ? aname : "unknown");

                snprintf(m->name, OG_NAME_LEN, "%s (%dx%d)%s",
                         aname ? aname : "display",
                         m->bounds.w, m->bounds.h,
                         m->primary ? " *" : "");
                if (aname) XFree(aname);
                m->native = (void *)(size_t)i;
                ++n;
            }
            XRRFreeMonitors(mons);
        }
    }

    if (n == 0 && G.has_randr) {
        /* XRRGetMonitors needs RandR 1.5. On an older server, walk the CRTCs
         * instead so a multi-head desktop is still seen as several screens
         * rather than one giant one. */
        XRRScreenResources *res = XRRGetScreenResourcesCurrent(G.dpy, G.root);
        if (res) {
            int c;
            RROutput primary = XRRGetOutputPrimary(G.dpy, G.root);
            for (c = 0; c < res->ncrtc && n < max; ++c) {
                XRRCrtcInfo *ci = XRRGetCrtcInfo(G.dpy, res, res->crtcs[c]);
                if (!ci) continue;
                if (ci->mode != None && ci->noutput > 0) {
                    og_monitor *m = &out[n];
                    XRROutputInfo *oi = XRRGetOutputInfo(G.dpy, res, ci->outputs[0]);
                    memset(m, 0, sizeof(*m));
                    m->bounds.x = ci->x;
                    m->bounds.y = ci->y;
                    m->bounds.w = (int)ci->width;
                    m->bounds.h = (int)ci->height;
                    m->primary  = (ci->outputs[0] == primary);
                    if (!edid_id_for_output(ci->outputs[0], m->id, OG_ID_LEN))
                        snprintf(m->id, OG_ID_LEN, "conn-%s",
                                 oi && oi->name ? oi->name : "unknown");
                    snprintf(m->name, OG_NAME_LEN, "%s (%dx%d)%s",
                             oi && oi->name ? oi->name : "display",
                             m->bounds.w, m->bounds.h, m->primary ? " *" : "");
                    if (oi) XRRFreeOutputInfo(oi);
                    m->native = (void *)(size_t)n;
                    ++n;
                }
                XRRFreeCrtcInfo(ci);
            }
            XRRFreeScreenResources(res);
        }
    }

    if (n == 0) {   /* no RandR at all, or it reported nothing usable */
        og_monitor *m = &out[0];
        memset(m, 0, sizeof(*m));
        m->bounds.x = 0;
        m->bounds.y = 0;
        m->bounds.w = DisplayWidth(G.dpy, G.screen);
        m->bounds.h = DisplayHeight(G.dpy, G.screen);
        m->primary = 1;
        snprintf(m->id, OG_ID_LEN, "x11-screen-%d", G.screen);
        snprintf(m->name, OG_NAME_LEN, "X screen %d (%dx%d)",
                 G.screen, m->bounds.w, m->bounds.h);
        n = 1;
    }

    G.overlay_count = n;
    return n;
}

int og_plat_displays_changed(void)
{
    int r = G.displays_dirty;
    G.displays_dirty = 0;
    return r;
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

int og_plat_cursor_pos(int *x, int *y)
{
    Window rr, cr;
    int rx, ry, wx, wy;
    unsigned mask;
    if (!G.dpy) return 0;
    if (!XQueryPointer(G.dpy, G.root, &rr, &cr, &rx, &ry, &wx, &wy, &mask))
        return 0;   /* pointer is on another X screen */
    if (x) *x = rx;
    if (y) *y = ry;
    return 1;
}

long og_plat_idle_ms(void)
{
    static XScreenSaverInfo *info = NULL;
    if (!G.dpy || !G.has_xss) return -1;
    if (!info) info = XScreenSaverAllocInfo();
    if (!info) return -1;
    if (!XScreenSaverQueryInfo(G.dpy, G.root, info)) return -1;
    return (long)info->idle;
}

unsigned og_plat_inhibit_flags(void)
{
    return og_linux_dbus_inhibit_flags();
}

/* ------------------------------------------------------------------ */
/* Fullscreen window detection                                          */
/* ------------------------------------------------------------------ */

static int window_is_fullscreen(Window w)
{
    Atom type;
    int fmt;
    unsigned long n = 0, after = 0, i;
    unsigned char *data = NULL;
    int found = 0;

    if (XGetWindowProperty(G.dpy, w, G.a_net_wm_state, 0, 32, False, XA_ATOM,
                           &type, &fmt, &n, &after, &data) != Success)
        return 0;
    if (data && type == XA_ATOM) {
        Atom *atoms = (Atom *)data;
        for (i = 0; i < n; ++i)
            if (atoms[i] == G.a_net_wm_state_fullscreen) { found = 1; break; }
    }
    if (data) XFree(data);
    return found;
}

static void refresh_fullscreen_cache(void)
{
    Atom type;
    int fmt;
    unsigned long n = 0, after = 0, i;
    unsigned char *data = NULL;
    long now = og_plat_now_ms();

    if (now - G.fs_cache_ms < 500) return;
    G.fs_cache_ms = now;
    G.fs_count = 0;

    if (XGetWindowProperty(G.dpy, G.root, G.a_net_client_list, 0, 1024, False,
                           XA_WINDOW, &type, &fmt, &n, &after, &data) != Success)
        return;

    if (data && type == XA_WINDOW) {
        Window *wins = (Window *)data;
        for (i = 0; i < n && G.fs_count < 16; ++i) {
            XWindowAttributes wa;
            Window child;
            int gx = 0, gy = 0;

            if (!XGetWindowAttributes(G.dpy, wins[i], &wa)) continue;
            if (wa.map_state != IsViewable) continue;
            if (!window_is_fullscreen(wins[i])) continue;
            if (!XTranslateCoordinates(G.dpy, wins[i], G.root, 0, 0,
                                       &gx, &gy, &child)) continue;

            G.fs_rect[G.fs_count].x = gx;
            G.fs_rect[G.fs_count].y = gy;
            G.fs_rect[G.fs_count].w = wa.width;
            G.fs_rect[G.fs_count].h = wa.height;
            ++G.fs_count;
        }
    }
    if (data) XFree(data);
}

int og_plat_monitor_has_fullscreen(const og_monitor *m)
{
    int i;
    if (!G.dpy || !m) return 0;
    refresh_fullscreen_cache();
    for (i = 0; i < G.fs_count; ++i) {
        /* Cover at least 90% of the monitor in each axis and start near it. */
        const og_rect *r = &G.fs_rect[i];
        int dx = r->x - m->bounds.x, dy = r->y - m->bounds.y;
        if (dx < -8 || dx > m->bounds.w / 2) continue;
        if (dy < -8 || dy > m->bounds.h / 2) continue;
        if (r->w * 10 >= m->bounds.w * 9 && r->h * 10 >= m->bounds.h * 9)
            return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Content sampling                                                     */
/* ------------------------------------------------------------------ */

int og_plat_capture_available(void)
{
    if (!G.dpy || !G.has_render) return 0;
    if (G.is_wayland) return 0;   /* XWayland shows us nothing useful */
    return 1;
}

static int ensure_sig_targets(void)
{
    XRenderPictFormat *fmt;
    XRenderPictureAttributes pa;

    if (G.sig_dst && G.root_src) return 1;

    fmt = XRenderFindVisualFormat(G.dpy, DefaultVisual(G.dpy, G.screen));
    if (!fmt) return 0;

    if (!G.sig_pix) {
        G.sig_pix = XCreatePixmap(G.dpy, G.root, OG_SIG_COLS, OG_SIG_ROWS,
                                  (unsigned)DefaultDepth(G.dpy, G.screen));
        if (!G.sig_pix) return 0;
    }
    if (!G.sig_dst) {
        G.sig_dst = XRenderCreatePicture(G.dpy, G.sig_pix, fmt, 0, NULL);
        if (!G.sig_dst) return 0;
    }
    if (!G.root_src) {
        /* IncludeInferiors is essential: without it we would sample the root
         * window's own (usually empty) contents rather than what is on screen. */
        pa.subwindow_mode = IncludeInferiors;
        G.root_src = XRenderCreatePicture(G.dpy, G.root, fmt,
                                          CPSubwindowMode, &pa);
        if (!G.root_src) return 0;
    }
    return 1;
}

static int mask_shift(unsigned long mask)
{
    int s = 0;
    if (!mask) return 0;
    while (!(mask & 1)) { mask >>= 1; ++s; }
    return s;
}

static int mask_bits(unsigned long mask)
{
    int b = 0;
    while (mask) { b += (int)(mask & 1); mask >>= 1; }
    return b ? b : 8;
}

int og_plat_capture_signature(const og_monitor *m, og_signature *sig)
{
    XTransform xf;
    XImage *img;
    int px, py;
    int rs, gs, bs, rb, gb, bb;

    if (!og_plat_capture_available() || !m || !sig) return 0;
    if (m->bounds.w <= 0 || m->bounds.h <= 0) return 0;
    if (!ensure_sig_targets()) return 0;

    /* Map destination pixel (dx,dy) back to source pixel
     * (dx * w/COLS + bounds.x, dy * h/ROWS + bounds.y). */
    memset(&xf, 0, sizeof xf);
    xf.matrix[0][0] = XDoubleToFixed((double)m->bounds.w / (double)OG_SIG_COLS);
    xf.matrix[0][2] = XDoubleToFixed((double)m->bounds.x);
    xf.matrix[1][1] = XDoubleToFixed((double)m->bounds.h / (double)OG_SIG_ROWS);
    xf.matrix[1][2] = XDoubleToFixed((double)m->bounds.y);
    xf.matrix[2][2] = XDoubleToFixed(1.0);

    XRenderSetPictureTransform(G.dpy, G.root_src, &xf);
    XRenderSetPictureFilter(G.dpy, G.root_src, FilterBilinear, NULL, 0);
    XRenderComposite(G.dpy, PictOpSrc, G.root_src, None, G.sig_dst,
                     0, 0, 0, 0, 0, 0, OG_SIG_COLS, OG_SIG_ROWS);
    XSync(G.dpy, False);

    img = XGetImage(G.dpy, G.sig_pix, 0, 0, OG_SIG_COLS, OG_SIG_ROWS,
                    AllPlanes, ZPixmap);
    if (!img) return 0;

    /* An XImage read back from a Pixmap has no visual behind it, so Xlib
     * leaves red_mask/green_mask/blue_mask at zero - unlike one read from a
     * Window. Taking the masks from the screen's visual instead is what
     * makes this work; using the image's own masks silently yields luma 0
     * for every cell, and therefore a screen that always looks static. */
    {
        Visual *vis = DefaultVisual(G.dpy, G.screen);
        unsigned long rm = img->red_mask   ? img->red_mask   : vis->red_mask;
        unsigned long gm = img->green_mask ? img->green_mask : vis->green_mask;
        unsigned long bm = img->blue_mask  ? img->blue_mask  : vis->blue_mask;

        if (!rm && !gm && !bm) {
            /* Not a TrueColor visual. Palette indices carry no luminance, so
             * use the raw index as a change signal; it still detects motion,
             * it just cannot be called brightness. */
            for (py = 0; py < OG_SIG_ROWS; ++py)
                for (px = 0; px < OG_SIG_COLS; ++px)
                    sig->cell[py * OG_SIG_COLS + px] =
                        (unsigned char)(XGetPixel(img, px, py) & 0xFF);
            XDestroyImage(img);
            sig->valid = 1;
            return 1;
        }
        img->red_mask = rm;
        img->green_mask = gm;
        img->blue_mask = bm;
    }

    rs = mask_shift(img->red_mask);   rb = mask_bits(img->red_mask);
    gs = mask_shift(img->green_mask); gb = mask_bits(img->green_mask);
    bs = mask_shift(img->blue_mask);  bb = mask_bits(img->blue_mask);

    for (py = 0; py < OG_SIG_ROWS; ++py) {
        for (px = 0; px < OG_SIG_COLS; ++px) {
            unsigned long p = XGetPixel(img, px, py);
            unsigned r = (unsigned)((p & img->red_mask)   >> rs);
            unsigned g = (unsigned)((p & img->green_mask) >> gs);
            unsigned b = (unsigned)((p & img->blue_mask)  >> bs);
            /* normalise each channel to 8 bits regardless of visual depth */
            if (rb != 8) r = rb ? (r * 255u) / ((1u << rb) - 1u) : 0;
            if (gb != 8) g = gb ? (g * 255u) / ((1u << gb) - 1u) : 0;
            if (bb != 8) b = bb ? (b * 255u) / ((1u << bb) - 1u) : 0;
            /* Rec.601 luma, integer */
            sig->cell[py * OG_SIG_COLS + px] =
                (unsigned char)((77u * r + 150u * g + 29u * b) >> 8);
        }
    }
    XDestroyImage(img);
    sig->valid = 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Overlays                                                             */
/* ------------------------------------------------------------------ */

static og_overlay_x11 *slot_for(og_monitor *m)
{
    int i;
    if (m->overlay) return (og_overlay_x11 *)m->overlay;
    for (i = 0; i < OG_MAX_MONITORS; ++i) {
        if (!G.overlays[i].win && !G.overlays[i].mapped) {
            m->overlay = &G.overlays[i];
            return &G.overlays[i];
        }
    }
    return NULL;
}

static void set_opacity(Window w, double frac)
{
    unsigned long v;
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    v = (unsigned long)(frac * 0xFFFFFFFFul);
    XChangeProperty(G.dpy, w, G.a_net_wm_opacity, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&v, 1);
}

int og_plat_overlay_show(og_monitor *m, int opacity_pct, int fade_ms)
{
    og_overlay_x11 *ov;
    XSetWindowAttributes swa;
    unsigned long pixel = 0;   /* always pure black */

    if (!G.dpy || !m) return 0;
    ov = slot_for(m);
    if (!ov) return 0;

    if (opacity_pct < 10)  opacity_pct = 10;
    if (opacity_pct > 100) opacity_pct = 100;
    ov->target_alpha = (double)opacity_pct / 100.0;

    if (!ov->win) {
        swa.override_redirect = True;      /* the WM must not manage or stack this */
        swa.background_pixel  = pixel;
        swa.border_pixel      = 0;
        swa.event_mask        = ExposureMask;
        swa.save_under        = True;
        ov->win = XCreateWindow(G.dpy, G.root,
                                m->bounds.x, m->bounds.y,
                                (unsigned)m->bounds.w, (unsigned)m->bounds.h,
                                0, CopyFromParent, InputOutput, CopyFromParent,
                                CWOverrideRedirect | CWBackPixel | CWBorderPixel |
                                CWEventMask | CWSaveUnder, &swa);
        if (!ov->win) return 0;

        /* Click-through: an empty input shape means the pointer interacts with
         * whatever is underneath. The blanker is a curtain, not a lock screen. */
        if (G.has_shape)
            XShapeCombineRectangles(G.dpy, ov->win, ShapeInput, 0, 0,
                                    NULL, 0, ShapeSet, Unsorted);

        XChangeProperty(G.dpy, ov->win, G.a_net_wm_window_type, XA_ATOM, 32,
                        PropModeReplace,
                        (unsigned char *)&G.a_net_wm_window_type_dock, 1);
    } else {
        XSetWindowBackground(G.dpy, ov->win, pixel);
        XMoveResizeWindow(G.dpy, ov->win, m->bounds.x, m->bounds.y,
                          (unsigned)m->bounds.w, (unsigned)m->bounds.h);
    }

    ov->fade_ms = fade_ms > 0 ? fade_ms : 0;
    ov->fade_start_ms = og_plat_now_ms();
    /* Both the fade and any opacity below 100% need a compositor. Without
     * one _NET_WM_WINDOW_OPACITY is ignored and the curtain is simply fully
     * black, which fails in the safe direction: more protection, not less. */
    set_opacity(ov->win, ov->fade_ms ? 0.0 : ov->target_alpha);

    XMapRaised(G.dpy, ov->win);
    XRaiseWindow(G.dpy, ov->win);
    XClearWindow(G.dpy, ov->win);
    XFlush(G.dpy);
    ov->mapped = 1;
    return 1;
}

void og_plat_overlay_hide(og_monitor *m)
{
    og_overlay_x11 *ov;
    if (!G.dpy || !m || !m->overlay) return;
    ov = (og_overlay_x11 *)m->overlay;
    if (!ov->win || !ov->mapped) return;
    XUnmapWindow(G.dpy, ov->win);
    XFlush(G.dpy);
    ov->mapped = 0;
    ov->fade_ms = 0;
}

void og_plat_overlay_destroy(og_monitor *m)
{
    og_overlay_x11 *ov;
    if (!G.dpy || !m || !m->overlay) return;
    ov = (og_overlay_x11 *)m->overlay;
    if (ov->win) XDestroyWindow(G.dpy, ov->win);
    memset(ov, 0, sizeof(*ov));
    m->overlay = NULL;
    XFlush(G.dpy);
}

/* ------------------------------------------------------------------ */
/* Per-tick housekeeping                                                */
/* ------------------------------------------------------------------ */

void og_plat_pump(void)
{
    long now;
    int i;

    if (!G.dpy) return;

    while (XPending(G.dpy)) {
        XEvent ev;
        XNextEvent(G.dpy, &ev);
        if (G.has_randr &&
            (ev.type == G.randr_event_base + RRScreenChangeNotify ||
             ev.type == G.randr_event_base + RRNotify)) {
            XRRUpdateConfiguration(&ev);
            G.displays_dirty = 1;
        }
    }

    now = og_plat_now_ms();
    for (i = 0; i < OG_MAX_MONITORS; ++i) {
        og_overlay_x11 *ov = &G.overlays[i];
        if (!ov->win || !ov->mapped || !ov->fade_ms) continue;
        {
            long dt = now - ov->fade_start_ms;
            if (dt >= ov->fade_ms) {
                set_opacity(ov->win, ov->target_alpha);
                ov->fade_ms = 0;
            } else {
                set_opacity(ov->win,
                            ov->target_alpha * (double)dt / (double)ov->fade_ms);
            }
            /* Keep the curtain on top of anything that raised itself. */
            XRaiseWindow(G.dpy, ov->win);
        }
    }
    XFlush(G.dpy);
}

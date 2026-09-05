/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 George
 */
/* og_plat_mac.m - macOS backend: CoreGraphics, IOKit power assertions, Cocoa.
 *
 * Two coordinate spaces are in play and mixing them up is the classic macOS
 * multi-monitor bug:
 *
 *   CoreGraphics  origin at the top-left of the main display, y increases down.
 *                 CGDisplayBounds and CGEventGetLocation both use this.
 *   Cocoa         origin at the bottom-left of the main display, y increases up.
 *                 NSWindow frames use this.
 *
 * The core works entirely in CG coordinates. cg_to_cocoa() is the only place
 * the conversion happens.
 */
#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>
#import <IOKit/pwr_mgt/IOPMLib.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include "oledguard/og_platform.h"
#include "oledguard/og_log.h"

#include <string.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */

@interface OGOverlayWindow : NSWindow
@end
@implementation OGOverlayWindow
/* A borderless window refuses key status by default, which is what we want:
 * the curtain must never steal focus from whatever is underneath. */
- (BOOL)canBecomeKeyWindow  { return NO; }
- (BOOL)canBecomeMainWindow { return NO; }
@end

typedef struct {
    void *win;            /* OGOverlayWindow *, retained */
    int   visible;
    long  fade_start_ms;
    int   fade_ms;
    double target_alpha;  /* what the fade climbs to, not always 1.0 */
} og_overlay_mac;

/* ScreenCaptureKit only captures asynchronously, so each monitor keeps the
 * most recent completed sample and the core collects it on the next tick.
 * The lag is one sample interval and is identical for every comparison, so
 * change detection is unaffected. */
typedef struct {
    og_signature latest;
    int  have;        /* latest holds a completed capture */
    int  consumed;    /* the core has already taken that capture */
    int  in_flight;
    int  failed;
    long started_ms;
} og_capture_slot;

static struct {
    int initialised;
    int displays_dirty;

    CGDirectDisplayID ids[OG_MAX_MONITORS];
    int id_count;

    og_overlay_mac overlays[OG_MAX_MONITORS];
    og_capture_slot cap[OG_MAX_MONITORS];

    /* fullscreen-window cache */
    long fs_cache_ms;
    CGRect fs_rect[24];
    int  fs_count;

    int capture_checked;
    int capture_ok;
} G;

/* ------------------------------------------------------------------ */

long og_plat_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

const char *og_plat_name(void) { return "macOS (CoreGraphics)"; }

static void display_reconfig_cb(CGDirectDisplayID display,
                                CGDisplayChangeSummaryFlags flags,
                                void *userInfo)
{
    (void)display; (void)userInfo;
    /* BeginConfiguration fires before the change lands; ignore it and act on
     * the settled state only. */
    if (flags & kCGDisplayBeginConfigurationFlag) return;
    G.displays_dirty = 1;
}

int og_plat_init(void)
{
    memset(&G, 0, sizeof G);
    CGDisplayRegisterReconfigurationCallback(display_reconfig_cb, NULL);
    G.initialised = 1;
    return 1;
}

void og_plat_shutdown(void)
{
    int i;
    if (!G.initialised) return;
    CGDisplayRemoveReconfigurationCallback(display_reconfig_cb, NULL);
    for (i = 0; i < OG_MAX_MONITORS; ++i) {
        if (G.overlays[i].win) {
            OGOverlayWindow *w = (__bridge_transfer OGOverlayWindow *)G.overlays[i].win;
            [w orderOut:nil];
            G.overlays[i].win = NULL;
            (void)w;
        }
    }
    G.initialised = 0;
}

/* ------------------------------------------------------------------ */
/* Displays                                                             */
/* ------------------------------------------------------------------ */

/* Height of the display that owns the Cocoa origin, used for the flip. */
static CGFloat cocoa_origin_height(void)
{
    NSArray<NSScreen *> *screens = [NSScreen screens];
    if (screens.count == 0) return 0;
    return NSHeight([screens firstObject].frame);
}

static NSRect cg_to_cocoa(og_rect r)
{
    CGFloat h = cocoa_origin_height();
    return NSMakeRect((CGFloat)r.x, h - (CGFloat)(r.y + r.h),
                      (CGFloat)r.w, (CGFloat)r.h);
}

static NSString *display_name(CGDirectDisplayID did)
{
    for (NSScreen *s in [NSScreen screens]) {
        NSNumber *n = s.deviceDescription[@"NSScreenNumber"];
        if (n && (CGDirectDisplayID)[n unsignedIntValue] == did) {
            if (@available(macOS 10.15, *)) return s.localizedName;
            return @"Display";
        }
    }
    return @"Display";
}

int og_plat_enum_monitors(og_monitor *out, int max)
{
    CGDirectDisplayID list[OG_MAX_MONITORS];
    uint32_t n = 0, i;

    if (!out || max <= 0) return 0;
    if (CGGetActiveDisplayList(OG_MAX_MONITORS, list, &n) != kCGErrorSuccess)
        return 0;

    G.id_count = 0;
    for (i = 0; i < n && (int)i < max; ++i) {
        CGDirectDisplayID did = list[i];
        CGRect b = CGDisplayBounds(did);
        og_monitor *m = &out[i];

        memset(m, 0, sizeof(*m));
        m->bounds.x = (int)b.origin.x;
        m->bounds.y = (int)b.origin.y;
        m->bounds.w = (int)b.size.width;
        m->bounds.h = (int)b.size.height;
        m->primary  = CGDisplayIsMain(did) ? 1 : 0;

        /* Vendor+model+serial is stable across reboots and reconnects.
         * Serial is 0 on plenty of panels, so the unit number is folded in
         * to keep two identical monitors distinguishable. */
        snprintf(m->id, OG_ID_LEN, "cg-%08x-%08x-%08x-%u",
                 CGDisplayVendorNumber(did),
                 CGDisplayModelNumber(did),
                 CGDisplaySerialNumber(did),
                 CGDisplayUnitNumber(did));
        snprintf(m->name, OG_NAME_LEN, "%s (%dx%d)%s",
                 [display_name(did) UTF8String],
                 m->bounds.w, m->bounds.h, m->primary ? " *" : "");

        G.ids[i] = did;
        m->native = (void *)(size_t)(i + 1);   /* 1-based index into G.ids */
        ++G.id_count;
    }
    return (int)G.id_count;
}

static CGDirectDisplayID did_for(const og_monitor *m)
{
    int i = (int)(size_t)m->native - 1;
    if (i < 0 || i >= G.id_count) return kCGNullDirectDisplay;
    return G.ids[i];
}

int og_plat_displays_changed(void)
{
    int r = G.displays_dirty;
    G.displays_dirty = 0;
    return r;
}

/* ------------------------------------------------------------------ */
/* Input                                                                */
/* ------------------------------------------------------------------ */

int og_plat_cursor_pos(int *x, int *y)
{
    /* CGEventGetLocation already reports in the CG (top-left) space, so no
     * flip is needed here. NSEvent.mouseLocation would need one. */
    CGEventRef e = CGEventCreate(NULL);
    CGPoint p;
    if (!e) return 0;
    p = CGEventGetLocation(e);
    CFRelease(e);
    if (x) *x = (int)p.x;
    if (y) *y = (int)p.y;
    return 1;
}

long og_plat_idle_ms(void)
{
    CFTimeInterval s = CGEventSourceSecondsSinceLastEventType(
        kCGEventSourceStateCombinedSessionState, kCGAnyInputEventType);
    if (s < 0) return -1;
    return (long)(s * 1000.0);
}

unsigned og_plat_inhibit_flags(void)
{
    /* IOPMCopyAssertionsStatus gives the aggregate count per assertion type
     * across every process. Safari, Chrome, QuickTime, IINA, VLC and Steam
     * all raise PreventUserIdleDisplaySleep while something is playing and
     * drop it on pause, which makes this the single most reliable signal on
     * macOS - better than looking at pixels. */
    CFDictionaryRef status = NULL;
    unsigned flags = OG_INHIBIT_NONE;
    static const CFStringRef display_keys[] = {
        CFSTR("PreventUserIdleDisplaySleep"),
        CFSTR("NoDisplaySleepAssertion"),
        CFSTR("PreventDisplayIdleSleep")
    };
    static const CFStringRef idle_keys[] = {
        CFSTR("PreventUserIdleSystemSleep"),
        CFSTR("NoIdleSleepAssertion")
    };
    size_t k;

    if (IOPMCopyAssertionsStatus(&status) != kIOReturnSuccess || !status)
        return OG_INHIBIT_NONE;

    for (k = 0; k < sizeof display_keys / sizeof display_keys[0]; ++k) {
        CFNumberRef v = CFDictionaryGetValue(status, display_keys[k]);
        int c = 0;
        if (v && CFNumberGetValue(v, kCFNumberIntType, &c) && c > 0) {
            flags |= OG_INHIBIT_DISPLAY;
            break;
        }
    }
    for (k = 0; k < sizeof idle_keys / sizeof idle_keys[0]; ++k) {
        CFNumberRef v = CFDictionaryGetValue(status, idle_keys[k]);
        int c = 0;
        if (v && CFNumberGetValue(v, kCFNumberIntType, &c) && c > 0) {
            flags |= OG_INHIBIT_IDLE;
            break;
        }
    }
    CFRelease(status);
    return flags;
}

int og_plat_inhibitor_name(char *buf, size_t len)
{
    /* Assertions are attributable, and saying which app is holding the
     * display on turns a mysterious "it never blanks" into something the
     * user can act on. Amphetamine, Caffeine, Endurance, screen sharing and
     * remote-desktop tools are the usual culprits. */
    CFDictionaryRef byProcess = NULL;
    CFIndex n, i;
    int found = 0;

    if (!buf || len == 0) return 0;
    buf[0] = '\0';

    if (IOPMCopyAssertionsByProcess(&byProcess) != kIOReturnSuccess || !byProcess)
        return 0;

    n = CFDictionaryGetCount(byProcess);
    if (n > 0) {
        const void **values = (const void **)calloc((size_t)n, sizeof(void *));
        if (values) {
            CFDictionaryGetKeysAndValues(byProcess, NULL, values);
            for (i = 0; i < n && !found; ++i) {
                CFArrayRef list = (CFArrayRef)values[i];
                CFIndex m, j;
                if (!list || CFGetTypeID(list) != CFArrayGetTypeID()) continue;
                m = CFArrayGetCount(list);
                for (j = 0; j < m; ++j) {
                    CFDictionaryRef a = CFArrayGetValueAtIndex(list, j);
                    CFStringRef type, name;
                    if (!a) continue;
                    type = CFDictionaryGetValue(a, kIOPMAssertionTypeKey);
                    if (!type) continue;
                    /* Only the display-related ones matter here. */
                    if (CFStringFind(type, CFSTR("Display"), 0).location == kCFNotFound)
                        continue;
                    /* "Process Name" is the documented key in the dictionary
                     * IOPMCopyAssertionsByProcess returns; there is no
                     * kIOPMAssertion... constant for it in the SDK. Fall back
                     * to the assertion's own name, which is at least a human
                     * description of what is holding the display on. */
                    name = CFDictionaryGetValue(a, CFSTR("Process Name"));
                    if (!name) name = CFDictionaryGetValue(a, kIOPMAssertionNameKey);
                    if (name && CFGetTypeID(name) == CFStringGetTypeID() &&
                        CFStringGetCString(name, buf, (CFIndex)len,
                                           kCFStringEncodingUTF8)) {
                        found = 1;
                        break;
                    }
                }
            }
            free(values);
        }
    }
    CFRelease(byProcess);
    return found;
}

/* ------------------------------------------------------------------ */
/* Fullscreen detection                                                 */
/* ------------------------------------------------------------------ */

static void refresh_fullscreen_cache(void)
{
    CFArrayRef list;
    CFIndex i, n;
    long now = og_plat_now_ms();

    if (now - G.fs_cache_ms < 500) return;
    G.fs_cache_ms = now;
    G.fs_count = 0;

    /* Window bounds and layer are readable without Screen Recording
     * permission; only the owner name and the pixels are gated. */
    list = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
        kCGNullWindowID);
    if (!list) return;

    n = CFArrayGetCount(list);
    for (i = 0; i < n && G.fs_count < 24; ++i) {
        CFDictionaryRef d = CFArrayGetValueAtIndex(list, i);
        CFNumberRef layer_n = CFDictionaryGetValue(d, kCGWindowLayer);
        CFDictionaryRef bounds_d = CFDictionaryGetValue(d, kCGWindowBounds);
        CGRect r;
        int layer = 0;

        if (!layer_n || !bounds_d) continue;
        CFNumberGetValue(layer_n, kCFNumberIntType, &layer);
        if (layer != 0) continue;   /* skip the menu bar, dock, overlays */
        if (!CGRectMakeWithDictionaryRepresentation(bounds_d, &r)) continue;

        G.fs_rect[G.fs_count++] = r;
    }
    CFRelease(list);
}

int og_plat_monitor_has_fullscreen(const og_monitor *m)
{
    int i;

    if (!m) return 0;

    /* CGDisplayIsCaptured was the old way to spot a game holding a display
     * exclusively. It has been deprecated since 10.9 and modern macOS games
     * use a fullscreen space instead, which the window-geometry check below
     * sees perfectly well. */
    refresh_fullscreen_cache();
    for (i = 0; i < G.fs_count; ++i) {
        CGRect r = G.fs_rect[i];
        double dx = r.origin.x - m->bounds.x;
        double dy = r.origin.y - m->bounds.y;
        if (dx < -8 || dx > m->bounds.w / 2.0) continue;
        if (dy < -8 || dy > m->bounds.h / 2.0) continue;
        if (r.size.width  * 10.0 >= m->bounds.w * 9.0 &&
            r.size.height * 10.0 >= m->bounds.h * 9.0)
            return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Content sampling                                                     */
/* ------------------------------------------------------------------ */

/* Cached SCShareableContent. Fetching it is itself asynchronous and not
 * cheap, so it is refreshed every few seconds rather than per sample.
 * ObjC objects live in file statics rather than in G, since ARC and plain C
 * structs make uneasy neighbours. */
static NSArray *gSCDisplays = nil;      /* SCDisplay *   */
static NSArray *gSCSelfApps = nil;      /* SCRunningApplication *, this process */
static BOOL     gSCFetching = NO;
static long     gSCFetchedMs = -1000000;

static void sck_refresh_content(void)
{
    if (@available(macOS 14.0, *)) {
        long now = og_plat_now_ms();
        if (gSCFetching) return;
        if (gSCDisplays && now - gSCFetchedMs < 5000) return;
        gSCFetching = YES;

        [SCShareableContent getShareableContentWithCompletionHandler:
            ^(SCShareableContent *content, NSError *error) {
                NSArray *displays = content ? content.displays : nil;
                NSArray *apps     = content ? content.applications : nil;
                dispatch_async(dispatch_get_main_queue(), ^{
                    NSMutableArray *mine = [NSMutableArray array];
                    pid_t me = getpid();
                    for (SCRunningApplication *a in apps)
                        if (a.processID == me) [mine addObject:a];
                    gSCDisplays  = displays;
                    gSCSelfApps  = mine;
                    gSCFetchedMs = og_plat_now_ms();
                    gSCFetching  = NO;
                    if (error)
                        og_log(OG_LOG_DEBUG, "SCShareableContent: %s",
                               error.localizedDescription.UTF8String);
                });
            }];
    }
}

/* Draw a captured frame into the 32x18 luma grid. Stretching a non-16:9
 * display into a 16:9 grid distorts the picture, which does not matter in
 * the slightest: every cell still maps to a fixed region of that screen and
 * the comparison is between two identically distorted frames. */
static int image_to_signature(CGImageRef img, og_signature *sig)
{
    CGColorSpaceRef cs;
    CGContextRef ctx;
    unsigned char buf[OG_SIG_CELLS * 4];
    int k;

    if (!img || !sig) return 0;

    cs = CGColorSpaceCreateDeviceRGB();
    memset(buf, 0, sizeof buf);
    ctx = CGBitmapContextCreate(buf, OG_SIG_COLS, OG_SIG_ROWS, 8,
                                OG_SIG_COLS * 4, cs,
                                kCGImageAlphaNoneSkipLast |
                                kCGBitmapByteOrder32Big);
    if (!ctx) { CGColorSpaceRelease(cs); return 0; }

    CGContextSetInterpolationQuality(ctx, kCGInterpolationLow);
    CGContextDrawImage(ctx, CGRectMake(0, 0, OG_SIG_COLS, OG_SIG_ROWS), img);

    for (k = 0; k < OG_SIG_CELLS; ++k) {
        unsigned r = buf[k * 4 + 0], g = buf[k * 4 + 1], b = buf[k * 4 + 2];
        sig->cell[k] = (unsigned char)((77u * r + 150u * g + 29u * b) >> 8);
    }
    sig->valid = 1;

    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
    return 1;
}

static void sck_start_capture(int idx, CGDirectDisplayID did, int mon_w, int mon_h)
{
    if (@available(macOS 14.0, *)) {
        SCDisplay *target = nil;
        SCContentFilter *filter;
        SCStreamConfiguration *cfg;

        for (SCDisplay *d in gSCDisplays)
            if (d.displayID == did) { target = d; break; }
        if (!target) return;   /* content not fetched yet; try again next tick */

        /* Excluding our own process means a curtain can never end up in the
         * sample and hold itself down. */
        if (gSCSelfApps.count > 0)
            filter = [[SCContentFilter alloc] initWithDisplay:target
                                        excludingApplications:gSCSelfApps
                                             exceptingWindows:@[]];
        else
            filter = [[SCContentFilter alloc] initWithDisplay:target
                                             excludingWindows:@[]];

        cfg = [[SCStreamConfiguration alloc] init];
        /* Ask for roughly 1/16 scale at the display's own aspect ratio and do
         * the final squeeze into the grid ourselves. Requesting 32x18 from
         * ScreenCaptureKit directly would letterbox anything not 16:9 and
         * waste a fifth of the cells on constant black bars. */
        cfg.width  = mon_w / 16 < OG_SIG_COLS ? OG_SIG_COLS : mon_w / 16;
        cfg.height = mon_h / 16 < OG_SIG_ROWS ? OG_SIG_ROWS : mon_h / 16;
        cfg.showsCursor = NO;      /* a blinking pointer is not playback */
        cfg.scalesToFit = YES;

        G.cap[idx].in_flight = 1;
        G.cap[idx].started_ms = og_plat_now_ms();

        [SCScreenshotManager captureImageWithFilter:filter
                                      configuration:cfg
                                  completionHandler:^(CGImageRef image,
                                                      NSError *err) {
            /* Convert here, on whatever queue this lands on, so no CGImage
             * ownership has to survive the hop back to the main thread. */
            og_signature sig;
            int ok;
            memset(&sig, 0, sizeof sig);
            ok = image_to_signature(image, &sig);

            dispatch_async(dispatch_get_main_queue(), ^{
                G.cap[idx].in_flight = 0;
                if (ok) {
                    G.cap[idx].latest   = sig;
                    G.cap[idx].have     = 1;
                    G.cap[idx].consumed = 0;
                    G.cap[idx].failed   = 0;
                } else {
                    G.cap[idx].failed = 1;
                    if (err)
                        og_log(OG_LOG_DEBUG, "capture failed: %s",
                               err.localizedDescription.UTF8String);
                }
            });
        }];
    }
}

int og_plat_capture_available(void)
{
    if (G.capture_checked) return G.capture_ok;
    G.capture_checked = 1;
    G.capture_ok = 0;

    if (@available(macOS 14.0, *)) {
        G.capture_ok = CGPreflightScreenCaptureAccess() ? 1 : 0;
        if (!G.capture_ok)
            og_log(OG_LOG_WARN,
                   "Screen Recording permission not granted; falling back to "
                   "power assertions only. Grant it in System Settings > "
                   "Privacy & Security > Screen Recording.");
    } else {
        og_log(OG_LOG_WARN,
               "screen sampling needs macOS 14 or later (ScreenCaptureKit); "
               "falling back to power assertions only");
    }
    return G.capture_ok;
}

/* Ask the system for Screen Recording access. Called once from the UI, not
 * from the hot path: the prompt only ever appears the first time. */
void og_mac_request_capture_access(void)
{
    if (@available(macOS 10.15, *)) {
        if (!CGPreflightScreenCaptureAccess()) CGRequestScreenCaptureAccess();
        G.capture_checked = 0;   /* re-evaluate on the next tick */
    }
}

int og_plat_capture_signature(const og_monitor *m, og_signature *sig)
{
    og_capture_slot *s;
    CGDirectDisplayID did;
    int idx, delivered = 0;

    if (!m || !sig || !og_plat_capture_available()) return 0;
    idx = (int)(size_t)m->native - 1;
    if (idx < 0 || idx >= G.id_count) return 0;
    did = did_for(m);
    if (did == kCGNullDirectDisplay) return 0;
    s = &G.cap[idx];

    /* A capture that never comes back must not wedge this slot for good. */
    if (s->in_flight && og_plat_now_ms() - s->started_ms > 5000) {
        s->in_flight = 0;
        s->failed = 1;
        og_log(OG_LOG_DEBUG, "capture timed out on %s", m->name);
    }

    if (s->have && !s->consumed) {
        *sig = s->latest;
        s->consumed = 1;
        delivered = 1;
    }

    sck_refresh_content();
    if (!s->in_flight) sck_start_capture(idx, did, m->bounds.w, m->bounds.h);

    /* The first call after startup has nothing to hand back yet; the core
     * treats that as "no sample this interval" and carries on. */
    return delivered;
}

/* ------------------------------------------------------------------ */
/* Overlays                                                             */
/* ------------------------------------------------------------------ */

static og_overlay_mac *slot_for(og_monitor *m)
{
    int i;
    if (m->overlay) return (og_overlay_mac *)m->overlay;
    for (i = 0; i < OG_MAX_MONITORS; ++i) {
        if (!G.overlays[i].win) {
            m->overlay = &G.overlays[i];
            return &G.overlays[i];
        }
    }
    return NULL;
}

int og_plat_overlay_show(og_monitor *m, int opacity_pct, int fade_ms)
{
    og_overlay_mac *ov;
    OGOverlayWindow *w;
    NSRect frame;
    double alpha;

    if (!m) return 0;
    ov = slot_for(m);
    if (!ov) return 0;

    frame = cg_to_cocoa(m->bounds);
    if (opacity_pct < 10)  opacity_pct = 10;
    if (opacity_pct > 100) opacity_pct = 100;
    alpha = (double)opacity_pct / 100.0;
    ov->target_alpha = alpha;

    if (!ov->win) {
        w = [[OGOverlayWindow alloc] initWithContentRect:frame
                                               styleMask:NSWindowStyleMaskBorderless
                                                 backing:NSBackingStoreBuffered
                                                   defer:NO];
        /* Above every ordinary window, including other apps' fullscreen
         * spaces. Deliberately not a lock screen: input passes straight
         * through and the curtain lifts the instant the pointer moves. */
        w.level = (NSWindowLevel)CGShieldingWindowLevel();
        w.hasShadow = NO;
        w.ignoresMouseEvents = YES;
        w.releasedWhenClosed = NO;
        w.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                               NSWindowCollectionBehaviorStationary |
                               NSWindowCollectionBehaviorIgnoresCycle |
                               NSWindowCollectionBehaviorFullScreenAuxiliary;
        ov->win = (__bridge_retained void *)w;
    } else {
        w = (__bridge OGOverlayWindow *)ov->win;
        [w setFrame:frame display:NO];
    }

    /* Always black. Opacity is carried by the window's alpha, not by a grey
     * fill: a grey curtain lights every pixel it covers, which is the exact
     * opposite of what an OLED needs. */
    w.backgroundColor = [NSColor blackColor];
    w.opaque = (alpha >= 1.0);
    ov->fade_ms = fade_ms > 0 ? fade_ms : 0;
    ov->fade_start_ms = og_plat_now_ms();
    [w setAlphaValue:(ov->fade_ms ? 0.0 : alpha)];
    [w orderFrontRegardless];
    ov->visible = 1;
    return 1;
}

void og_plat_overlay_hide(og_monitor *m)
{
    og_overlay_mac *ov;
    OGOverlayWindow *w;
    if (!m || !m->overlay) return;
    ov = (og_overlay_mac *)m->overlay;
    if (!ov->win || !ov->visible) return;
    w = (__bridge OGOverlayWindow *)ov->win;
    [w orderOut:nil];
    ov->visible = 0;
    ov->fade_ms = 0;
}

void og_plat_overlay_destroy(og_monitor *m)
{
    og_overlay_mac *ov;
    if (!m || !m->overlay) return;
    ov = (og_overlay_mac *)m->overlay;
    if (ov->win) {
        OGOverlayWindow *w = (__bridge_transfer OGOverlayWindow *)ov->win;
        [w orderOut:nil];
        w = nil;
    }
    memset(ov, 0, sizeof(*ov));
    m->overlay = NULL;
}

/* ------------------------------------------------------------------ */

void og_plat_pump(void)
{
    long now = og_plat_now_ms();
    int i;

    for (i = 0; i < OG_MAX_MONITORS; ++i) {
        og_overlay_mac *ov = &G.overlays[i];
        OGOverlayWindow *w;
        long dt;

        if (!ov->win || !ov->visible || !ov->fade_ms) continue;
        w = (__bridge OGOverlayWindow *)ov->win;
        dt = now - ov->fade_start_ms;
        if (dt >= ov->fade_ms) {
            [w setAlphaValue:(CGFloat)ov->target_alpha];
            ov->fade_ms = 0;
        } else {
            [w setAlphaValue:(CGFloat)(ov->target_alpha * (double)dt / (double)ov->fade_ms)];
        }
    }
}

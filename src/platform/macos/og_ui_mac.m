/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 George
 */
/* og_ui_mac.m - menu bar item, settings panel and the macOS entry point.
 *
 * The app is an accessory (no Dock icon, no menu bar of its own): it lives in
 * the status bar, which is where a background utility belongs.
 */
#import <Cocoa/Cocoa.h>

#include "oledguard/og_core.h"
#include "oledguard/og_config.h"
#include "oledguard/og_platform.h"
#include "oledguard/og_log.h"

#include <stdio.h>
#include <string.h>

void og_mac_request_capture_access(void);   /* from og_plat_mac.m */

/* Liquid Glass arrived in the macOS 26 SDK. The deployment target here is
 * 12.3 and the project is meant to build with Command Line Tools, which may
 * carry an older SDK, so the glass code needs a compile-time guard as well as
 * the runtime @available checks. Without the SDK, every card falls back to a
 * plain rounded box and the layout is otherwise identical. */
#if defined(__MAC_OS_X_VERSION_MAX_ALLOWED) && __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000
#  define OG_HAVE_GLASS 1
#else
#  define OG_HAVE_GLASS 0
#endif

/* SMAppService, the modern login-item API, is macOS 13. Same guard shape as
 * the glass one: below 13 the checkbox is simply absent rather than the build
 * failing. The old LSSharedFileList path is not worth reviving - it has been
 * deprecated for years and writes into a list the user cannot audit. */
#if defined(__MAC_OS_X_VERSION_MAX_ALLOWED) && __MAC_OS_X_VERSION_MAX_ALLOWED >= 130000
#  define OG_HAVE_LOGIN_ITEM 1
#  import <ServiceManagement/ServiceManagement.h>
#else
#  define OG_HAVE_LOGIN_ITEM 0
#endif

static const int kTimeoutPresets[] = { 15, 30, 60, 120, 300, 600, 1200, 1800 };
static const int kTimeoutPresetCount =
    (int)(sizeof kTimeoutPresets / sizeof kTimeoutPresets[0]);

/* ------------------------------------------------------------------ */

/* An NSClipView is not flipped, so a document view with no height of its own
 * settles at the bottom-left of the scroll area instead of filling it from
 * the top. Wrapping the list in a flipped container is the fix. */
@interface OGFlippedView : NSView
@end
@implementation OGFlippedView
- (BOOL)isFlipped { return YES; }
@end

/* The window's backdrop.
 *
 * Liquid Glass refracts and blurs whatever sits behind it, so glass over a
 * flat grey window has nothing to work with and reads as a plain panel. This
 * gives it something: a diagonal wash in the icon's own colours, with a soft
 * bloom standing in for the light thrown by a lit screen. */
/* Set from the config before the window is built. A file static because the
 * view is constructed by AppKit machinery that has nowhere to carry it. */
static double g_tint = 0.45;
/* Set when macOS's own Reduce Transparency is on. That setting means "no
 * blur", not merely "less of it", so the gradient goes fully solid and the
 * vibrancy layer is taken out of the window altogether. */
static BOOL g_opaque = NO;

@interface OGGradientView : NSView
@end
@implementation OGGradientView

/* Deliberately not opaque: an NSVisualEffectView underneath is blurring the
 * desktop, and this only tints it. Claiming opacity would let AppKit skip
 * drawing what is behind and the window would go solid again. */
- (BOOL)isOpaque { return NO; }

- (void)drawRect:(NSRect)dirty
{
    NSAppearanceName match = [self.effectiveAppearance
        bestMatchFromAppearancesWithNames:@[ NSAppearanceNameAqua,
                                             NSAppearanceNameDarkAqua ]];
    BOOL dark = [match isEqualToString:NSAppearanceNameDarkAqua];
    NSGradient *base, *bloom;
    CGFloat a0, a1, a2, a3;

    /* The alpha climbs down the window, and that ramp is the whole effect:
     * near the title bar it is barely there, so the blurred desktop reads
     * through as glass; by the bottom it is almost solid black. Transparent
     * at the top, purple through the middle, black at the foot. */
    /* One ramp, four stops; opacity is the only thing the accessibility path
     * changes, so the colours stay identical either way. */
    a0 = g_opaque ? 1.0 : 0.02 * g_tint;
    a1 = g_opaque ? 1.0 : 0.34 * g_tint;
    a2 = g_opaque ? 1.0 : 0.62 * g_tint;
    a3 = g_opaque ? 1.0 : 0.86 * g_tint;

    if (dark) {
        base = [[NSGradient alloc] initWithColorsAndLocations:
            [NSColor colorWithSRGBRed:0.30 green:0.27 blue:0.49 alpha:a0], 0.00,
            [NSColor colorWithSRGBRed:0.27 green:0.24 blue:0.46 alpha:a1], 0.28,
            [NSColor colorWithSRGBRed:0.14 green:0.12 blue:0.27 alpha:a2], 0.66,
            [NSColor colorWithSRGBRed:0.04 green:0.03 blue:0.08 alpha:a3], 1.00,
            nil];
        bloom = [[NSGradient alloc]
            initWithStartingColor:[NSColor colorWithSRGBRed:0.30 green:0.79
                                                       blue:0.94 alpha:0.22 * g_tint]
                      endingColor:[NSColor colorWithSRGBRed:0.30 green:0.79
                                                       blue:0.94 alpha:0.0]];
    } else {
        /* The same idea at a fraction of the strength: in light appearance a
         * deep violet would fight every control on top of it. */
        base = [[NSGradient alloc] initWithColorsAndLocations:
            [NSColor colorWithSRGBRed:0.88 green:0.86 blue:0.97 alpha:a0], 0.00,
            [NSColor colorWithSRGBRed:0.86 green:0.84 blue:0.96 alpha:a1 * 0.76], 0.28,
            [NSColor colorWithSRGBRed:0.93 green:0.92 blue:0.98 alpha:a2 * 0.81], 0.66,
            [NSColor colorWithSRGBRed:0.99 green:0.98 blue:1.00 alpha:a3 * 0.88], 1.00,
            nil];
        bloom = [[NSGradient alloc]
            initWithStartingColor:[NSColor colorWithSRGBRed:0.35 green:0.62
                                                       blue:0.95 alpha:0.13 * g_tint]
                      endingColor:[NSColor colorWithSRGBRed:0.35 green:0.62
                                                       blue:0.95 alpha:0.0]];
    }

    [base drawInRect:self.bounds angle:-75.0];
    [bloom drawInRect:self.bounds relativeCenterPosition:NSMakePoint(0.32, 0.40)];
}

- (void)viewDidChangeEffectiveAppearance
{
    [super viewDidChangeEffectiveAppearance];
    self.needsDisplay = YES;   /* repaint when the user flips light/dark */
}
@end

/* One monitor's row inside the menu bar panel. */
@interface OGPanelRow : NSObject
@property (nonatomic, strong) NSSwitch    *toggle;
@property (nonatomic, strong) NSTextField *name;
@property (nonatomic, strong) NSTextField *status;
@property (nonatomic, strong) NSStackView *view;
@property (nonatomic, copy)   NSString    *ident;
@end
@implementation OGPanelRow
@end

@interface OGMonitorRow : NSObject
@property (nonatomic, strong) NSSwitch    *toggle;
@property (nonatomic, strong) NSImageView *glyph;
@property (nonatomic, strong) NSTextField *name;
@property (nonatomic, strong) NSTextField *detail;
@property (nonatomic, strong) NSTextField *dot;
@property (nonatomic, strong) NSTextField *status;
@property (nonatomic, strong) NSStackView *view;
@property (nonatomic, copy)   NSString    *ident;
@end
@implementation OGMonitorRow
@end

/* ------------------------------------------------------------------ */

@interface OGAppDelegate : NSObject <NSApplicationDelegate, NSPopoverDelegate,
                                     NSWindowDelegate>
@end

@implementation OGAppDelegate {
    og_config      _cfg;
    char           _cfgPath[1024];
    og_core       *_core;

    NSStatusItem  *_statusItem;
    NSMenuItem    *_pauseItem;
    NSWindow      *_settings;
    NSStackView   *_root;
    NSVisualEffectView *_vibrancy;
    OGGradientView     *_backdrop;
    NSMenuItem    *_reduceItemApp;
    NSMenuItem    *_reduceItemStatus;
    NSStackView   *_monitorStack;
    NSLayoutConstraint *_monitorHeight;
    NSTextField   *_summary;
    NSButton      *_permissionButton;
    NSButton      *_pauseButton;
    NSButton      *_menuBarCheck;
    NSButton      *_dockCheck;
    NSButton      *_loginCheck;
    NSTextField   *_loginHint;
    BOOL           _firstRun;
    BOOL           _windowUp;   /* settings window on screen */
    NSMutableArray<OGMonitorRow *> *_rows;

    NSPopover     *_popover;
    NSMenu        *_fallbackMenu;
    NSStackView   *_panelMonStack;
    NSTextField   *_panelSummary;
    NSButton      *_panelPause;
    NSSlider      *_panelOpacity;
    NSMutableArray<OGPanelRow *> *_panelRows;

    NSTimer       *_tick;
    NSTimer       *_uiTimer;
    BOOL           _suppress;
}

/* ---- lifecycle ------------------------------------------------- */

- (void)applicationDidFinishLaunching:(NSNotification *)note
{
    _rows = [NSMutableArray array];
    _panelRows = [NSMutableArray array];

    og_config_defaults(&_cfg);
    if (!og_config_path(_cfgPath, sizeof _cfgPath)) {
        NSLog(@"OLEDGuard: cannot determine a config directory");
        [NSApp terminate:nil];
        return;
    }
    /* og_config_load reports 0 when there was no file to read, which is the
     * only honest signal that this is a first run. */
    _firstRun = !og_config_load(&_cfg, _cfgPath);

    if (!og_plat_init()) { [NSApp terminate:nil]; return; }
    _core = og_core_create(&_cfg);
    if (!_core) { [NSApp terminate:nil]; return; }

    [self buildMainMenu];
    [self applyPresence];
    [self applyLoginItem];
    [self applyTransparency];
    [[[NSWorkspace sharedWorkspace] notificationCenter]
        addObserver:self
           selector:@selector(accessibilityOptionsChanged:)
               name:NSWorkspaceAccessibilityDisplayOptionsDidChangeNotification
             object:nil];

    _tick = [NSTimer scheduledTimerWithTimeInterval:(double)OG_TICK_MS / 1000.0
                                            repeats:YES
                                              block:^(NSTimer *t) {
        og_core_tick(self->_core);
    }];
    /* Common mode so the engine keeps ticking while a menu is open. */
    [[NSRunLoop currentRunLoop] addTimer:_tick forMode:NSRunLoopCommonModes];

    _uiTimer = [NSTimer scheduledTimerWithTimeInterval:0.25
                                               repeats:YES
                                                 block:^(NSTimer *t) {
        [self refreshUI];
    }];
    /* Common mode as well: a status-item click can put the run loop into
     * event tracking, and a panel whose numbers freeze while it is open is
     * worse than no numbers. */
    [[NSRunLoop currentRunLoop] addTimer:_uiTimer forMode:NSRunLoopCommonModes];

    /* Open the window on a genuine first run only - there is nothing to pick
     * yet - and otherwise stay quietly in the background. */
    if (_firstRun || !_cfg.start_minimised) [self openSettings:nil];
}

/* Clicking the Dock icon, or opening the app while it is already running,
 * is the user asking for the window. Nothing else surfaces it. */
- (BOOL)applicationShouldHandleReopen:(NSApplication *)sender
                    hasVisibleWindows:(BOOL)hasVisible
{
    [self openSettings:nil];
    return YES;
}

- (void)applicationWillTerminate:(NSNotification *)note
{
    og_core_wake_all(_core);
/* Deliberately not saving here.
 *
 * Every setting is written the moment it changes, so a save on the way out
 * adds nothing - and it actively destroys work: edit the ini by hand while
 * the app is running, quit, and this would write the stale in-memory copy
 * straight back over the edit. Hand-editing then looks like it did nothing. */
    og_core_destroy(_core);
    _core = NULL;
    og_plat_shutdown();
}

- (void)saveConfig { og_config_save(&_cfg, _cfgPath); }

/* ---- status bar ------------------------------------------------ */

/* An app menu, so Cmd-Q and a Dock right-click behave normally once the Dock
 * icon is switched on. An accessory app never displays it, so building it
 * unconditionally costs nothing. */
- (void)buildMainMenu
{
    NSMenu *bar = [[NSMenu alloc] init];
    NSMenuItem *appItem = [[NSMenuItem alloc] init];
    NSMenu *appMenu = [[NSMenu alloc] init];

    [appMenu addItemWithTitle:@"Settings…"
                       action:@selector(openSettings:) keyEquivalent:@","].target = self;
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Blank now"
                       action:@selector(blankNow:) keyEquivalent:@""].target = self;
    _reduceItemApp = [appMenu addItemWithTitle:@"Reduce transparency"
                                        action:@selector(toggleReduceTransparency:)
                                 keyEquivalent:@""];
    _reduceItemApp.target = self;
    [appMenu addItem:[NSMenuItem separatorItem]];
    /* nil targets below: these travel the responder chain, performClose: to
     * the key window and the rest to NSApp. */
    [appMenu addItemWithTitle:@"Close Window"
                       action:@selector(performClose:) keyEquivalent:@"w"];
    [appMenu addItemWithTitle:@"Hide OLEDGuard"
                       action:@selector(hide:) keyEquivalent:@"h"];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Quit OLEDGuard"
                       action:@selector(terminate:) keyEquivalent:@"q"];
    /* macOS draws the first menu's title from the bundle name, but the title
     * is what VoiceOver reads, so it is set rather than left blank. */
    appMenu.title = @"OLEDGuard";
    appItem.submenu = appMenu;
    [bar addItem:appItem];
    NSApp.mainMenu = bar;
}

/* Translucency, from three inputs: the configured tint, this app's own
 * accessibility toggle, and the system-wide Reduce Transparency setting.
 * The system one wins and is the strongest: it removes the blur entirely
 * rather than just tinting over it. */
- (void)applyTransparency
{
    BOOL sysReduce = [[NSWorkspace sharedWorkspace]
                        accessibilityDisplayShouldReduceTransparency];
    BOOL reduce = _cfg.reduce_transparency || sysReduce;

    g_tint   = reduce ? 1.0 : (double)_cfg.window_tint_pct / 100.0;
    g_opaque = sysReduce;

    _reduceItemApp.state    = _cfg.reduce_transparency ? NSControlStateValueOn
                                                       : NSControlStateValueOff;
    _reduceItemStatus.state = _reduceItemApp.state;
    /* The system setting is not ours to change, so the menu item says so
     * rather than pretending a click would do anything. */
    _reduceItemApp.enabled    = !sysReduce;
    _reduceItemStatus.enabled = !sysReduce;

    if (!_settings) return;   /* called before the window exists: globals only */

    _settings.opaque = g_opaque;
    _settings.backgroundColor = g_opaque ? [NSColor windowBackgroundColor]
                                         : [NSColor clearColor];
    _vibrancy.hidden = g_opaque;
    _backdrop.needsDisplay = YES;
}

- (void)toggleReduceTransparency:(id)sender
{
    _cfg.reduce_transparency = !_cfg.reduce_transparency;
    [self applyTransparency];
    [self saveConfig];
}

- (void)accessibilityOptionsChanged:(NSNotification *)n
{
    [self applyTransparency];
}

/* Where the running app is visible. This is the difference between a
 * background utility and a lost process, so the one rule enforced here is
 * that it can never be neither. */
- (void)applyPresence
{
    if (!_cfg.menu_bar_icon && !_cfg.dock_icon) _cfg.dock_icon = 1;

    if (_cfg.menu_bar_icon && !_statusItem) {
        [self buildStatusItem];
    } else if (!_cfg.menu_bar_icon && _statusItem) {
        [[NSStatusBar systemStatusBar] removeStatusItem:_statusItem];
        _statusItem = nil;
        _pauseItem = nil;
    }

    /* An Accessory app never owns the menu bar, however key its window is:
     * focus the settings window and the menu bar still belongs to whatever
     * was in front before, so there is no OLEDGuard menu and no Quit in it.
     * The only way to own the menu bar is Regular, so the app becomes Regular
     * for as long as the window is on screen and drops back to Accessory when
     * it closes. A Dock icon rides along for that time - macOS ties the two
     * together - which is a fair trade for a window that behaves like a
     * window. */
    BOOL windowUp = _windowUp && _settings;
    [NSApp setActivationPolicy:((_cfg.dock_icon || windowUp)
        ? NSApplicationActivationPolicyRegular
        : NSApplicationActivationPolicyAccessory)];

    og_log(OG_LOG_INFO, "visible as:%s%s%s",
           _cfg.menu_bar_icon ? " menu bar" : "",
           windowUp && !_cfg.dock_icon ? " window" : "",
           _cfg.dock_icon ? " dock" : "");
}

- (void)presenceChanged:(NSButton *)b
{
    if (_suppress) return;
    _cfg.menu_bar_icon = (_menuBarCheck.state == NSControlStateValueOn);
    _cfg.dock_icon     = (_dockCheck.state == NSControlStateValueOn);

    [self applyPresence];

    /* applyPresence may have forced one back on; reflect that in the boxes
     * rather than letting them lie about the state. */
    _suppress = YES;
    _menuBarCheck.state = _cfg.menu_bar_icon ? NSControlStateValueOn
                                             : NSControlStateValueOff;
    _dockCheck.state    = _cfg.dock_icon ? NSControlStateValueOn
                                         : NSControlStateValueOff;
    _suppress = NO;

    [self saveConfig];
    /* Switching to a Dock icon reorders windows; put ours back in front. */
    [NSApp activateIgnoringOtherApps:YES];
    [_settings makeKeyAndOrderFront:nil];
}

/* Start at login.
 *
 * A burn-in guard that only runs when you remember to start it is not doing
 * its job, so this defaults to on and reconciles itself on every launch: if
 * the config says yes and the system says no, register.
 *
 * Two things are deliberate. It registers rather than silently assuming -
 * macOS can drop the registration when the bundle moves - and it reports
 * RequiresApproval honestly instead of pretending success, because that state
 * means the user (or a profile) switched it off in System Settings and only
 * they can switch it back.
 *
 * SMAppService registers the bundle at its current path. Moving OLEDGuard.app
 * afterwards leaves a stale entry; launching it from the new location fixes
 * itself on the next tick through here. */
- (void)applyLoginItem
{
    NSString *note = @"";

#if OG_HAVE_LOGIN_ITEM
    if (@available(macOS 13.0, *)) {
        SMAppService *svc = [SMAppService mainAppService];
        NSError *err = nil;
        BOOL want = _cfg.open_at_login ? YES : NO;
        BOOL have = (svc.status == SMAppServiceStatusEnabled);

        if (want && !have) {
            if (svc.status == SMAppServiceStatusRequiresApproval) {
                note = @"Turned off in System Settings › General › "
                       @"Login Items. Only you can turn it back on there.";
            } else if (![svc registerAndReturnError:&err]) {
                note = [NSString stringWithFormat:@"Could not register: %@",
                        err.localizedDescription ?: @"unknown error"];
                og_log(OG_LOG_WARN, "login item register failed: %s",
                       err.localizedDescription.UTF8String ?: "?");
            }
        } else if (!want && have) {
            if (![svc unregisterAndReturnError:&err])
                og_log(OG_LOG_WARN, "login item unregister failed: %s",
                       err.localizedDescription.UTF8String ?: "?");
        }

        /* An app run straight out of a build directory registers that path.
         * Worth saying once, because it is the difference between "it did not
         * work" and "it is guarding a copy you have since replaced". */
        if (_cfg.open_at_login && !note.length) {
            NSString *path = NSBundle.mainBundle.bundlePath;
            if (![path hasPrefix:@"/Applications/"])
                note = @"Registered at the app's current location. Move it to "
                       @"Applications first if you plan to keep it there.";
        }
    } else {
        note = @"Needs macOS 13 or later.";
    }
#else
    note = @"This build was compiled without login-item support.";
#endif

    _loginHint.stringValue = note;
    _loginHint.hidden = (note.length == 0);
    _suppress = YES;
    _loginCheck.state = _cfg.open_at_login ? NSControlStateValueOn
                                           : NSControlStateValueOff;
    _suppress = NO;
}

- (void)loginItemChanged:(NSButton *)b
{
    if (_suppress) return;
    _cfg.open_at_login = (b.state == NSControlStateValueOn);
    [self applyLoginItem];
    [self saveConfig];
}

/* The menu bar glyph: the app icon reduced to what survives at 18 points.
 * A monitor outline with the left half filled - blanked screen, live screen,
 * the whole idea in two shapes.
 *
 * Drawn rather than loaded. A named image or an SF Symbol can come back nil,
 * and a status item with no image and no title is zero pixels wide: present,
 * working, and invisible. Drawing cannot fail, and it stays crisp on any
 * display scale. */
static NSImage *og_screen_image(CGFloat side)
{
    NSImage *img = [NSImage imageWithSize:NSMakeSize(side, side)
                                  flipped:NO
                           drawingHandler:^BOOL(NSRect dirty) {
        /* One 18-unit design grid scaled to whatever size was asked for, so
         * the menu bar glyph and the list glyph are literally the same mark. */
        CGFloat k = side / 18.0;
        NSRect body = NSMakeRect(1.6 * k, 3.6 * k, 14.8 * k, 10.8 * k);
        NSBezierPath *screen, *dark;

        [[NSColor blackColor] set];

        /* Two shapes and nothing else. No stand, no detail: at 18 points
         * anything more turns to grey mush against the menu bar. */
        screen = [NSBezierPath bezierPathWithRoundedRect:
                    NSInsetRect(body, 0.7 * k, 0.7 * k)
                                                 xRadius:2.0 * k yRadius:2.0 * k];
        screen.lineWidth = 1.4 * k;
        [screen stroke];

        /* The blanked half, clipped to the outline so it follows the corners.
         * Stopping just short of centre leaves a clean seam. */
        [NSGraphicsContext saveGraphicsState];
        [screen addClip];
        dark = [NSBezierPath bezierPathWithRect:
                NSMakeRect(NSMinX(body), NSMinY(body),
                           NSWidth(body) / 2.0 - 0.5 * k, NSHeight(body))];
        [dark fill];
        [NSGraphicsContext restoreGraphicsState];
        return YES;
    }];

    /* Template mode lets AppKit recolour it for light and dark menu bars and
     * for the highlighted state. The unfilled half stays transparent, so the
     * half-on/half-off reading comes for free. */
    img.template = YES;
    return img;
}

static NSImage *og_menu_bar_image(void) { return og_screen_image(18.0); }

/* The name the backend formats carries its resolution, which is useful in a
 * log line and redundant beside a field showing the same thing. */
static NSString *og_short_name(const og_monitor *m)
{
    NSString *n = [NSString stringWithUTF8String:m->name];
    NSRange r = [n rangeOfString:@" (" options:NSBackwardsSearch];
    if (r.location != NSNotFound) n = [n substringToIndex:r.location];
    return n;
}

static NSBox *og_separator(void)
{
    NSBox *b = [[NSBox alloc] init];
    b.boxType = NSBoxSeparator;
    return b;
}

/* ---- menu bar panel -------------------------------------------- */

/* A popover rather than a menu, so the common actions - which screens are
 * guarded, how opaque the curtain is, blank now - are reachable without
 * opening the settings window at all. The menu survives as a right-click
 * fallback: if the popover ever fails to appear there is still a way to quit. */
- (void)buildPopover
{
    NSViewController *vc = [[NSViewController alloc] init];
    NSStackView *root, *actions;
    NSView *host;

    _panelSummary = [NSTextField labelWithString:@""];
    _panelSummary.font = [NSFont systemFontOfSize:[NSFont smallSystemFontSize]];
    _panelSummary.textColor = [NSColor secondaryLabelColor];

    _panelMonStack = [[NSStackView alloc] init];
    _panelMonStack.orientation = NSUserInterfaceLayoutOrientationVertical;
    _panelMonStack.alignment = NSLayoutAttributeLeading;
    _panelMonStack.spacing = 10;

    _panelOpacity = [NSSlider sliderWithValue:_cfg.curtain_opacity
                                     minValue:10 maxValue:100
                                       target:self
                                       action:@selector(panelOpacityChanged:)];

    _panelPause = [NSButton buttonWithTitle:@"Pause"
                                     target:self action:@selector(togglePause:)];
    actions = [NSStackView stackViewWithViews:@[
        [NSButton buttonWithTitle:@"Blank now"
                           target:self action:@selector(blankNow:)],
        _panelPause,
        [NSButton buttonWithTitle:@"Settings…"
                           target:self action:@selector(openSettingsFromPanel:)],
        [NSButton buttonWithTitle:@"Quit"
                           target:self action:@selector(quit:)]
    ]];
    actions.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    actions.spacing = 6;
    actions.distribution = NSStackViewDistributionFillEqually;

    root = [NSStackView stackViewWithViews:@[
        [self heading:@"OLEDGuard"],
        _panelSummary,
        _panelMonStack,
        og_separator(),
        [self hint:@"Curtain opacity"],
        _panelOpacity,
        actions
    ]];
    root.orientation = NSUserInterfaceLayoutOrientationVertical;
    root.alignment = NSLayoutAttributeLeading;
    root.spacing = 10;
    root.edgeInsets = NSEdgeInsetsMake(16, 16, 16, 16);
    root.translatesAutoresizingMaskIntoConstraints = NO;

    host = [[NSView alloc] init];
    [host addSubview:root];
    [NSLayoutConstraint activateConstraints:@[
        [root.leadingAnchor  constraintEqualToAnchor:host.leadingAnchor],
        [root.trailingAnchor constraintEqualToAnchor:host.trailingAnchor],
        [root.topAnchor      constraintEqualToAnchor:host.topAnchor],
        [root.bottomAnchor   constraintEqualToAnchor:host.bottomAnchor],
        [root.widthAnchor    constraintEqualToConstant:320],
        [_panelMonStack.widthAnchor  constraintEqualToAnchor:root.widthAnchor
                                                    constant:-32],
        [_panelOpacity.widthAnchor   constraintEqualToAnchor:root.widthAnchor
                                                    constant:-32],
        [actions.widthAnchor         constraintEqualToAnchor:root.widthAnchor
                                                    constant:-32]
    ]];
    vc.view = host;

    _popover = [[NSPopover alloc] init];
    _popover.contentViewController = vc;
    _popover.behavior = NSPopoverBehaviorTransient;
    /* Only so the status item's highlight can be dropped when the popover
     * closes by any route - Escape, a click elsewhere, or the button again. */
    _popover.delegate = self;
    _popover.animates = YES;

    [self rebuildPanelRows];
}

- (void)rebuildPanelRows
{
    int i, n;

    for (NSView *v in [_panelMonStack.views copy]) [_panelMonStack removeView:v];
    [_panelRows removeAllObjects];

    n = og_core_monitor_count(_core);
    for (i = 0; i < n; ++i) {
        og_core_status st;
        OGPanelRow *r;
        NSStackView *text;
        NSView *spacer;

        if (!og_core_status_at(_core, i, &st)) continue;

        r = [OGPanelRow new];
        r.ident = [NSString stringWithUTF8String:st.mon->id];

        r.name = [NSTextField labelWithString:og_short_name(st.mon)];
        r.name.font = [NSFont systemFontOfSize:[NSFont systemFontSize]
                                        weight:NSFontWeightMedium];
        r.status = [NSTextField labelWithString:@""];
        r.status.font = [NSFont systemFontOfSize:[NSFont smallSystemFontSize]];
        r.status.textColor = [NSColor secondaryLabelColor];

        text = [NSStackView stackViewWithViews:@[ r.name, r.status ]];
        text.orientation = NSUserInterfaceLayoutOrientationVertical;
        text.alignment = NSLayoutAttributeLeading;
        text.spacing = 1;

        spacer = [[NSView alloc] init];
        [spacer setContentHuggingPriority:NSLayoutPriorityDefaultLow - 1
                           forOrientation:NSLayoutConstraintOrientationHorizontal];

        r.toggle = [[NSSwitch alloc] init];
        r.toggle.tag = i;
        r.toggle.target = self;
        r.toggle.action = @selector(panelToggled:);
        r.toggle.state = st.enabled ? NSControlStateValueOn : NSControlStateValueOff;

        r.view = [NSStackView stackViewWithViews:@[ text, spacer, r.toggle ]];
        r.view.orientation = NSUserInterfaceLayoutOrientationHorizontal;
        r.view.alignment = NSLayoutAttributeCenterY;
        r.view.spacing = 8;
        /* Add first, constrain second. Activating a constraint between two
         * views with no common ancestor yet throws, and the throw happens
         * inside a button action, where AppKit swallows it - which looked
         * exactly like "the first click does nothing and the panel is empty
         * ever after". */
        [_panelMonStack addView:r.view inGravity:NSStackViewGravityTop];
        [r.view.widthAnchor
            constraintEqualToAnchor:_panelMonStack.widthAnchor].active = YES;
        [_panelRows addObject:r];
    }

    /* Never leave a silent gap: an empty list should say why it is empty. */
    if (_panelRows.count == 0) {
        NSTextField *none = [self hint:@"No screens detected."];
        [_panelMonStack addView:none inGravity:NSStackViewGravityTop];
    }
}

- (BOOL)panelRowsMatchCore
{
    int i, n = og_core_monitor_count(_core);
    if ((NSInteger)n != (NSInteger)_panelRows.count) return NO;
    for (i = 0; i < n; ++i) {
        og_core_status st;
        if (!og_core_status_at(_core, i, &st)) return NO;
        if (![_panelRows[i].ident isEqualToString:
              [NSString stringWithUTF8String:st.mon->id]]) return NO;
    }
    return YES;
}

- (void)refreshPanel
{
    int i, blanked = 0, n;

    /* Deliberately not gated on isShown. The panel is built once, before the
     * core has enumerated a single monitor, so its rows start empty; gating
     * the refresh on a popover that is not on screen yet meant the rebuild
     * never ran and the panel opened blank every time. The periodic caller
     * does the isShown check instead, so this stays cheap. */
    if (!_popover) return;
    if (![self panelRowsMatchCore]) [self rebuildPanelRows];

    n = (int)_panelRows.count;
    for (i = 0; i < n; ++i) {
        og_core_status st;
        if (!og_core_status_at(_core, i, &st)) continue;
        _panelRows[i].status.stringValue = [self describe:&st];
        _suppress = YES;
        _panelRows[i].toggle.state = st.enabled ? NSControlStateValueOn
                                                : NSControlStateValueOff;
        _suppress = NO;
        if (st.state == OG_ST_BLANK) ++blanked;
    }

    _panelSummary.stringValue = [NSString stringWithFormat:
        @"%@ — %d of %d screen%s blanked",
        og_core_paused(_core) ? @"Paused" : @"Guarding",
        blanked, n, n == 1 ? "" : "s"];
    _panelPause.title = og_core_paused(_core) ? @"Resume" : @"Pause";
    _panelOpacity.doubleValue = _cfg.curtain_opacity;
}

- (void)panelToggled:(NSSwitch *)sw
{
    if (_suppress) return;
    og_core_set_enabled(_core, (int)sw.tag, sw.state == NSControlStateValueOn);
    [self saveConfig];
}

- (void)panelOpacityChanged:(NSSlider *)s
{
    _cfg.curtain_opacity = (int)s.doubleValue;
    [self saveConfig];
}

- (void)openSettingsFromPanel:(id)sender
{
    [_popover performClose:sender];
    [self openSettings:sender];
}

- (void)statusClicked:(id)sender
{
    NSEvent *ev = NSApp.currentEvent;

    /* Right-click keeps the plain menu. It is the escape hatch if the popover
     * ever misbehaves, and it is where Quit stays reachable. */
    if (ev && (ev.type == NSEventTypeRightMouseUp ||
               (ev.modifierFlags & NSEventModifierFlagControl))) {
        /* popUpContextMenu blocks until the menu is dismissed, so the
         * highlight can simply be bracketed around it. */
        _statusItem.button.highlighted = YES;
        [NSMenu popUpContextMenu:_fallbackMenu withEvent:ev forView:_statusItem.button];
        _statusItem.button.highlighted = NO;
        return;
    }

    if (_popover.isShown) {
        [_popover performClose:sender];
        return;
    }
    if (!_popover) [self buildPopover];
    [self refreshPanel];
    [_popover showRelativeToRect:_statusItem.button.bounds
                          ofView:_statusItem.button
                   preferredEdge:NSRectEdgeMinY];
    /* A transient popover needs the app active to take key, or its controls
     * ignore the first click. */
    [NSApp activateIgnoringOtherApps:YES];
    /* AppKit draws the pressed-in menu bar background by itself only when the
     * status item owns an NSMenu. This one drives a popover from its action
     * instead, so the highlight is ours to manage: set here, cleared in
     * popoverDidClose:. Without it the icon stays flat while its panel is
     * open, which reads as a click that did not register. */
    _statusItem.button.highlighted = YES;
    [self refreshPanel];
}

- (void)popoverDidClose:(NSNotification *)note
{
    _statusItem.button.highlighted = NO;
}

- (void)buildStatusItem
{
    NSMenu *menu = [[NSMenu alloc] init];

    _statusItem = [[NSStatusBar systemStatusBar]
                   statusItemWithLength:NSVariableStatusItemLength];
    _statusItem.button.image = og_menu_bar_image();
    _statusItem.button.toolTip = @"OLEDGuard";
    _statusItem.visible = YES;

    [menu addItemWithTitle:@"Settings…"
                    action:@selector(openSettings:) keyEquivalent:@","].target = self;
    [menu addItem:[NSMenuItem separatorItem]];
    [menu addItemWithTitle:@"Blank now"
                    action:@selector(blankNow:) keyEquivalent:@""].target = self;
    _pauseItem = [menu addItemWithTitle:@"Pause"
                                 action:@selector(togglePause:) keyEquivalent:@""];
    _pauseItem.target = self;
    _reduceItemStatus = [menu addItemWithTitle:@"Reduce transparency"
                                        action:@selector(toggleReduceTransparency:)
                                 keyEquivalent:@""];
    _reduceItemStatus.target = self;
    [menu addItem:[NSMenuItem separatorItem]];
    [menu addItemWithTitle:@"Quit OLEDGuard"
                    action:@selector(quit:) keyEquivalent:@"q"].target = self;

    /* Not assigned as .menu: the button drives the popover instead, and the
     * menu is shown by hand on a right-click. */
    _fallbackMenu = menu;
    _statusItem.button.target = self;
    _statusItem.button.action = @selector(statusClicked:);
    [_statusItem.button sendActionOn:(NSEventMaskLeftMouseUp |
                                      NSEventMaskRightMouseUp)];
}

- (void)blankNow:(id)s   { og_core_blank_now(_core); }
- (void)togglePause:(id)s{ og_core_set_paused(_core, !og_core_paused(_core)); }
- (void)quit:(id)s       { [NSApp terminate:nil]; }

/* ---- settings window ------------------------------------------- */

- (NSStackView *)rowWithLabel:(NSString *)text control:(NSView *)ctl
{
    NSTextField *l = [NSTextField labelWithString:text];
    NSStackView *sv = [NSStackView stackViewWithViews:@[ l, ctl ]];
    sv.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    sv.spacing = 12;
    sv.alignment = NSLayoutAttributeCenterY;
    [sv setHuggingPriority:NSLayoutPriorityDefaultLow
            forOrientation:NSLayoutConstraintOrientationHorizontal];
    return sv;
}

- (NSTextField *)heading:(NSString *)text
{
    NSTextField *t = [NSTextField labelWithString:text];
    t.font = [NSFont boldSystemFontOfSize:[NSFont systemFontSize]];
    return t;
}

- (NSTextField *)hint:(NSString *)text
{
    NSTextField *t = [NSTextField labelWithString:text];
    t.font = [NSFont systemFontOfSize:[NSFont smallSystemFontSize]];
    t.textColor = [NSColor secondaryLabelColor];
    return t;
}

/* A card: one group of settings on its own surface.
 *
 * NSGlassEffectView does not lay out the view assigned to contentView, so the
 * card only takes its size from its contents because of the constraints added
 * here. NSBox, by contrast, manages its own content view, which is why the
 * two branches differ. */
- (NSView *)card:(NSView *)inner
{
    NSView *card = nil;

    inner.translatesAutoresizingMaskIntoConstraints = NO;

#if OG_HAVE_GLASS
    if (@available(macOS 26.0, *)) {
        NSGlassEffectView *g = [[NSGlassEffectView alloc] init];
        g.translatesAutoresizingMaskIntoConstraints = NO;
        g.cornerRadius = 14.0;
        g.contentView = inner;
        [NSLayoutConstraint activateConstraints:@[
            [inner.leadingAnchor  constraintEqualToAnchor:g.leadingAnchor],
            [inner.trailingAnchor constraintEqualToAnchor:g.trailingAnchor],
            [inner.topAnchor      constraintEqualToAnchor:g.topAnchor],
            [inner.bottomAnchor   constraintEqualToAnchor:g.bottomAnchor]
        ]];
        card = g;
    }
#endif
    if (!card) {
        NSBox *b = [[NSBox alloc] init];
        b.translatesAutoresizingMaskIntoConstraints = NO;
        b.boxType = NSBoxCustom;
        b.titlePosition = NSNoTitle;
        b.cornerRadius = 14.0;
        b.borderWidth = 1.0;
        b.borderColor = [NSColor separatorColor];
        b.fillColor = [NSColor controlBackgroundColor];
        b.contentViewMargins = NSMakeSize(0, 0);
        b.contentView = inner;      /* NSBox pins its own content */
        card = b;
    }
    return card;
}

/* The stack that goes inside a card: a heading, then its controls, with the
 * padding that keeps them off the glass edge. */
- (NSStackView *)group:(NSString *)title views:(NSArray<NSView *> *)views
{
    NSMutableArray<NSView *> *all = [NSMutableArray array];
    NSStackView *sv;

    if (title) [all addObject:[self heading:title]];
    [all addObjectsFromArray:views];

    sv = [NSStackView stackViewWithViews:all];
    sv.orientation = NSUserInterfaceLayoutOrientationVertical;
    sv.alignment = NSLayoutAttributeLeading;
    sv.spacing = 10;
    sv.edgeInsets = NSEdgeInsetsMake(16, 16, 16, 16);
    return sv;
}

/* Cards live inside one container so the system can render them as a single
 * glass system rather than several unrelated panes. `spacing` is the distance
 * at which neighbouring cards fuse into one shape; it is deliberately smaller
 * than the gap between them, so they stay separate surfaces. */
- (NSView *)glassContainer:(NSArray<NSView *> *)cards
{
    NSStackView *stack = [NSStackView stackViewWithViews:cards];
    NSView *v;

    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.spacing = 14;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    for (v in cards)
        [v.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;

#if OG_HAVE_GLASS
    if (@available(macOS 26.0, *)) {
        NSGlassEffectContainerView *c = [[NSGlassEffectContainerView alloc] init];
        c.translatesAutoresizingMaskIntoConstraints = NO;
        c.spacing = 6.0;
        c.contentView = stack;
        [NSLayoutConstraint activateConstraints:@[
            [stack.leadingAnchor  constraintEqualToAnchor:c.leadingAnchor],
            [stack.trailingAnchor constraintEqualToAnchor:c.trailingAnchor],
            [stack.topAnchor      constraintEqualToAnchor:c.topAnchor],
            [stack.bottomAnchor   constraintEqualToAnchor:c.bottomAnchor]
        ]];
        return c;
    }
#endif
    return stack;
}

- (void)buildSettingsWindow
{
    NSStackView *root, *buttons;
    NSScrollView *scroll;
    OGFlippedView *doc;
    NSPopUpButton *timeoutPop, *wakePop;
    NSPopUpButton *inhibitPop;
    NSSlider *dim;
    NSView *content, *cards, *spacer;
    OGGradientView *backdrop;
    NSVisualEffectView *vibrancy;
    int i;

    _settings = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 620, 700)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable |
                             NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _settings.title = @"OLEDGuard";
    _settings.releasedWhenClosed = NO;
    _settings.delegate = self;
    /* Let the backdrop run the full height of the window, under the title
     * bar, so the gradient is the window rather than a panel inside one. The
     * traffic lights float over it; the root stack's top inset clears them. */
    _settings.styleMask |= NSWindowStyleMaskFullSizeContentView;
    _settings.titlebarAppearsTransparent = YES;
    _settings.titleVisibility = NSWindowTitleVisible;
    _settings.movableByWindowBackground = YES;
    /* A window only shows what is behind it if it stops painting its own
     * background. Without both of these, an NSVisualEffectView set to blend
     * behind the window has nothing to sample and renders flat grey. */
    _settings.opaque = NO;
    _settings.backgroundColor = [NSColor clearColor];
    [_settings center];

    _summary = [NSTextField labelWithString:@""];
    _summary.textColor = [NSColor secondaryLabelColor];

    _monitorStack = [[NSStackView alloc] init];
    _monitorStack.orientation = NSUserInterfaceLayoutOrientationVertical;
    _monitorStack.alignment = NSLayoutAttributeLeading;
    _monitorStack.spacing = 10;
    _monitorStack.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    _monitorStack.translatesAutoresizingMaskIntoConstraints = NO;

    doc = [[OGFlippedView alloc] init];
    doc.translatesAutoresizingMaskIntoConstraints = NO;
    [doc addSubview:_monitorStack];

    scroll = [[NSScrollView alloc] init];
    scroll.translatesAutoresizingMaskIntoConstraints = NO;
    scroll.hasVerticalScroller = YES;
    scroll.autohidesScrollers = YES;      /* no track when nothing scrolls */
    scroll.scrollerStyle = NSScrollerStyleOverlay;
    scroll.drawsBackground = NO;
    scroll.documentView = doc;

    /* The document view takes its width from the clip view and its height
     * from the list inside it; that pair is what makes the scroller behave. */
    [NSLayoutConstraint activateConstraints:@[
        [doc.leadingAnchor constraintEqualToAnchor:scroll.contentView.leadingAnchor],
        [doc.topAnchor     constraintEqualToAnchor:scroll.contentView.topAnchor],
        [doc.widthAnchor   constraintEqualToAnchor:scroll.contentView.widthAnchor],
        [_monitorStack.leadingAnchor  constraintEqualToAnchor:doc.leadingAnchor],
        [_monitorStack.trailingAnchor constraintEqualToAnchor:doc.trailingAnchor],
        [_monitorStack.topAnchor      constraintEqualToAnchor:doc.topAnchor],
        [_monitorStack.bottomAnchor   constraintEqualToAnchor:doc.bottomAnchor]
    ]];
    /* Sized to the actual list after every rebuild, so two monitors do not
     * leave a window full of empty space. */
    _monitorHeight = [scroll.heightAnchor constraintEqualToConstant:120];
    _monitorHeight.active = YES;

    _permissionButton = [NSButton buttonWithTitle:@"Grant Screen Recording permission…"
                                           target:self
                                           action:@selector(requestPermission:)];
    _permissionButton.hidden = YES;

    timeoutPop = [[NSPopUpButton alloc] init];
    for (i = 0; i < kTimeoutPresetCount; ++i) {
        int s = kTimeoutPresets[i];
        [timeoutPop addItemWithTitle:(s < 60
            ? [NSString stringWithFormat:@"%d seconds", s]
            : [NSString stringWithFormat:@"%d minute%s", s / 60, s == 60 ? "" : "s"])];
        [timeoutPop lastItem].tag = s;
    }
    if (![self selectTag:_cfg.timeout_sec in:timeoutPop]) {
        [timeoutPop addItemWithTitle:
            [NSString stringWithFormat:@"%d seconds (from config file)", _cfg.timeout_sec]];
        [timeoutPop lastItem].tag = _cfg.timeout_sec;
        [timeoutPop selectItemAtIndex:timeoutPop.numberOfItems - 1];
    }
    timeoutPop.target = self;
    timeoutPop.action = @selector(timeoutChanged:);

    wakePop = [[NSPopUpButton alloc] init];
    [wakePop addItemsWithTitles:@[
        @"Pointer movement over that screen only",
        @"Pointer movement, or typing while the pointer rests there",
        @"Any keyboard or mouse activity wakes every screen" ]];
    [wakePop selectItemAtIndex:(NSInteger)_cfg.wake_mode];
    wakePop.target = self;
    wakePop.action = @selector(wakeChanged:);

    inhibitPop = [[NSPopUpButton alloc] init];
    [inhibitPop addItemsWithTitles:@[
        @"Ignore them — what is on screen decides",
        @"Trust them only when screen sampling is unavailable",
        @"Always trust them" ]];
    [inhibitPop selectItemAtIndex:(NSInteger)_cfg.inhibitor_policy];
    inhibitPop.target = self;
    inhibitPop.action = @selector(inhibitChanged:);

    dim = [NSSlider sliderWithValue:_cfg.curtain_opacity minValue:10 maxValue:100
                             target:self action:@selector(opacityChanged:)];
    [dim.widthAnchor constraintEqualToConstant:200].active = YES;

    /* Quit belongs here and not only in the menu bar. An accessory app has no
     * Dock icon and no app menu, so if the menu bar item ever fails to appear
     * this button is the only way out that does not involve a process list. */
    _pauseButton = [NSButton buttonWithTitle:@"Pause"
                                      target:self action:@selector(togglePause:)];
    /* An empty view that hugs nothing, so it absorbs the slack and pushes the
     * buttons to the trailing edge where macOS expects actions. */
    spacer = [[NSView alloc] init];
    [spacer setContentHuggingPriority:NSLayoutPriorityDefaultLow - 1
                       forOrientation:NSLayoutConstraintOrientationHorizontal];

    buttons = [NSStackView stackViewWithViews:@[
        spacer,
        [NSButton buttonWithTitle:@"Blank now"
                           target:self action:@selector(blankNow:)],
        _pauseButton,
        [NSButton buttonWithTitle:@"Quit OLEDGuard"
                           target:self action:@selector(quit:)]
    ]];
    buttons.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    buttons.spacing = 8;
    buttons.translatesAutoresizingMaskIntoConstraints = NO;

    _menuBarCheck = [NSButton checkboxWithTitle:@"Show in the menu bar"
                                         target:self
                                         action:@selector(presenceChanged:)];
    _menuBarCheck.state = _cfg.menu_bar_icon ? NSControlStateValueOn
                                             : NSControlStateValueOff;
    _dockCheck = [NSButton checkboxWithTitle:@"Show in the Dock"
                                      target:self
                                      action:@selector(presenceChanged:)];
    _dockCheck.state = _cfg.dock_icon ? NSControlStateValueOn
                                      : NSControlStateValueOff;
    _loginCheck = [NSButton checkboxWithTitle:@"Open at login"
                                       target:self
                                       action:@selector(loginItemChanged:)];
    _loginCheck.state = _cfg.open_at_login ? NSControlStateValueOn
                                           : NSControlStateValueOff;
    _loginHint = [self hint:@""];
    _loginHint.hidden = YES;

    /* Three surfaces, grouped by the question each answers: which screens,
     * how it behaves, where to find it. Sections as bare headings in one long
     * column is what a pre-26 window looks like; glass needs shapes to sit
     * on, which is the whole point of the restructure. */
    cards = [self glassContainer:@[
        [self card:[self group:@"Screens to guard"
                          views:@[ scroll, _permissionButton ]]],

        [self card:[self group:@"Timing and behaviour" views:@[
            [self rowWithLabel:@"Blank after" control:timeoutPop],
            [self rowWithLabel:@"Wake on" control:wakePop],
            [self rowWithLabel:@"Apps asking to keep the display on"
                       control:inhibitPop],
            [self hint:@"Amphetamine, Caffeine, Endurance and screen sharing "
                       @"hold that request permanently, so always obeying it "
                       @"would stop anything ever blanking."],
            [self rowWithLabel:@"Curtain opacity % (100 = pixels off)" control:dim],
            [self hint:@"The curtain is always black. Below 100% the screen "
                       @"shows faintly through, which costs some protection."]
        ]]],

        [self card:[self group:@"Where to find OLEDGuard" views:@[
            [self hint:@"It keeps guarding after this window is closed. "
                       @"At least one of these stays on."],
            _menuBarCheck,
            _dockCheck,
            _loginCheck,
            _loginHint
        ]]]
    ]];

    root = [NSStackView stackViewWithViews:@[ _summary, cards, buttons ]];
    root.orientation = NSUserInterfaceLayoutOrientationVertical;
    root.alignment = NSLayoutAttributeLeading;
    root.spacing = 14;
    /* 44 at the top clears the traffic lights now that the content view
     * extends under the title bar. */
    root.edgeInsets = NSEdgeInsetsMake(44, 20, 20, 20);
    root.translatesAutoresizingMaskIntoConstraints = NO;
    _root = root;

    /* Both the card stack and the footer span the window; only the summary
     * line is free to be its natural width. */
    [NSLayoutConstraint activateConstraints:@[
        [cards.widthAnchor   constraintEqualToAnchor:root.widthAnchor constant:-40],
        [buttons.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-40]
    ]];

    content = _settings.contentView;

    /* The bottom layer: blurs the desktop behind the window. Everything
     * above it is a tint on top of that blur. */
    vibrancy = [[NSVisualEffectView alloc] init];
    _vibrancy = vibrancy;
    vibrancy.translatesAutoresizingMaskIntoConstraints = NO;
    switch (_cfg.window_material) {
    case 1:  vibrancy.material = NSVisualEffectMaterialPopover;  break;
    case 2:  vibrancy.material = NSVisualEffectMaterialSidebar;  break;
    case 3:  vibrancy.material = NSVisualEffectMaterialUnderWindowBackground; break;
    case 4:  vibrancy.material = NSVisualEffectMaterialWindowBackground; break;
    case 5:  vibrancy.material = NSVisualEffectMaterialFullScreenUI; break;
    default: vibrancy.material = NSVisualEffectMaterialHUDWindow; break;
    }
    vibrancy.blendingMode = NSVisualEffectBlendingModeBehindWindow;
    vibrancy.state = NSVisualEffectStateActive;

    backdrop = [[OGGradientView alloc] init];
    _backdrop = backdrop;
    backdrop.translatesAutoresizingMaskIntoConstraints = NO;

    /* No separate title bar layer. One was tried and it was a second, more
     * opaque blur sitting exactly where the window is supposed to be at its
     * clearest. The base layer already runs the full height, and the tint
     * ramp starts at 2%, so the top is essentially bare glass. */
    [content addSubview:vibrancy];       /* added first, so it stays behind */
    [content addSubview:backdrop];
    [content addSubview:root];

    [NSLayoutConstraint activateConstraints:@[
        [vibrancy.leadingAnchor  constraintEqualToAnchor:content.leadingAnchor],
        [vibrancy.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [vibrancy.topAnchor      constraintEqualToAnchor:content.topAnchor],
        [vibrancy.bottomAnchor   constraintEqualToAnchor:content.bottomAnchor],
        [backdrop.leadingAnchor  constraintEqualToAnchor:content.leadingAnchor],
        [backdrop.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [backdrop.topAnchor      constraintEqualToAnchor:content.topAnchor],
        [backdrop.bottomAnchor   constraintEqualToAnchor:content.bottomAnchor],
        [root.leadingAnchor  constraintEqualToAnchor:content.leadingAnchor],
        [root.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [root.topAnchor      constraintEqualToAnchor:content.topAnchor],
        [root.bottomAnchor   constraintEqualToAnchor:content.bottomAnchor]
    ]];

    [self rebuildMonitorRows];
    [self applyTransparency];   /* now that the window and its layers exist */
    /* The launch-time call ran before these controls existed, so the checkbox
     * and its hint are filled in here. */
    [self applyLoginItem];
}

- (BOOL)selectTag:(int)tag in:(NSPopUpButton *)pop
{
    NSInteger idx = [pop indexOfItemWithTag:tag];
    if (idx < 0) return NO;
    [pop selectItemAtIndex:idx];
    return YES;
}

- (void)openSettings:(id)sender
{
    if (!_settings) [self buildSettingsWindow];
    og_core_wake_all(_core);
    /* Become Regular before the window is ordered in, so it arrives with a
     * menu bar of its own rather than acquiring one a moment later. */
    _windowUp = YES;
    [self applyPresence];
    [NSApp activateIgnoringOtherApps:YES];
    [_settings makeKeyAndOrderFront:nil];
    [self refreshUI];
}

/* Closing the window is the app going back to being a background utility.
 * Deferred by one turn of the run loop because the window is still visible
 * while this notification is being delivered. */
- (void)windowWillClose:(NSNotification *)note
{
    if (note.object != _settings) return;
    _windowUp = NO;
    /* Deferred by one turn of the run loop: dropping to Accessory while the
     * window is mid-close leaves the menu bar in a half-swapped state. */
    dispatch_async(dispatch_get_main_queue(), ^{
        [self applyPresence];
    });
}

- (void)requestPermission:(id)sender
{
    og_mac_request_capture_access();
}

/* ---- monitor rows ---------------------------------------------- */

- (void)rebuildMonitorRows
{
    int i, n;

    for (NSView *v in [_monitorStack.views copy]) [_monitorStack removeView:v];
    [_rows removeAllObjects];

    n = og_core_monitor_count(_core);
    for (i = 0; i < n; ++i) {
        og_core_status st;
        OGMonitorRow *r;
        NSStackView *textCol, *statusLine;
        NSView *spacer;

        if (!og_core_status_at(_core, i, &st)) continue;

        /* A hairline between screens, so a list of them reads as a list
         * rather than as one block of text. */
        if (i > 0) {
            NSBox *sep = og_separator();
            [_monitorStack addView:sep inGravity:NSStackViewGravityTop];
            [sep.widthAnchor constraintEqualToAnchor:_monitorStack.widthAnchor
                                            constant:-16].active = YES;
        }

        r = [OGMonitorRow new];
        r.ident = [NSString stringWithUTF8String:st.mon->id];

        /* The app's own mark, at list size. Tinted by state, so a glance down
         * the column says which screens are doing what. */
        r.glyph = [NSImageView imageViewWithImage:og_screen_image(22.0)];
        [r.glyph.widthAnchor constraintEqualToConstant:22.0].active = YES;

        r.name = [NSTextField labelWithString:og_short_name(st.mon)];
        r.name.font = [NSFont systemFontOfSize:[NSFont systemFontSize]
                                        weight:NSFontWeightMedium];

        r.detail = [NSTextField labelWithString:
            [NSString stringWithFormat:@"%d × %d%@",
             st.mon->bounds.w, st.mon->bounds.h,
             st.mon->primary ? @" · Main" : @""]];
        r.detail.font = [NSFont systemFontOfSize:[NSFont smallSystemFontSize]];
        r.detail.textColor = [NSColor tertiaryLabelColor];

        r.dot = [NSTextField labelWithString:@"●"];
        r.dot.font = [NSFont systemFontOfSize:8.0];

        r.status = [NSTextField labelWithString:@""];
        r.status.font = [NSFont systemFontOfSize:[NSFont smallSystemFontSize]];
        r.status.textColor = [NSColor secondaryLabelColor];

        statusLine = [NSStackView stackViewWithViews:@[ r.dot, r.status ]];
        statusLine.orientation = NSUserInterfaceLayoutOrientationHorizontal;
        statusLine.alignment = NSLayoutAttributeCenterY;
        statusLine.spacing = 5;

        textCol = [NSStackView stackViewWithViews:@[ r.name, r.detail, statusLine ]];
        textCol.orientation = NSUserInterfaceLayoutOrientationVertical;
        textCol.alignment = NSLayoutAttributeLeading;
        textCol.spacing = 2;

        spacer = [[NSView alloc] init];
        [spacer setContentHuggingPriority:NSLayoutPriorityDefaultLow - 1
                           forOrientation:NSLayoutConstraintOrientationHorizontal];

        /* A switch, not a checkbox: this turns a thing on, and it matches the
         * control in the menu bar panel. */
        r.toggle = [[NSSwitch alloc] init];
        r.toggle.tag = i;
        r.toggle.target = self;
        r.toggle.action = @selector(monitorToggled:);
        _suppress = YES;
        r.toggle.state = st.enabled ? NSControlStateValueOn : NSControlStateValueOff;
        _suppress = NO;

        r.view = [NSStackView stackViewWithViews:@[
            r.glyph, textCol, spacer, r.toggle ]];
        r.view.orientation = NSUserInterfaceLayoutOrientationHorizontal;
        r.view.alignment = NSLayoutAttributeCenterY;
        r.view.spacing = 10;

        [_monitorStack addView:r.view inGravity:NSStackViewGravityTop];
        [r.view.widthAnchor constraintEqualToAnchor:_monitorStack.widthAnchor
                                           constant:-16].active = YES;
        [_rows addObject:r];
    }
    [self fitMonitorArea];
}

/* The colour of a screen's status dot. Same information as the text beside
 * it, readable without reading. */
- (NSColor *)dotColourFor:(const og_core_status *)st
{
    if (st->hold & OG_HOLD_DISABLED)    return [NSColor tertiaryLabelColor];
    if (st->state == OG_ST_BLANK)       return [NSColor systemPurpleColor];
    if (st->hold & OG_HOLD_MOTION)      return [NSColor systemBlueColor];
    if (st->hold & OG_HOLD_SYS_INHIBIT) return [NSColor systemTealColor];
    if (st->hold & OG_HOLD_NO_CAPTURE)  return [NSColor systemOrangeColor];
    return [NSColor systemGreenColor];
}

/* Grow the list area to the rows it actually holds, up to a ceiling past
 * which it scrolls, then shrink the window around the result. */
- (void)fitMonitorArea
{
    CGFloat h;

    if (!_monitorHeight || !_root) return;
    [_monitorStack layoutSubtreeIfNeeded];
    h = _monitorStack.fittingSize.height;
    if (h < 56)  h = 56;
    if (h > 320) h = 320;
    _monitorHeight.constant = h;

    [_root layoutSubtreeIfNeeded];
    {
        NSSize fit = _root.fittingSize;
        /* Cards carry their own padding, so the window needs more room than
         * the bare-stack layout did before it. */
        if (fit.width < 600) fit.width = 600;
        [_settings setContentSize:fit];
    }
}

- (BOOL)rowsMatchCore
{
    int i, n = og_core_monitor_count(_core);
    if ((NSInteger)n != (NSInteger)_rows.count) return NO;
    for (i = 0; i < n; ++i) {
        og_core_status st;
        if (!og_core_status_at(_core, i, &st)) return NO;
        if (![_rows[i].ident isEqualToString:
              [NSString stringWithUTF8String:st.mon->id]]) return NO;
    }
    return YES;
}

- (NSString *)describe:(const og_core_status *)st
{
    if (st->hold & OG_HOLD_DISABLED) return @"not guarded";
    if (st->state == OG_ST_BLANK)
        return @"blanked — move the pointer here to wake it";
    if (st->hold & OG_HOLD_MOTION)
        return [NSString stringWithFormat:
                @"awake — playback detected (%.0f%% of the screen changing)",
                st->motion_ratio * 100.0];
    if (st->hold & OG_HOLD_SYS_INHIBIT) {
        char who[128];
        if (og_plat_inhibitor_name(who, sizeof who))
            return [NSString stringWithFormat:
                    @"awake — %s is holding the display on", who];
        return @"awake — an application is holding the display on";
    }
    if (st->hold & OG_HOLD_INHIBIT_SEEN) {
        char who[128];
        NSString *holder = og_plat_inhibitor_name(who, sizeof who)
            ? [NSString stringWithUTF8String:who] : @"an app";
        return [NSString stringWithFormat:
                @"awake — idle %lds of %ds (%@ asks to keep the display on; "
                @"ignored while the screen is being sampled)",
                st->idle_ms / 1000, _cfg.timeout_sec, holder];
    }
    if (st->hold & OG_HOLD_NO_CAPTURE)
        return [NSString stringWithFormat:
                @"awake — idle %lds (no screen sampling; assertions only)",
                st->idle_ms / 1000];
    return [NSString stringWithFormat:
            @"awake — idle %lds of %ds (last change %.1f%%)",
            st->idle_ms / 1000, _cfg.timeout_sec, st->motion_ratio * 100.0];
}

- (void)refreshUI
{
    int i, blanked = 0, n;

    _pauseItem.title = og_core_paused(_core) ? @"Resume" : @"Pause";
    _pauseButton.title = og_core_paused(_core) ? @"Resume" : @"Pause";
    if (_popover.isShown) [self refreshPanel];

    if (!_settings || !_settings.isVisible) return;
    if (![self rowsMatchCore]) [self rebuildMonitorRows];

    n = (int)_rows.count;
    for (i = 0; i < n; ++i) {
        og_core_status st;
        if (!og_core_status_at(_core, i, &st)) continue;
        _rows[i].status.stringValue = [self describe:&st];
        _rows[i].dot.textColor = [self dotColourFor:&st];
        _rows[i].glyph.contentTintColor = st.enabled
            ? [self dotColourFor:&st] : [NSColor tertiaryLabelColor];
        _suppress = YES;
        _rows[i].toggle.state = st.enabled ? NSControlStateValueOn
                                           : NSControlStateValueOff;
        _suppress = NO;
        if (st.state == OG_ST_BLANK) ++blanked;
    }

    _permissionButton.hidden = og_plat_capture_available() ? YES : NO;
    _summary.stringValue = [NSString stringWithFormat:
        @"%s — %@ — %d of %d screen%s blanked",
        og_plat_name(),
        og_core_paused(_core) ? @"paused" : @"guarding",
        blanked, n, n == 1 ? "" : "s"];
}

/* ---- settings actions ------------------------------------------ */

- (void)monitorToggled:(NSSwitch *)sw
{
    if (_suppress) return;
    og_core_set_enabled(_core, (int)sw.tag, sw.state == NSControlStateValueOn);
    [self saveConfig];
}

- (void)timeoutChanged:(NSPopUpButton *)p
{
    _cfg.timeout_sec = (int)p.selectedItem.tag;
    [self saveConfig];
}

- (void)wakeChanged:(NSPopUpButton *)p
{
    _cfg.wake_mode = (og_wake_mode)p.indexOfSelectedItem;
    [self saveConfig];
}

- (void)inhibitChanged:(NSPopUpButton *)p
{
    if (_suppress) return;
    _cfg.inhibitor_policy = (og_inhibitor_policy)p.indexOfSelectedItem;
    [self saveConfig];
}

- (void)opacityChanged:(NSSlider *)s
{
    _cfg.curtain_opacity = (int)s.doubleValue;
    [self saveConfig];
}

@end

/* ------------------------------------------------------------------ */

/* NSApplication.delegate is a weak property, so the delegate needs an owner
 * that outlives the autorelease pool or it is deallocated immediately after
 * assignment and the app launches with no delegate at all. */
static OGAppDelegate *gAppDelegate = nil;

int main(int argc, const char *argv[])
{
    @autoreleasepool {
        int i;
        for (i = 1; i < argc; ++i) {
            if (!strcmp(argv[i], "--verbose") || !strcmp(argv[i], "-v"))
                og_log_set_level(OG_LOG_DEBUG);
            else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
                char p[1024];
                og_config_path(p, sizeof p);
                printf("OLEDGuard - per-monitor OLED burn-in curtain\n\n"
                       "Runs in the menu bar. Configuration file:\n  %s\n", p);
                return 0;
            }
        }

        NSApplication *app = [NSApplication sharedApplication];
        /* Accessory: status bar only, no Dock icon, no app menu. */
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory];
        gAppDelegate = [[OGAppDelegate alloc] init];
        app.delegate = gAppDelegate;
        [app run];
    }
    return 0;
}

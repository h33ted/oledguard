# OLEDGuard

An OLED pixel dims in proportion to how long it has been lit, so anything that
sits on screen unchanged for hours (a taskbar, a chat sidebar, an editor gutter,
a dashboard) eventually leaves a permanent mark. The defences the operating
system offers are blunt. Display sleep and screensavers act on every monitor at
once, they treat "no keyboard or mouse input" as "nothing happening", and so
they fire in the middle of a film and do nothing at all about the second screen
you left showing a static window.

OLEDGuard takes a narrower approach. You nominate the screens that need
protecting; each one gets its own idle timer and is covered with a black curtain
when that timer expires. Moving the pointer onto a covered screen lifts it at
once. A screen with a film, a game or a video call running on it is left alone,
and a screen with a static window on it is not.

![The OLEDGuard settings window on macOS, with one of two screens guarded and
currently blanked](docs/settings-window.png)

## Project status

macOS and Windows are both built, tested and released. The Windows backend has
been exercised on x64 hardware; the ARM64 build compiles and is published, but
has not been run on an ARM64 machine, so treat that slice as unverified.

Linux (X11, XRandR, XRender, GTK4) is present in the tree and compiles, but has
not been exercised on real hardware, so it is unreleased rather than supported.
It is intended to follow.

The portable core and its test suite build and pass on all three.

## What it does

Each monitor has its own timer and its own state, so the OLED can be guarded
while the IPS panel beside it is left alone. Moving the pointer onto a covered
screen lifts that curtain and no other, with a worst case latency of one 40 ms
tick.

The curtain is black rather than dim, because on OLED `#000000` is the only
colour that actually switches the pixels off. Opacity is adjustable if you would
rather still make out notifications underneath, at some cost in protection. It
is a curtain and not a lock screen: clicks and keystrokes pass straight through
to whatever is beneath it.

The interesting part is that a film, a game or a video call keeps its screen
awake, while Discord sitting idle with a blinking caret, or a web page nobody is
scrolling, does not.

## How playback is distinguished from a static window

Two independent signals, either of which holds a screen awake.

**Operating system idle assertions.** Applications that play media ask the
system to keep the display on, and stop asking when you press pause. Reading
that costs nothing and is exactly right whenever it answers at all.

| | queried through |
|---|---|
| macOS | `IOPMCopyAssertionsStatus`, `PreventUserIdleDisplaySleep` and related |
| Linux | `systemd-logind ListInhibitors`, `org.gnome.SessionManager.IsInhibited(8)`, `org.freedesktop.PowerManagement.Inhibit.HasInhibit` |
| Windows | `SHQueryUserNotificationState`, which covers exclusive fullscreen D3D and presentation mode |

Assertions cannot be trusted absolutely, because Amphetamine, Caffeine,
Endurance and any active screen sharing session hold one permanently. Obeying
those unconditionally would mean nothing ever blanked. The default policy is
therefore to obey an assertion only when pixel sampling is unavailable, and this
is configurable in the settings window.

**Pixel sampling.** The screen is scaled down to a 32 by 18 grid of luma values,
576 bytes, roughly every 1.5 seconds, and consecutive grids are compared. The
measure is the fraction of cells that changed by more than a small delta, which
is what separates the cases:

| what is on screen | fraction of cells changing |
|---|---|
| fullscreen film or game | 0.30 to 0.80 |
| windowed video | 0.10 to 0.40 |
| scrolling a page | a brief spike, then nothing |
| Discord idle, blinking caret | 0.00 to 0.01 |
| a static web page | 0.00 |
| paused video | 0.00 |

The threshold is 6% for a windowed application and 1.5% when a fullscreen window
covers the monitor, on the reasoning that a fullscreen window plus any motion at
all is almost certainly media. A hysteresis latch sits on top: two consecutive
moving samples turn playback detection on, five consecutive still samples turn
it off. The asymmetry is deliberate, since a dark and quiet shot in a film must
not arm the blanker, while a genuine pause should release it within a few
seconds.

The downscale is done by the GPU or the display server rather than in the
process: `SCScreenshotManager` into a small context on macOS, an XRender
transform on Linux, `StretchBlt` with `HALFTONE` on Windows. Only 576 pixels
ever cross into the application.

The two signals are complementary. Assertions catch exclusive fullscreen games
that no capture API can see, and pixel sampling catches the players and
emulators that never learned to raise an assertion.

## Building on macOS

Requires CMake and the Xcode command line tools. The deployment target is 12.3;
pixel sampling needs macOS 14 or later, since it uses `SCScreenshotManager`, and
below that the application still runs with sampling reported as unavailable.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
```

The result is `build/oledguard.app`.

Full Xcode is optional. If `actool` is available, the layered Icon Composer
artwork in `packaging/macos/AppIcon.icon` is compiled into the bundle, which is
what macOS 26 uses for its dark and tinted icon variants. Without it the build
falls back to `packaging/macos/OLEDGuard.icns` and is otherwise identical.

### Screen recording and code signing

Pixel sampling needs the Screen Recording permission, granted once in System
Settings under Privacy and Security. Denying it costs the pixel signal only, not
the application.

macOS ties every privacy grant to the application's code signature. An ad hoc
signature is derived from the binary itself, so it changes on every build and
the system treats each rebuild as a new application that has been granted
nothing. If you are going to rebuild repeatedly, create a self signed code
signing certificate once, in Keychain Access under Certificate Assistant,
Create a Certificate, with identity type Self Signed Root and certificate type
Code Signing, then sign each build with it:

```sh
codesign --force --deep --sign "OLEDGuard Dev" build/oledguard.app
```

The grant then survives rebuilds.

## Building on Windows

Requires CMake and the Visual Studio 2022 build tools with the Desktop
development with C++ workload. The Visual Studio generator finds the compiler
by itself, so no Developer Command Prompt is needed:

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release
```

Substitute `-A ARM64` for an ARM64 build. The result is
`build\Release\oledguard.exe`.

The build compiles `packaging/windows/oledguard.rc`, which carries the icon,
the version block and the application manifest. The manifest is not cosmetic:
without it the process gets ComCtl32 v5 controls and no per-monitor DPI
awareness, so the window is drawn at the wrong scale on anything but a 96 dpi
display. Regenerate the icon with `packaging/windows/make_ico.py` after
changing the artwork.

MinGW also works, including as a cross-compile from Linux, using the included
toolchain file:

```sh
cmake -S . -B build-win \
      -DCMAKE_TOOLCHAIN_FILE=packaging/windows/mingw-toolchain.cmake
cmake --build build-win -j
```

## Building on Linux

Untested; included so the portable core can be checked against a third
platform. Needs `libx11 libxrandr libxrender libxext libxss gtk4` and their
development packages:

```sh
sudo apt install cmake build-essential libx11-dev libxrandr-dev \
     libxrender-dev libxext-dev libxss-dev libgtk-4-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
```

## Running

On macOS the application lives in the menu bar by default. Clicking the icon
opens a panel listing every guarded screen with its live status and a switch,
along with the curtain opacity and the common actions. The settings window has
the rest: which screens to guard, the idle timeout, what counts as a wake event,
how to treat applications asking to keep the display on, whether to appear in
the Dock as well as the menu bar, and whether to open at login. Closing the
window leaves the application running.

On Windows it lives in the notification area and offers the same settings.
Only one instance runs at a time; launching the executable again re-presents
the existing window rather than starting a second copy.

Settings are written to a plain INI file that can also be edited by hand:

```
macOS    ~/Library/Application Support/OLEDGuard/oledguard.ini
Linux    $XDG_CONFIG_HOME/oledguard/oledguard.ini
Windows  %APPDATA%\OLEDGuard\oledguard.ini
```

Monitors are keyed by EDID (vendor, product, serial) rather than by connector,
so unplugging a screen and putting it back in a different port keeps its
setting. The detection thresholds are exposed in the file for tuning if a
particular application fools the classifier.

Running the binary with `--verbose` logs every sample and state change to
stderr, including the measured change ratio, which is the quickest way to find
out why a screen did or did not blank.

## Tests

```sh
./build/og_test_motion      # the classifier: ratios, hysteresis, edge cases
./build/og_test_core        # 21 end to end scenarios against a mock backend
```

Both run headless on any platform, with no monitor and no display server.
`og_test_core` drives the real state machine through a mock backend with
simulated time, so a scenario such as "fullscreen playback for four timeouts,
then pause" runs in milliseconds and deterministically.

## Known limitations

None of these are hidden at runtime. The settings window names the signal that
is holding a screen awake, and says so when pixel sampling is unavailable.

**A covered screen cannot be sampled**, since the curtain is what the sampler
would measure. If playback starts on an already covered screen, only the
assertion path can notice it, which covers browsers and media players but not an
application that asserts nothing.

**Wayland** gives an ordinary client neither screen capture nor dependable
always on top windows. Under XWayland the backend reports sampling as
unavailable and falls back to idle inhibitors alone, which still covers browsers
and most media players. Native support needs `ext-idle-notify-v1`,
`wlr-layer-shell` and a PipeWire ScreenCast portal grant, which is separate work
and not portable across compositors today.

**Windows exclusive fullscreen D3D** is invisible to GDI capture and is handled
by `SHQueryUserNotificationState` instead. Windows also exposes no supported way
to enumerate another process's power requests, since `powercfg /requests` uses a
private interface, so assertion coverage there is narrower than elsewhere.

**Login items are registered at the bundle's current path.** Moving
`OLEDGuard.app` after enabling "Open at login" leaves a stale entry until it is
launched from its new location.

## Design notes

```
include/oledguard/       the core's public headers
src/core/                platform free engine: config, state machine, classifier
src/platform/macos/      ScreenCaptureKit, IOKit assertions, Cocoa interface
src/platform/linux/      X11, XRandR, XRender, XScreenSaver, GTK4 interface
src/platform/win32/      GDI capture, layered curtains, tray icon
tests/                   unit tests and a mock backend
```

The core never includes a platform header. `og_platform.h` declares sixteen
functions; each backend implements exactly those, and so do the tests. That is
what makes it possible to test the interesting behaviour, such as whether a
paused film releases the hold after five samples, without a monitor, a display
server or a graphical session, and it is why the classifier lives in its own
file with no dependency on anything else.

Timing is monotonic throughout. A screen blanker driven by wall clock time
misbehaves every time an NTP correction or a daylight saving change lands.

## Licence

GNU General Public License, version 3 or later. The full text is in
[LICENSE](LICENSE), and every source file carries an
`SPDX-License-Identifier: GPL-3.0-or-later` line.

In short: use it for anything, modify it, redistribute it. If you distribute a
modified version, or a program that incorporates this one, you must release
that under the GPL as well and make the source available. It cannot be turned
into a closed-source product.

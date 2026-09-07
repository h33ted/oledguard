cmake_minimum_required(VERSION 3.16)
project(oledguard VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(OG_BUILD_TESTS "Build the headless test suite" ON)

set(OG_CORE_SRC
  src/core/og_core.c
  src/core/og_config.c
  src/core/og_motion.c
  src/core/og_log.c)

add_library(og_core STATIC ${OG_CORE_SRC})
target_include_directories(og_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(og_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# ---------------------------------------------------------------------------
# Platform backend
# ---------------------------------------------------------------------------

if(WIN32)

  # The icon, the version block and the manifest. The manifest is not optional
  # polish: without it the process gets ComCtl32 v5 controls and no per-monitor
  # DPI awareness. Regenerate the icon with packaging/windows/make_ico.py.
  enable_language(RC)

  add_executable(oledguard WIN32
    src/platform/win32/og_plat_win32.c
    src/platform/win32/og_ui_win32.c
    packaging/windows/oledguard.rc)
  target_link_libraries(oledguard PRIVATE og_core
    user32 gdi32 shell32 comctl32 advapi32 ole32 shlwapi uxtheme dwmapi uuid)
  target_compile_definitions(oledguard PRIVATE
    UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
  if(MINGW)
    # MinGW's startup code only looks for wWinMain when -municode is on.
    target_compile_options(oledguard PRIVATE -municode)
    target_link_options(oledguard PRIVATE -municode -mwindows)
  endif()
  if(MSVC)
    # The linker embeds a manifest of its own unless told not to, and two
    # RT_MANIFEST resources with id 1 is a hard error (CVT1100). Ours, in
    # oledguard.rc, is the one to keep: MinGW has no equivalent switch.
    target_link_options(oledguard PRIVATE /MANIFEST:NO)
  endif()

elseif(APPLE)

  enable_language(OBJC)
  set(CMAKE_OBJC_STANDARD 99)
  # ScreenCaptureKit arrived in 12.3; SCScreenshotManager, which the sampler
  # uses, in 14.0. Below 14 the app still runs, with sampling disabled.
  if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.3" CACHE STRING "" FORCE)
  endif()
  # Regenerate with: python3 packaging/macos/make_icns.py
  set(OG_ICNS ${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/OLEDGuard.icns)
  set_source_files_properties(${OG_ICNS} PROPERTIES
    MACOSX_PACKAGE_LOCATION Resources)

  add_executable(oledguard MACOSX_BUNDLE
    src/platform/macos/og_plat_mac.m
    src/platform/macos/og_ui_mac.m
    ${OG_ICNS})
  # ARC: the backend uses __bridge_retained/__bridge_transfer to hand NSWindow
  # ownership across the C boundary, which requires it.
  target_compile_options(oledguard PRIVATE -fobjc-arc)
  target_link_libraries(oledguard PRIVATE og_core
    "-framework Cocoa"
    "-framework CoreGraphics"
    "-framework ApplicationServices"
    "-framework ScreenCaptureKit"
    "-framework ServiceManagement"
    "-framework CoreMedia"
    "-framework IOKit")
  set_target_properties(oledguard PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "OLEDGuard"
    MACOSX_BUNDLE_ICON_FILE "OLEDGuard.icns"
    MACOSX_BUNDLE_GUI_IDENTIFIER "io.oledguard.OLEDGuard"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/Info.plist.in")

  # Layered Liquid Glass artwork for macOS 26, from Icon Composer. Needs full
  # Xcode's actool; with Command Line Tools only the script is a no-op and the
  # .icns above serves every version. The .icns is kept either way as the
  # pre-26 fallback, so this is an addition rather than a replacement.
  add_custom_command(TARGET oledguard POST_BUILD
    COMMAND sh
      "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/embed_appicon.sh"
      "$<TARGET_BUNDLE_DIR:oledguard>"
      "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/AppIcon.icon"
      "${CMAKE_OSX_DEPLOYMENT_TARGET}"
    VERBATIM)

else()

  find_package(PkgConfig REQUIRED)
  pkg_check_modules(X11 REQUIRED IMPORTED_TARGET
    x11 xrandr xrender xext xscrnsaver)
  pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4 gio-2.0)

  add_executable(oledguard
    src/platform/linux/og_plat_x11.c
    src/platform/linux/og_inhibit_dbus.c
    src/platform/linux/og_ui_gtk.c)
  target_link_libraries(oledguard PRIVATE og_core PkgConfig::X11 PkgConfig::GTK4)
  target_include_directories(oledguard PRIVATE src/platform/linux)

endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(oledguard PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

install(TARGETS oledguard
  RUNTIME DESTINATION bin
  BUNDLE  DESTINATION .)

# ---------------------------------------------------------------------------
# Tests: the classifier and the state machine, both driven by a mock backend
# so they run headless in CI on any platform.
# ---------------------------------------------------------------------------

if(OG_BUILD_TESTS)
  enable_testing()

  add_executable(og_test_motion tests/test_motion.c src/core/og_motion.c)
  target_include_directories(og_test_motion PRIVATE include)
  add_test(NAME motion COMMAND og_test_motion)

  add_executable(og_test_core
    tests/test_core.c
    tests/mock_platform.c
    ${OG_CORE_SRC})
  target_include_directories(og_test_core PRIVATE include)
  add_test(NAME core COMMAND og_test_core)

  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(og_test_motion PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_compile_options(og_test_core   PRIVATE -Wall -Wextra -Wno-unused-parameter)
  endif()
endif()

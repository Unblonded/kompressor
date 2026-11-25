cmake_minimum_required(VERSION 3.21)

# Some fetched dependencies declare very old minimum versions; keep CMake 4.x happy.
if(NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
endif()

# macOS: dragging files out and the file pasteboard are Objective-C++ (AppKit). The language has to be
# known from the start, not enabled later, or CMake's generator misses its rules.
set(KZ_LANGUAGES C CXX)
if(CMAKE_HOST_APPLE)
  list(APPEND KZ_LANGUAGES OBJC OBJCXX)  # OBJC too, or GLFW's .m files are built as C++
endif()
project(Kompressor VERSION 2.1.0 LANGUAGES ${KZ_LANGUAGES})

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(KOMPRESSOR_BUILD_GUI "Build the Dear ImGui desktop app" ON)
option(KOMPRESSOR_BUILD_CLI "Build the command line tool" ON)
option(KOMPRESSOR_GLFW_WAYLAND "Build GLFW with native Wayland support (Linux)" OFF)

include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# ---------------------------------------------------------------------------
# Archive / compression libraries.
#   * Windows / macOS-universal: provided by vcpkg (see vcpkg.json + CMakePresets.json)
#   * macOS (Homebrew) / Linux (apt, dnf, pacman): provided by the system
# ---------------------------------------------------------------------------
if(APPLE AND NOT VCPKG_TOOLCHAIN)
  # Homebrew installs libarchive keg-only; make it discoverable automatically.
  find_program(BREW_EXE brew)
  if(BREW_EXE)
    foreach(pkg libarchive xz zstd)
      execute_process(COMMAND ${BREW_EXE} --prefix ${pkg}
                      OUTPUT_VARIABLE KZ_BREW_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE
                      ERROR_QUIET)
      if(KZ_BREW_PREFIX AND EXISTS "${KZ_BREW_PREFIX}")
        list(PREPEND CMAKE_PREFIX_PATH "${KZ_BREW_PREFIX}")
      endif()
    endforeach()
    # Prefer Homebrew's liblzma (has the ARM64 BCJ filter) over the SDK stub.
    set(CMAKE_FIND_FRAMEWORK LAST)
  endif()
endif()

find_package(LibArchive REQUIRED)
find_package(LibLZMA REQUIRED)
find_package(ZLIB REQUIRED)
find_package(BZip2)

find_package(zstd CONFIG QUIET)
if(TARGET zstd::libzstd)
  set(KZ_ZSTD_TARGET zstd::libzstd)
elseif(TARGET zstd::libzstd_static)
  set(KZ_ZSTD_TARGET zstd::libzstd_static)
elseif(TARGET zstd::libzstd_shared)
  set(KZ_ZSTD_TARGET zstd::libzstd_shared)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(KZ_ZSTD REQUIRED IMPORTED_TARGET libzstd)
  set(KZ_ZSTD_TARGET PkgConfig::KZ_ZSTD)
endif()

# xxHash is header-only (XXH_INLINE_ALL), fetched so every platform gets the same version.
FetchContent_Declare(xxhash
  URL https://github.com/Cyan4973/xxHash/archive/refs/tags/v0.8.3.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(xxhash)

# ---------------------------------------------------------------------------
# Core library: everything that is not UI.
# ---------------------------------------------------------------------------
add_library(kompressor_core STATIC
  src/core/kz_common.cpp
  src/core/kz_formats.cpp
  src/core/kz_archive.cpp
  src/core/kz_hash.cpp
  src/core/kz_search.cpp
  src/core/kz_ramdrive.cpp
  src/core/kz_sfx.cpp
  src/core/kz_crypto.cpp
  src/core/kz_7z.cpp
  src/core/kz_writer.cpp
  src/core/kmp.cpp
  src/core/kz_parallel.cpp
  src/core/kz_cm.cpp
  src/core/kz_advisor.cpp
  src/core/kz_rar.cpp
  src/core/native/nat_core.cpp
  src/core/native/nat_exec.cpp
  src/core/native/nat_cfb.cpp
  src/core/native/nat_disk.cpp
  src/core/native/nat_codecs.cpp
  src/core/native/nat_fat.cpp
  src/core/native/nat_ntfs.cpp
  src/core/native/nat_ext.cpp
  src/core/native/nat_squash.cpp
  src/core/native/nat_lzx.cpp
  src/core/native/nat_chm.cpp
  src/core/native/nat_wim.cpp
  src/core/native/nat_nsis.cpp
  src/core/native/nat_small.cpp
  src/core/native/nat_xar.cpp
  src/core/native/nat_inno.cpp
  src/core/native/nat_arj.cpp
  src/core/native/nat_udf.cpp
  src/core/native/nat_lzfse.cpp
  src/core/native/nat_hfs.cpp
  src/core/native/nat_dmg.cpp
  src/core/native/nat_apfs.cpp
  src/core/native/nat_lvm.cpp
  src/core/native/nat_uefi.cpp
  src/core/native/nat_pending.cpp
  src/core/kz_perf.cpp
  src/core/kz_zip.cpp
  src/core/kz_gpu.cpp
)
target_include_directories(kompressor_core PUBLIC src/core PRIVATE ${xxhash_SOURCE_DIR})
target_link_libraries(kompressor_core PUBLIC LibArchive::LibArchive LibLZMA::LibLZMA ZLIB::ZLIB ${KZ_ZSTD_TARGET})
if(BZip2_FOUND)
  # Parallel .bz2 output; without it libarchive's single-threaded bzip2 filter is used.
  target_link_libraries(kompressor_core PUBLIC BZip2::BZip2)
  target_compile_definitions(kompressor_core PRIVATE KZ_HAVE_BZIP2)
endif()
target_compile_definitions(kompressor_core PUBLIC KZ_VERSION="${PROJECT_VERSION}")
if(WIN32)
  target_compile_definitions(kompressor_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
  target_link_libraries(kompressor_core PUBLIC shell32 powrprof ole32 advapi32 virtdisk)
endif()
if(MSVC)
  target_compile_options(kompressor_core PUBLIC /utf-8 /W3)
else()
  target_compile_options(kompressor_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
find_package(Threads REQUIRED)
target_link_libraries(kompressor_core PUBLIC Threads::Threads)
if(APPLE)
  target_link_libraries(kompressor_core PUBLIC "-framework CoreFoundation")
endif()

# ---------------------------------------------------------------------------
# Self-extractor (Windows): the small program at the front of a self-extracting .exe, drawn with the
# same ImGui look as the app. It is built with the GUI (below) and embedded in Kompressor.exe and
# kompressor-cli.exe as a resource, which copy it out when they make one.
# ---------------------------------------------------------------------------
if(WIN32 AND KOMPRESSOR_BUILD_GUI)
  set(KZ_SFX_EXE "${CMAKE_BINARY_DIR}/kz-sfx.exe")
  set(KZ_SFX_RC "${CMAKE_BINARY_DIR}/kz_sfx_embed.rc")
  file(WRITE "${KZ_SFX_RC}" "KZSFX RCDATA \"${KZ_SFX_EXE}\"\n")
  set_source_files_properties("${KZ_SFX_RC}" PROPERTIES OBJECT_DEPENDS "${KZ_SFX_EXE}")
  target_compile_definitions(kompressor_core PRIVATE KZ_HAVE_SFX)
endif()

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
if(KOMPRESSOR_BUILD_CLI)
  if(WIN32)
    add_executable(kompressor-cli src/cli/main.cpp packaging/windows/kompressor-cli.rc ${KZ_SFX_RC})
    if(KZ_SFX_RC)
      add_dependencies(kompressor-cli kz-sfx)
    endif()
  else()
    add_executable(kompressor-cli src/cli/main.cpp)
  endif()
  target_link_libraries(kompressor-cli PRIVATE kompressor_core)
endif()

# ---------------------------------------------------------------------------
# GUI (GLFW + OpenGL3 + Dear ImGui)
# ---------------------------------------------------------------------------
if(KOMPRESSOR_BUILD_GUI)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_WAYLAND ${KOMPRESSOR_GLFW_WAYLAND} CACHE BOOL "" FORCE)
  FetchContent_Declare(glfw
    URL https://github.com/glfw/glfw/archive/refs/tags/3.4.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  # The docking branch release: adds multi-viewports (dialogs can be dragged out into their own OS windows).
  FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b-docking.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE)  # fresh timestamps, so switching versions rebuilds the library
  # Native OS file dialogs: Windows IFileDialog, macOS NSOpenPanel, Linux xdg-desktop-portal (D-Bus).
  set(NFD_PORTAL ON CACHE BOOL "" FORCE)
  set(NFD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(NFD_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(nfd
    URL https://github.com/btzy/nativefiledialog-extended/archive/refs/tags/v1.2.1.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(glfw imgui nfd)

  find_package(OpenGL REQUIRED)

  add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
  target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
  target_link_libraries(imgui PUBLIC glfw OpenGL::GL)
  if(APPLE)
    target_compile_definitions(imgui PUBLIC GL_SILENCE_DEPRECATION)
  endif()

  if(WIN32)
    add_executable(kz-sfx WIN32 src/sfx/sfx_main.cpp src/gui/theme.cpp src/gui/chrome.cpp src/gui/app_icon.cpp
                   src/gui/window_frame.cpp packaging/windows/kz-sfx.rc)
    target_include_directories(kz-sfx PRIVATE src/gui)
    target_compile_definitions(kz-sfx PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(kz-sfx PRIVATE imgui LibArchive::LibArchive LibLZMA::LibLZMA ZLIB::ZLIB ${KZ_ZSTD_TARGET}
                          dwmapi shell32 ole32 user32 gdi32)
    if(TARGET BZip2::BZip2)
      target_link_libraries(kz-sfx PRIVATE BZip2::BZip2)
    endif()
    if(MSVC)
      target_compile_options(kz-sfx PRIVATE /utf-8 /W3)
      target_link_options(kz-sfx PRIVATE /ENTRY:wWinMainCRTStartup /MANIFEST:NO)  # the .rc embeds the manifest
    endif()
    set_target_properties(kz-sfx PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    set_source_files_properties(packaging/windows/kz-sfx.rc PROPERTIES
      OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/packaging/icons/kompressor.ico;${CMAKE_SOURCE_DIR}/packaging/windows/kz-sfx.manifest")
  endif()

  set(KZ_GUI_SOURCES
    src/gui/main.cpp
    src/gui/app.cpp
    src/gui/app_draw.cpp
    src/gui/app_files.cpp
    src/gui/app_hash.cpp
    src/gui/app_search.cpp
    src/gui/app_ramdrive.cpp
    src/gui/app_icon.cpp
    src/gui/chrome.cpp
    src/gui/file_dialog.cpp
    src/gui/native_files.cpp
    src/gui/prefs.cpp
    src/gui/shell_integration.cpp
    src/gui/theme.cpp
    src/gui/window_frame.cpp)

  if(APPLE)
    # Dragging files out and the file pasteboard use AppKit (Objective-C++).
    list(APPEND KZ_GUI_SOURCES src/gui/native_files_mac.mm)
    set_source_files_properties(src/gui/native_files_mac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    add_executable(Kompressor MACOSX_BUNDLE ${KZ_GUI_SOURCES})
    target_link_libraries(Kompressor PRIVATE "-framework AppKit")
    set_target_properties(Kompressor PROPERTIES
      MACOSX_BUNDLE_GUI_IDENTIFIER "app.kompressor"
      MACOSX_BUNDLE_BUNDLE_NAME "Kompressor"
      MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
      MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
      MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/packaging/macos/Info.plist.in")
    set(KZ_ICNS "${CMAKE_SOURCE_DIR}/packaging/icons/kompressor.icns")
    target_sources(Kompressor PRIVATE ${KZ_ICNS})
    set_source_files_properties(${KZ_ICNS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  elseif(WIN32)
    add_executable(Kompressor WIN32 ${KZ_GUI_SOURCES} packaging/windows/kompressor.rc ${KZ_SFX_RC})
    add_dependencies(Kompressor kz-sfx)
    if(MSVC)
      # The .rc embeds our own manifest (DPI awareness, UTF-8 code page, long paths).
      target_link_options(Kompressor PRIVATE /ENTRY:mainCRTStartup /MANIFEST:NO)
    endif()
    target_link_libraries(Kompressor PRIVATE dwmapi winmm)  # custom title bar: shadow, dark/rounded frame
    # Explorer context-menu handler: puts "Komp" next to 7-Zip's entry (static verbs can only go top/bottom).
    add_library(KompShell SHARED src/shellext/komp_shell.cpp src/shellext/komp_shell.def)
    target_compile_definitions(KompShell PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
    target_link_libraries(KompShell PRIVATE shell32 shlwapi advapi32 ole32)
    add_dependencies(Kompressor KompShell)
  else()
    add_executable(Kompressor ${KZ_GUI_SOURCES})
  endif()
  target_link_libraries(Kompressor PRIVATE kompressor_core imgui nfd)
endif()

if(KOMPRESSOR_BUILD_CLI)
  install(TARGETS kompressor-cli RUNTIME DESTINATION bin)
endif()
if(KOMPRESSOR_BUILD_GUI)
  install(TARGETS Kompressor RUNTIME DESTINATION bin BUNDLE DESTINATION .)
  if(WIN32)
    install(TARGETS KompShell RUNTIME DESTINATION bin)
  endif()
endif()

if(WIN32)
  # Rebuild the resources when the icon or manifest they embed changes (the RC compiler's
  # dependency scan does not track these).
  set_source_files_properties(packaging/windows/kompressor.rc packaging/windows/kompressor-cli.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/packaging/icons/kompressor.ico;${CMAKE_SOURCE_DIR}/packaging/windows/kompressor.manifest")
endif()

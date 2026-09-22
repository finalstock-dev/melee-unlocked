# Native Melee port: recompiled guest code + host runtime.
cmake_minimum_required(VERSION 3.15)
add_executable(port_frame_queue_test tests/frame_queue_test.cpp)
target_include_directories(port_frame_queue_test PRIVATE runtime/gx)
target_compile_features(port_frame_queue_test PRIVATE cxx_std_17)
add_test(NAME port_frame_queue COMMAND port_frame_queue_test)
set_tests_properties(port_frame_queue PROPERTIES TIMEOUT 10)
add_executable(port_video_background_learning_test tests/video_background_learning_test.cpp)
target_include_directories(port_video_background_learning_test PRIVATE runtime/gx)
target_compile_features(port_video_background_learning_test PRIVATE cxx_std_17)
add_test(NAME port_video_background_learning COMMAND port_video_background_learning_test)
add_executable(port_memory_range_test tests/memory_range_test.cpp)
target_include_directories(port_memory_range_test PRIVATE runtime/host)
target_compile_features(port_memory_range_test PRIVATE cxx_std_17)
add_test(NAME port_memory_ranges COMMAND port_memory_range_test)
add_test(NAME port_dol_validation COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/dol_validation_test.py")
add_executable(port_texture_test tests/texture_snapshot_test.cpp runtime/gx/gx_texture.cpp)
target_include_directories(port_texture_test PRIVATE runtime/gx)
target_compile_features(port_texture_test PRIVATE cxx_std_17)
add_test(NAME port_texture_snapshots COMMAND port_texture_test)
add_executable(port_ax_ucode_test tests/ax_ucode_test.cpp runtime/hle/ax_ucode.cpp)
target_include_directories(port_ax_ucode_test PRIVATE runtime/hle)
target_compile_features(port_ax_ucode_test PRIVATE cxx_std_17)
add_test(NAME port_ax_ucode COMMAND port_ax_ucode_test)
add_executable(port_pad_report_test tests/pad_report_test.cpp runtime/host/playstation_pad.cpp runtime/host/hid_pad.cpp)
target_include_directories(port_pad_report_test PRIVATE runtime/host runtime/ppc)
target_compile_features(port_pad_report_test PRIVATE cxx_std_17)
target_compile_definitions(port_pad_report_test PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(port_pad_report_test PRIVATE hid user32)
add_test(NAME port_pad_reports COMMAND port_pad_report_test)
add_executable(port_controller_profiles_test tests/controller_profiles_test.cpp runtime/host/controller_profiles.cpp)
target_include_directories(port_controller_profiles_test PRIVATE runtime/host)
target_compile_features(port_controller_profiles_test PRIVATE cxx_std_17)
add_test(NAME port_controller_profiles COMMAND port_controller_profiles_test)
add_executable(port_vcdiff_test tests/vcdiff_test.cpp runtime/host/vcdiff.cpp)
target_include_directories(port_vcdiff_test PRIVATE runtime/host)
target_compile_features(port_vcdiff_test PRIVATE cxx_std_17)
add_test(NAME port_vcdiff COMMAND port_vcdiff_test)
add_executable(port_subframe_test tests/subframe_test.cpp runtime/gx/subframe.cpp runtime/gx/gx_texture.cpp runtime/gx/gx_shader.cpp runtime/gx/authored_pose.cpp)
target_include_directories(port_subframe_test PRIVATE runtime/gx "${PROJECT_SOURCE_DIR}/native")
target_link_libraries(port_subframe_test PRIVATE native_animation)
target_compile_features(port_subframe_test PRIVATE cxx_std_17)
target_compile_definitions(port_subframe_test PRIVATE NOMINMAX)
add_test(NAME port_subframe COMMAND port_subframe_test)
set(PORT_LOAD_TEST "${CMAKE_CURRENT_BINARY_DIR}/generated/load_test.cpp")
add_custom_command(OUTPUT "${PORT_LOAD_TEST}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/generate_load_test.py" "${PORT_LOAD_TEST}"
  DEPENDS tests/generate_load_test.py recomp/emit.py recomp/gekko.py)
add_executable(port_load_test "${PORT_LOAD_TEST}")
target_include_directories(port_load_test PRIVATE runtime/ppc)
target_compile_features(port_load_test PRIVATE cxx_std_17)
add_test(NAME port_signed_halfword_loads COMMAND port_load_test)

# The instruction set the translated game and the runtime are built for. AVX2 is the default and
# what every release so far has used; it needs Intel Haswell (2013) or AMD Ryzen. "AVX" reaches
# back to 2011, and "SSE2" is every x64 processor ever made. A player on a processor without AVX2
# cannot start the game at all, so this exists to build them something that runs.
# MELEE_CPU_BASELINE and MELEE_ARCH_FLAG are declared at the root CMakeLists.txt, before this
# subdirectory is added, so native_animation (built there) also gets the right /arch flag.
message(STATUS "melee: CPU baseline ${MELEE_CPU_BASELINE}")

set(PORT_GEN "${CMAKE_CURRENT_SOURCE_DIR}/generated")
if(NOT EXISTS "${PORT_GEN}/guest_sources.cmake")
  message(FATAL_ERROR "port: generated code missing; run python port/recomp/recomp.py")
endif()
include("${PORT_GEN}/guest_sources.cmake")

# Guest library: large generated translation units. A second checkout of the same generated code can
# link an existing guest.lib instead of compiling it again (several GB of objects).
set(MELEE_PREBUILT_GUEST "" CACHE FILEPATH "Link this guest.lib (built from identical port/generated) instead of compiling it")
if(MELEE_PREBUILT_GUEST)
  add_library(guest STATIC IMPORTED GLOBAL)
  set_target_properties(guest PROPERTIES IMPORTED_LOCATION "${MELEE_PREBUILT_GUEST}")
  target_include_directories(guest INTERFACE "${PORT_GEN}" "${CMAKE_CURRENT_SOURCE_DIR}/runtime/ppc")
  target_compile_features(guest INTERFACE cxx_std_17)
else()
add_library(guest STATIC ${GUEST_SOURCES} "${PORT_GEN}/function_names.cpp" "${PORT_GEN}/gecko_data.cpp")
target_include_directories(guest PUBLIC "${PORT_GEN}" runtime/ppc)
target_include_directories(guest PRIVATE runtime/hle)   # gecko_data.h: run-time optional code flags
target_compile_features(guest PUBLIC cxx_std_17)
target_compile_options(guest PRIVATE /bigobj /MP /EHsc ${MELEE_ARCH_FLAG} /fp:precise /wd4102 /wd4101 /wd4189 /W1)
target_compile_definitions(guest PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()

# Host runtime: CPU helpers, memory, HLE, host services.
file(GLOB RUNTIME_SOURCES runtime/ppc/*.cpp runtime/hle/*.cpp runtime/host/*.cpp runtime/gx/*.cpp)
option(MELEE_ENABLE_DLSS5 "Build experimental DLSS 5 neural rendering" OFF)
if(NOT MELEE_ENABLE_DLSS5)
  list(REMOVE_ITEM RUNTIME_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/runtime/gx/gx_dlss5.cpp")
endif()
enable_language(C)
file(GLOB ENET_SOURCES third_party/enet/*.c)
add_library(port_enet STATIC ${ENET_SOURCES})
target_include_directories(port_enet PUBLIC third_party/enet/include)
target_compile_definitions(port_enet PRIVATE _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
target_link_libraries(port_enet PUBLIC ws2_32 winmm)
# libusb, as Dolphin uses for the GameCube adapter. Talking to WinUSB directly meant an adapter on
# libusbK or libusb-win32 was invisible to us while Dolphin reported it detected, and players were
# being asked to swap drivers for hardware that already worked. libusb's Windows backend covers all
# three, so an adapter that works in Slippi works here.
add_library(port_libusb STATIC
  third_party/libusb/libusb/core.c
  third_party/libusb/libusb/descriptor.c
  third_party/libusb/libusb/hotplug.c
  third_party/libusb/libusb/io.c
  third_party/libusb/libusb/strerror.c
  third_party/libusb/libusb/sync.c
  third_party/libusb/libusb/os/events_windows.c
  third_party/libusb/libusb/os/threads_windows.c
  third_party/libusb/libusb/os/windows_common.c
  third_party/libusb/libusb/os/windows_usbdk.c
  third_party/libusb/libusb/os/windows_winusb.c)
target_include_directories(port_libusb PUBLIC third_party/libusb/libusb PRIVATE third_party/libusb/msvc)
target_compile_definitions(port_libusb PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(port_libusb PUBLIC advapi32 ole32 setupapi)
file(STRINGS "${PROJECT_SOURCE_DIR}/VERSION" MELEE_PORT_VERSION LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/VERSION")
# VERSIONINFO wants the version as four comma separated numbers, not the "0.6.2" string. An
# executable with no version resource at all has no company, product or description for Windows to
# show, which is part of what makes an unsigned binary look like a dropper to SmartScreen and
# Defender: players have had the launcher quarantined on download. This does not replace signing,
# but it is free and it is a prerequisite for it.
string(REPLACE "." ";" MELEE_VERSION_PARTS "${MELEE_PORT_VERSION}")
list(LENGTH MELEE_VERSION_PARTS MELEE_VERSION_PART_COUNT)
while(MELEE_VERSION_PART_COUNT LESS 4)
  list(APPEND MELEE_VERSION_PARTS "0")
  list(LENGTH MELEE_VERSION_PARTS MELEE_VERSION_PART_COUNT)
endwhile()
list(SUBLIST MELEE_VERSION_PARTS 0 4 MELEE_VERSION_PARTS)
list(JOIN MELEE_VERSION_PARTS "," MELEE_VERSION_COMMAS)
add_library(runtime STATIC ${RUNTIME_SOURCES} third_party/slippilib/SlippiGame.cpp)
target_compile_definitions(runtime PRIVATE MELEE_PORT_VERSION="${MELEE_PORT_VERSION}")
# The replay code list a playback build was translated against, so it can tell a player when a
# replay comes from a different Slippi version instead of quietly diverging. Set these to the
# --extra-gct-base and the list's size when generating port/generated_playback; left at zero, the
# playback build says it has none, which is the honest answer for a build made without them.
set(MELEE_PLAYBACK_GCT_BASE "0" CACHE STRING "Guest address of the replay code list this playback build was translated for")
set(MELEE_PLAYBACK_GCT_SIZE "0" CACHE STRING "Size in bytes of that replay code list")
target_compile_definitions(runtime PRIVATE
  MELEE_PLAYBACK_GCT_BASE=${MELEE_PLAYBACK_GCT_BASE}u MELEE_PLAYBACK_GCT_SIZE=${MELEE_PLAYBACK_GCT_SIZE}u)
target_include_directories(runtime PUBLIC runtime/ppc runtime/host runtime/hle runtime/gx "${PORT_GEN}" "${PROJECT_SOURCE_DIR}/native" third_party)
target_compile_features(runtime PUBLIC cxx_std_17)
target_compile_options(runtime PRIVATE /EHsc ${MELEE_ARCH_FLAG} /fp:precise /W3)
target_compile_definitions(runtime PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING)
target_link_libraries(runtime PUBLIC native_animation port_enet port_libusb bcrypt d3d12 d3d11 dxgi d3dcompiler xinput9_1_0 winmm user32 gdi32 setupapi winusb hid mfplat mfreadwrite mfuuid ole32 shell32)

# WIN32: no console window when the game is double-clicked or started from the launcher. Command-line
# use still works, because main() reattaches to a parent console when there is one (see app/main.cpp).
add_executable(melee_port WIN32 app/main.cpp app/settings_window.cpp app/melee_unlocked.rc)
add_executable(port_mm_probe tests/mm_probe.cpp)
target_link_libraries(port_mm_probe PRIVATE runtime guest)
target_compile_options(port_mm_probe PRIVATE /EHsc)
target_compile_definitions(port_mm_probe PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
add_executable(port_settings_load_test tests/settings_load_test.cpp)
target_link_libraries(port_settings_load_test PRIVATE runtime guest)
target_include_directories(port_settings_load_test PRIVATE runtime)
target_compile_options(port_settings_load_test PRIVATE /EHsc)
target_compile_definitions(port_settings_load_test PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
add_test(NAME port_settings_load COMMAND port_settings_load_test)
target_link_libraries(melee_port PRIVATE runtime guest dbghelp)   # dbghelp: crash minidump
target_compile_options(melee_port PRIVATE /EHsc)
target_compile_definitions(melee_port PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS MELEE_PORT_VERSION="${MELEE_PORT_VERSION}"
  MELEE_CPU_BASELINE_NAME="${MELEE_CPU_BASELINE}"
  MELEE_VERSION_COMMAS=${MELEE_VERSION_COMMAS})
if(MELEE_CPU_BASELINE STREQUAL "AVX2")
  target_compile_definitions(melee_port PRIVATE MELEE_NEEDS_AVX2)
endif()

# Melee Unlocked client: Win32 launcher (Play page, Build tab, self-update). No runtime dependency.
add_executable(melee_unlocked WIN32 app/launcher.cpp runtime/host/updater.cpp app/launcher.rc)
set_target_properties(melee_unlocked PROPERTIES OUTPUT_NAME MeleeUnlockedLauncher)
target_include_directories(melee_unlocked PRIVATE runtime/host runtime/ppc runtime/hle runtime/gx "${PORT_GEN}" third_party)
target_compile_features(melee_unlocked PRIVATE cxx_std_17)
target_compile_options(melee_unlocked PRIVATE /EHsc)
target_compile_definitions(melee_unlocked PRIVATE NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS MELEE_PORT_VERSION="${MELEE_PORT_VERSION}"
  MELEE_VERSION_COMMAS=${MELEE_VERSION_COMMAS})
target_link_libraries(melee_unlocked PRIVATE comctl32 winhttp user32 gdi32 shell32 comdlg32 msimg32 uxtheme)

# Replay playback build: the same runtime with the game translated against the Slippi Playback
# code set (python port/recomp/recomp.py --sys-dir port/slippi_sys_playback --out port/generated_playback).
set(PORT_GEN_PLAYBACK "${CMAKE_CURRENT_SOURCE_DIR}/generated_playback")
if(EXISTS "${PORT_GEN_PLAYBACK}/guest_sources.cmake")
  include("${PORT_GEN_PLAYBACK}/guest_sources.cmake")
  set(MELEE_PREBUILT_GUEST_PLAYBACK "" CACHE FILEPATH "Link this guest_playback.lib (built from identical port/generated_playback) instead of compiling it")
  if(MELEE_PREBUILT_GUEST_PLAYBACK)
    add_library(guest_playback STATIC IMPORTED GLOBAL)
    set_target_properties(guest_playback PROPERTIES IMPORTED_LOCATION "${MELEE_PREBUILT_GUEST_PLAYBACK}")
    target_include_directories(guest_playback BEFORE INTERFACE "${PORT_GEN_PLAYBACK}")
    target_include_directories(guest_playback INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/runtime/ppc")
    target_compile_features(guest_playback INTERFACE cxx_std_17)
  else()
  add_library(guest_playback STATIC ${GUEST_SOURCES} "${PORT_GEN_PLAYBACK}/function_names.cpp" "${PORT_GEN_PLAYBACK}/gecko_data.cpp")
  target_include_directories(guest_playback BEFORE PUBLIC "${PORT_GEN_PLAYBACK}")
  target_include_directories(guest_playback PUBLIC runtime/ppc)
  target_include_directories(guest_playback PRIVATE runtime/hle)
  target_compile_features(guest_playback PUBLIC cxx_std_17)
  target_compile_options(guest_playback PRIVATE /bigobj /MP /EHsc ${MELEE_ARCH_FLAG} /fp:precise /wd4102 /wd4101 /wd4189 /W1)
  target_compile_definitions(guest_playback PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
  endif()
  # The .rc carries the icon and the version resource. This target never had it, so the playback
  # build shipped with a blank Explorer icon and no company or product name at all, which is the
  # profile an unsigned executable is judged on.
  add_executable(melee_port_playback app/main.cpp app/settings_window.cpp app/melee_unlocked.rc)
  target_link_libraries(melee_port_playback PRIVATE runtime guest_playback dbghelp)
  target_compile_options(melee_port_playback PRIVATE /EHsc)
  # It ships in the release now, and the packager refuses an executable that does not report the
  # release's own version, so it needs the same definition the other builds get.
  target_compile_definitions(melee_port_playback PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS
    MELEE_PORT_VERSION="${MELEE_PORT_VERSION}"
    MELEE_VERSION_COMMAS=${MELEE_VERSION_COMMAS}
    MELEE_RC_DESCRIPTION="Melee Unlocked Replay Playback"
    MELEE_RC_INTERNAL_NAME="melee_port_playback"
    MELEE_RC_ORIGINAL_FILENAME="melee_port_playback.exe")
endif()
if(EXISTS "${PROJECT_SOURCE_DIR}/melee/orig/GALE01/sys/main.dol")
  add_test(NAME port_image_identity COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/image_identity_test.py" "$<TARGET_FILE:melee_port>"
    "${PROJECT_SOURCE_DIR}/melee/orig/GALE01/sys/main.dol")
endif()

add_executable(port_gpu_resource_test tests/gpu_resource_test.cpp runtime/gx/gx_d3d12.cpp runtime/gx/gx_shader.cpp runtime/gx/gx_texture.cpp runtime/gx/gx_streamline.cpp runtime/gx/gx_xess.cpp runtime/gx/flicker_scan.cpp runtime/gx/texture_pack.cpp)
target_include_directories(port_gpu_resource_test PRIVATE runtime/gx runtime/host runtime/ppc runtime/hle third_party)
target_compile_features(port_gpu_resource_test PRIVATE cxx_std_17)
target_compile_definitions(port_gpu_resource_test PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS GX_SRV_HEAP_SIZE=32 GX_SAMPLER_HEAP_SIZE=16 GX_CONSTANT_PAGE_SIZE=4096)
target_link_libraries(port_gpu_resource_test PRIVATE d3d12 dxgi d3dcompiler user32)
add_test(NAME port_gpu_resources COMMAND port_gpu_resource_test)
set_tests_properties(port_gpu_resources PROPERTIES TIMEOUT 60)

add_executable(port_render_observer_test tests/render_observer_test.cpp runtime/gx/render_observer.cpp runtime/gx/gx_texture.cpp runtime/gx/authored_pose.cpp runtime/gx/subframe.cpp runtime/gx/gx_shader.cpp)
target_include_directories(port_render_observer_test PRIVATE runtime/ppc runtime/gx "${PROJECT_SOURCE_DIR}/native")
target_compile_features(port_render_observer_test PRIVATE cxx_std_17)
target_compile_definitions(port_render_observer_test PRIVATE NOMINMAX)
target_link_libraries(port_render_observer_test PRIVATE native_animation)
add_test(NAME port_render_observer COMMAND port_render_observer_test)

# Cache identity follows shader generation, constant layout and pipeline setup.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  runtime/gx/gx_shader.cpp runtime/gx/gx_shader.h runtime/gx/gx_d3d12.cpp)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/runtime/gx/gx_shader.cpp" GX_SHADER_HASH)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/runtime/gx/gx_shader.h" GX_LAYOUT_HASH)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/runtime/gx/gx_d3d12.cpp" GX_PIPELINE_HASH)
string(SHA256 GX_CACHE_HASH "${GX_SHADER_HASH}${GX_LAYOUT_HASH}${GX_PIPELINE_HASH}")
foreach(cache_target runtime port_gpu_resource_test)
  target_compile_definitions(${cache_target} PRIVATE GX_SHADER_CACHE_VERSION="${GX_CACHE_HASH}")
endforeach()

add_library(port_imgui STATIC
  third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
  third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
  third_party/imgui/backends/imgui_impl_dx12.cpp third_party/imgui/backends/imgui_impl_win32.cpp)
target_include_directories(port_imgui PUBLIC third_party/imgui)
target_compile_features(port_imgui PUBLIC cxx_std_17)
target_compile_options(port_imgui PRIVATE ${MELEE_ARCH_FLAG})
target_link_libraries(port_imgui PUBLIC d3d12 dxgi d3dcompiler dwmapi)
target_compile_definitions(runtime PRIVATE GX_PC_SETTINGS)
target_link_libraries(runtime PUBLIC port_imgui)

# NVIDIA Streamline (DLSS): optional, enabled when the SDK subset is present under third_party/streamline.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/streamline/include/sl.h")
  target_compile_definitions(runtime PRIVATE GX_STREAMLINE)
  target_include_directories(runtime PRIVATE third_party/streamline/include)
  target_link_libraries(runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/streamline/lib/x64/sl.interposer.lib" wintrust crypt32 delayimp)
  target_link_options(melee_port PRIVATE /DELAYLOAD:sl.interposer.dll)
  foreach(sl_dll sl.interposer.dll sl.common.dll sl.dlss.dll nvngx_dlss.dll sl.dlss_g.dll nvngx_dlssg.dll sl.reflex.dll sl.pcl.dll)
    add_custom_command(TARGET melee_port POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/third_party/streamline/bin/x64/${sl_dll}" "$<TARGET_FILE_DIR:melee_port>/${sl_dll}")
  endforeach()
  # Intel XeSS (loaded at run time; missing file = no XeSS modes)
  add_custom_command(TARGET melee_port POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/xess/bin/libxess.dll" "$<TARGET_FILE_DIR:melee_port>/libxess.dll")
  # EXPERIMENTAL DLSS 5 Neural Rendering (gx_dlss5.h): NGX core from NVIDIA's DLSS SDK, plus the
  # forwarder DLL the model accepts as a caller. The model itself is never shipped.
  if(MELEE_ENABLE_DLSS5)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ngx/include/nvsdk_ngx.h")
      message(FATAL_ERROR "DLSS 5 requires the NGX SDK headers")
    endif()
    target_compile_definitions(runtime PUBLIC GX_DLSS5)
    target_include_directories(runtime PRIVATE third_party/ngx/include)
    target_link_libraries(runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ngx/lib/nvsdk_ngx_d.lib")
    add_library(dlss5_forwarder SHARED dlss5_forwarder/forwarder.cpp)
    set_target_properties(dlss5_forwarder PROPERTIES OUTPUT_NAME "nvngx.dll_meleedlss5" PREFIX "" SUFFIX ".dll")
    add_dependencies(melee_port dlss5_forwarder)
    add_custom_command(TARGET melee_port POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "$<TARGET_FILE:dlss5_forwarder>" "$<TARGET_FILE_DIR:melee_port>/nvngx.dll_meleedlss5.dll")
  endif()
endif()
if(MELEE_ENABLE_DLSS5 AND NOT TARGET dlss5_forwarder)
  message(FATAL_ERROR "DLSS 5 requires Streamline and NGX")
endif()

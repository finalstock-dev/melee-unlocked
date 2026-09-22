cmake_minimum_required(VERSION 3.15)
project(MeleeUnlocked LANGUAGES CXX)
enable_testing()
find_package(Python3 COMPONENTS Interpreter REQUIRED)

# The instruction set baseline for every target in this build, including native_animation below.
# Declared here (not in port/CMakeLists.txt) because native_animation is built before add_subdirectory
# (port) runs and links into melee_port_compat.exe same as everything else; without this it kept MSVC's
# default codegen regardless of MELEE_CPU_BASELINE, so a "compat" build could still fault on a
# pre-Haswell/pre-Ryzen CPU with an illegal instruction coming out of the animation/spline math that
# runs every frame, even though port/ itself was genuinely built at SSE2.
set(MELEE_CPU_BASELINE "AVX2" CACHE STRING "Instruction set baseline: AVX2, AVX or SSE2")
set_property(CACHE MELEE_CPU_BASELINE PROPERTY STRINGS AVX2 AVX SSE2)
if(MELEE_CPU_BASELINE STREQUAL "SSE2")
  set(MELEE_ARCH_FLAG "")          # the x64 default; naming it explicitly is an error in MSVC
else()
  set(MELEE_ARCH_FLAG "/arch:${MELEE_CPU_BASELINE}")
endif()

set(HOST_FOBJ "${CMAKE_CURRENT_BINARY_DIR}/generated/FObjHost.cpp")
add_custom_command(OUTPUT "${HOST_FOBJ}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_fobj_host.py" "${HOST_FOBJ}"
  DEPENDS tools/generate_fobj_host.py melee/src/sysdolphin/baselib/fobj.c
          melee/src/sysdolphin/baselib/fobj.h melee/src/sysdolphin/baselib/spline.c)
add_library(native_animation STATIC "${HOST_FOBJ}")
target_include_directories(native_animation PUBLIC native)
target_compile_features(native_animation PUBLIC cxx_std_14)
if(MSVC)
  target_compile_options(native_animation PRIVATE ${MELEE_ARCH_FLAG})
endif()
add_executable(native_asset_test tests/native_asset_test.cpp)
add_executable(packed_animation_test tests/packed_animation_test.cpp)
add_executable(native_practice_model_test
  tests/native_practice_model_test.cpp
  port/runtime/hle/native_practice_model.cpp)
target_include_directories(native_practice_model_test PRIVATE port/runtime/hle)
target_compile_features(native_practice_model_test PRIVATE cxx_std_17)
add_test(NAME native_practice_model COMMAND native_practice_model_test)
target_link_libraries(packed_animation_test PRIVATE native_animation)
add_test(NAME packed_animation COMMAND packed_animation_test)
target_link_libraries(native_asset_test PRIVATE native_animation)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/runtime/native-assets/PlFcAJ.dat")
  add_test(NAME native_assets COMMAND native_asset_test "${CMAKE_CURRENT_SOURCE_DIR}/runtime/native-assets")
endif()

option(MELEE_BUILD_EXPERIMENTAL_PORT "Build the incomplete native recompilation prototype" OFF)
if(MELEE_BUILD_EXPERIMENTAL_PORT)
  if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The experimental port requires Windows x64 and MSVC")
  endif()
  add_subdirectory(port)
endif()
add_executable(native_animation_test tests/native_animation_test.cpp)
target_include_directories(native_animation_test PRIVATE native)
target_compile_features(native_animation_test PRIVATE cxx_std_14)
add_test(NAME native_animation COMMAND native_animation_test)
if(WIN32)
  add_executable(native_viewer native/Viewer.cpp)
  target_link_libraries(native_viewer PRIVATE native_animation)
endif()

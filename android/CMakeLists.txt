cmake_minimum_required(VERSION 3.28)
project("rpcsx-android")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_POSITION_INDEPENDENT_CODE on)

# ---------------------------------------------------------------------------
# Per-ABI/CPU-level variant selection.
# Gradle invokes this CMake project once per micro-architecture variant,
# passing e.g. -DUSE_ARCH=armv8.4-a, and expects the produced .so to be
# renamed by Gradle to librpcsx-android-<abi>-<arch>.so afterwards.
# ---------------------------------------------------------------------------
set(USE_ARCH "" CACHE STRING "Specify -march= value to build for (e.g. armv8-a, armv8.4-a, armv9.1-a)")

if(USE_ARCH)
    add_compile_options(-march=${USE_ARCH})
endif()

# ---------------------------------------------------------------------------
# Prebuilt FFmpeg for Android.
# Vanilla rpcs3's own 3rdparty/CMakeLists.txt only builds 3rdparty::ffmpeg
# when NOT ANDROID (rpcs3-ffmpeg-core only ships Windows/macOS/Linux prebuilts).
# We supply the same 3rdparty::ffmpeg alias ourselves here, sourced from the
# RPCS3-Android ffmpeg-android prebuilts, BEFORE pulling in the core so that
# Emu/CMakeLists.txt's unconditional `target_link_libraries(rpcs3_emu PUBLIC
# 3rdparty::ffmpeg)` resolves.
# ---------------------------------------------------------------------------
set(FFMPEG_VERSION 5.1)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(RPCS3_DOWNLOAD_ARCH "arm64-v8a")
else()
    set(RPCS3_DOWNLOAD_ARCH "x86-64")
endif()

if(NOT EXISTS ${CMAKE_BINARY_DIR}/ffmpeg-${FFMPEG_VERSION}.tar.gz)
    message(STATUS "Downloading ffmpeg-${FFMPEG_VERSION}")
    file(DOWNLOAD
        https://github.com/RPCS3-Android/ffmpeg-android/releases/download/${FFMPEG_VERSION}/ffmpeg-${RPCS3_DOWNLOAD_ARCH}-Android.tar.gz
        ${CMAKE_BINARY_DIR}/ffmpeg-${FFMPEG_VERSION}.tar.gz
        SHOW_PROGRESS
    )
endif()

set(FFMPEG_PATH "${CMAKE_BINARY_DIR}/ffmpeg-${FFMPEG_VERSION}")
make_directory(${FFMPEG_PATH})

add_custom_command(
    OUTPUT ${FFMPEG_PATH}/src
    COMMAND ${CMAKE_COMMAND} -E tar xzf ${CMAKE_BINARY_DIR}/ffmpeg-${FFMPEG_VERSION}.tar.gz
    DEPENDS ${CMAKE_BINARY_DIR}/ffmpeg-${FFMPEG_VERSION}.tar.gz
    WORKING_DIRECTORY ${FFMPEG_PATH}
    COMMENT "Unpacking ${CMAKE_BINARY_DIR}/ffmpeg-${FFMPEG_VERSION}.tar.gz"
)
add_custom_target(ffmpeg-unpack DEPENDS ${FFMPEG_PATH}/src)

function(import_ffmpeg_library name)
    add_custom_command(
        OUTPUT "${FFMPEG_PATH}/lib${name}/lib${name}.a"
        DEPENDS ffmpeg-unpack
        WORKING_DIRECTORY ${FFMPEG_PATH}
    )
    add_custom_target(ffmpeg-unpack-${name} DEPENDS "${FFMPEG_PATH}/lib${name}/lib${name}.a")
    add_library(ffmpeg::${name} STATIC IMPORTED GLOBAL)
    set_property(TARGET ffmpeg::${name} PROPERTY IMPORTED_LOCATION "${FFMPEG_PATH}/lib${name}/lib${name}.a")
    set_property(TARGET ffmpeg::${name} PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_PATH}" "${FFMPEG_PATH}/src")
    add_dependencies(ffmpeg::${name} ffmpeg-unpack-${name})
endfunction()

import_ffmpeg_library(avcodec)
import_ffmpeg_library(avformat)
import_ffmpeg_library(avfilter)
import_ffmpeg_library(avdevice)
import_ffmpeg_library(avutil)
import_ffmpeg_library(swscale)
import_ffmpeg_library(swresample)

# Vanilla rpcs3's 3rdparty/CMakeLists.txt skips building ffmpeg when ANDROID
# but still runs its unconditional `add_library(3rdparty::ffmpeg ALIAS
# 3rdparty_ffmpeg)`, so we must provide the 3rdparty_ffmpeg target it aliases
# — and must NOT create the alias ourselves or the two collide.
add_library(3rdparty_ffmpeg INTERFACE)
target_link_libraries(3rdparty_ffmpeg INTERFACE
    ffmpeg::avformat
    ffmpeg::avcodec
    ffmpeg::avutil
    ffmpeg::swscale
    ffmpeg::swresample
)
add_dependencies(3rdparty_ffmpeg ffmpeg-unpack)

# ---------------------------------------------------------------------------
# Options for the core rpcs3 build. These use rpcs3's *current* (vanilla)
# option names - not the outdated rpcsx/rpcs3 fork's, which had drifted
# (WITH_PS4/WITH_PS3/USE_OPENSL/ASMJIT_NO_SHM_OPEN etc. that don't exist here).
# 3rdparty/CMakeLists.txt already has NOT-ANDROID guarded fallback ("dummy")
# targets for OpenGL/OpenAL/GLEW/SDL3/libevdev, and rpcs3/CMakeLists.txt +
# rpcs3/Emu/CMakeLists.txt already skip Qt6/rpcs3qt/rpcs3_lib/rpcs3 exe
# entirely when ANDROID is set - that part of "removing Qt" is already done
# upstream, we only need to supply the Android entry point below.
# ---------------------------------------------------------------------------
set(USE_DISCORD_RPC off)
set(USE_SDL off)
set(USE_FAUDIO off)
set(USE_LIBEVDEV off)
set(USE_SYSTEM_LIBUSB off)
set(USE_SYSTEM_CURL off)
set(USE_SYSTEM_HIDAPI off)
set(USE_SYSTEM_OPENCV off)
set(USE_SYSTEM_ZLIB on)
set(USE_VULKAN on)
set(WITH_LLVM on)
set(BUILD_LLVM on)
set(STATIC_LINK_LLVM on)
set(USE_LTO on)
set(USE_PRECOMPILED_HEADERS off)
set(USE_NATIVE_INSTRUCTIONS off)
set(USE_GAMEMODE off)
set(ASMJIT_NO_SHM_OPEN on)

add_subdirectory(${CMAKE_SOURCE_DIR}/.. rpcs3_core EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# The Android JNI entry point library.
# ---------------------------------------------------------------------------
add_library(${CMAKE_PROJECT_NAME} SHARED
    src/rpcsx-android.cpp
    thirdparty/dev/iso.cpp
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    thirdparty
)

target_link_libraries(${CMAKE_PROJECT_NAME}
    rpcs3_emu
    3rdparty::hidapi
    3rdparty::libusb
    android
    log
)

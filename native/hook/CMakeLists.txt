cmake_minimum_required(VERSION 3.22)
project(cam_hook)

set(CMAKE_CXX_STANDARD 17)

# Build as a shared library injected into cameraserver via ptrace.
add_library(cam_hook SHARED
    libcam_hook.cpp
    inline_hook.cpp
    jpeg_rescale.cpp
)

# Shared with daemon: imu_shm.h defines the gyro shared-ring layout.
target_include_directories(cam_hook PRIVATE ../daemon)

target_compile_options(cam_hook PRIVATE
    -O2
    -fvisibility=hidden          # hide all symbols except __attribute__((visibility("default")))
    -fstack-protector-strong     # canaries on functions with addressable locals / arrays
)

target_link_libraries(cam_hook
    log          # __android_log_print
    jnigraphics  # AndroidBitmap_compress (NDK API 30+) — BLOB JPEG re-encode
    android      # AImageDecoder — decode source JPEG into RGBA
)

# No .so versioning suffix — push as libcam_hook.so directly
set_target_properties(cam_hook PROPERTIES
    OUTPUT_NAME "cam_hook"
    PREFIX      "lib"
    SUFFIX      ".so"
)

// imu_shm.h — shared-memory layout for the gyroscope sample ring.
//
// Writer: hal_inject_daemon (imu.cpp). Produces samples at 200 Hz via the
// Android NDK ASensor API. Advances `head` after each write and bumps a
// seqlock counter so readers can detect torn snapshots.
//
// Reader: libcam_hook.so (in cameraserver). Mmaps the file RO and uses
// `imu_window(exposure_start_ns, exposure_end_ns)` to slice the samples
// that cover a given frame's shutter interval. Step 4 will turn that into
// a motion-blur PSF for the composited document.
//
// File: /data/local/tmp/hal_inject/imu.shm  (fixed IMU_SHM_SIZE).

#pragma once

#include <stdint.h>

#define IMU_SHM_MAGIC   0x494D5530u          // 'IMU0' little-endian
#define IMU_RING_COUNT  1024u                // 1024 samples × 24 B = 24 KB
#define IMU_SHM_SIZE    (32u * 1024u)        // round to 32 KB (header + ring + pad)

#ifdef __cplusplus
extern "C" {
#endif

struct ImuSample {
    uint64_t ts_ns;   // CLOCK_BOOTTIME (matches ANDROID_SENSOR_TIMESTAMP)
    float    x;       // rad/s, Android sensor frame (right-handed, +X right)
    float    y;       // rad/s                       (+Y up)
    float    z;       // rad/s                       (+Z out of screen)
    uint32_t pad;     // → 24 B aligned
};

struct ImuShmHeader {
    uint32_t magic;          // IMU_SHM_MAGIC
    uint32_t ring_count;     // IMU_RING_COUNT (defensive, in case layout evolves)
    uint32_t stride;         // sizeof(ImuSample)
    uint32_t sample_rate_hz; // writer's achieved rate (0 until first batch)
    volatile uint32_t head;  // index of NEXT slot to write (ring position = head % ring_count)
    volatile uint32_t seq;   // bumped odd→write→even; readers use for consistency
    uint64_t boot_ns_at_mmap;// writer's boot time when the file was initialised
    uint8_t  reserved[32];   // → 64-byte aligned ring start
};

// Layout: ImuShmHeader at offset 0, then ImuSample[IMU_RING_COUNT] at offset 64.
// Keep room for growth: IMU_SHM_SIZE is 32 KB so there's ~7 KB slack after the ring.

#ifdef __cplusplus
}
#endif

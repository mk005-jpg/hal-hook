// imu.cpp — gyroscope producer thread for hal_inject_daemon.
//
// Opens the Android ASensor queue for the default gyroscope, mmaps the
// shared ring at /data/local/tmp/hal_inject/imu.shm, and streams samples
// into it at the sensor's native rate (200 Hz on LSM6DSV/Pixel 7a).
//
// The cameraserver hook mmaps the same file RO and slices the ring per
// frame via imu_window() (see hook side for the reader).

#include "imu_shm.h"

#include <android/log.h>
#include <android/looper.h>
#include <android/sensor.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <atomic>
#include <thread>

#define TAG  "hal_imu"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)

#define IMU_SHM_PATH "/data/local/tmp/hal_inject/imu.shm"

static ImuShmHeader *g_hdr  = nullptr;
static ImuSample    *g_ring = nullptr;
static std::atomic<bool> g_running{false};

static uint64_t boot_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// Create or reopen the backing file at IMU_SHM_SIZE, map it RW, and write
// the header. Returns true on success (g_hdr / g_ring populated).
static bool open_shm() {
    int fd = open(IMU_SHM_PATH, O_RDWR | O_CREAT, 0644);
    if (fd < 0) { LOGW("open(%s): %s", IMU_SHM_PATH, strerror(errno)); return false; }
    if (ftruncate(fd, IMU_SHM_SIZE) != 0) {
        LOGW("ftruncate: %s", strerror(errno)); close(fd); return false;
    }
    void *base = mmap(nullptr, IMU_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (base == MAP_FAILED) { LOGW("mmap: %s", strerror(errno)); return false; }

    g_hdr  = (ImuShmHeader *)base;
    g_ring = (ImuSample *)((uint8_t *)base + sizeof(ImuShmHeader));

    g_hdr->magic           = IMU_SHM_MAGIC;
    g_hdr->ring_count      = IMU_RING_COUNT;
    g_hdr->stride          = sizeof(ImuSample);
    g_hdr->sample_rate_hz  = 0;
    g_hdr->head            = 0;
    g_hdr->seq             = 0;
    g_hdr->boot_ns_at_mmap = boot_ns();
    memset(g_hdr->reserved, 0, sizeof(g_hdr->reserved));

    // Readable by cameraserver (which runs as the `cameraserver` uid, not root).
    chmod(IMU_SHM_PATH, 0644);
    return true;
}

// Push one sample to the ring with a seqlock.  Writer is single-threaded
// (this is the only producer), so we only need to bump seq around the
// write + head update.
static inline void push_sample(uint64_t ts_ns, float x, float y, float z) {
    uint32_t idx = g_hdr->head % IMU_RING_COUNT;
    uint32_t next_seq = g_hdr->seq + 1;
    g_hdr->seq = next_seq;                     // odd — write in progress
    __asm__ volatile("dmb ish" ::: "memory");

    g_ring[idx].ts_ns = ts_ns;
    g_ring[idx].x = x; g_ring[idx].y = y; g_ring[idx].z = z;
    g_ring[idx].pad = 0;
    g_hdr->head = g_hdr->head + 1;             // advance after fill

    __asm__ volatile("dmb ish" ::: "memory");
    g_hdr->seq = next_seq + 1;                 // even — writer done
}

static void imu_thread_main() {
    if (!open_shm()) { LOGW("imu_thread: shm init failed"); return; }

    ASensorManager *mgr = ASensorManager_getInstanceForPackage("hal_mission_daemon");
    if (!mgr) {
        LOGW("ASensorManager_getInstance failed"); return;
    }
    // Prefer AOSP's "Corrected Gyroscope Sensor" (bias-removed) over the raw
    // hardware gyro. On Pixel 7a there are two type=android.sensor.gyroscope
    // devices:
    //   handle 0x01010004 "LSM6DSV Gyroscope"             (raw, ≈ 0.08 rad/s bias at rest)
    //   handle 0x5f636779 "Corrected Gyroscope Sensor"    (AOSP, ≈ 0.01 at rest)
    // ASensorManager_getDefaultSensor returns one of them non-deterministically
    // (observed: the raw STMicro). We enumerate and pick by name instead so
    // Step-4's "skip if stationary" gate tightens from 0.08 → 0.01.
    const ASensor *gyro = nullptr;
    ASensorList list = nullptr;
    int count = ASensorManager_getSensorList(mgr, &list);
    LOGD("imu_thread: enumerating %d sensors", count);
    for (int i = 0; i < count; i++) {
        const ASensor *s = list[i];
        int tp = ASensor_getType(s);
        const char *nm = ASensor_getName(s);
        if (tp == ASENSOR_TYPE_GYROSCOPE) {
            LOGD("  gyro candidate: type=%d name='%s' vendor='%s'",
                 tp, nm ? nm : "?", ASensor_getVendor(s) ? ASensor_getVendor(s) : "?");
            if (nm && strstr(nm, "Corrected")) { gyro = s; break; }
            if (!gyro) gyro = s;   // remember any gyroscope as fallback
        }
    }
    if (!gyro) {
        LOGW("no gyroscope sensor available"); return;
    }
    // Create a dedicated ALooper for this thread so the queue has something to
    // pump events into — poll/dispatch pattern.
    ALooper *looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    if (!looper) { LOGW("ALooper_prepare failed"); return; }

    const int IMU_IDENT = 0x494D55;    // 'IMU'
    ASensorEventQueue *q = ASensorManager_createEventQueue(mgr, looper, IMU_IDENT,
                                                           nullptr, nullptr);
    if (!q) { LOGW("createEventQueue failed"); return; }

    if (ASensorEventQueue_enableSensor(q, gyro) < 0) {
        LOGW("enableSensor(gyro) failed"); return;
    }
    // 5 ms = 200 Hz, the LSM6DSV's sustained ceiling on this device.
    // ASensorManager_getMinDelay returns minimum µs; honour it but clamp to 5 ms.
    int32_t min_us = ASensor_getMinDelay(gyro);
    int32_t period_us = min_us > 0 && min_us < 5000 ? 5000 : (min_us > 0 ? min_us : 5000);
    if (ASensorEventQueue_setEventRate(q, gyro, period_us) < 0)
        LOGW("setEventRate(%d us) failed — using sensor default", period_us);

    LOGD("imu_thread: gyro '%s' vendor '%s' ident=0x%x period=%dus",
         ASensor_getName(gyro), ASensor_getVendor(gyro), IMU_IDENT, period_us);

    uint64_t batch_t0 = boot_ns();
    uint32_t batch_n = 0;

    // Software bias correction — the NDK can't see the AOSP "Corrected
    // Gyroscope Sensor", so we subtract an EWMA estimate here. The update
    // is gated on "likely stationary" (|ω_debiased|² < 0.01, i.e. ~0.1
    // rad/s). This settles the bias from 0 → true in ~1 second at 200 Hz
    // and never consumes genuine rotation because fast motion pauses the
    // update. Result: at-rest |ω| drops from ~0.08 to ~0.01 rad/s, so
    // Step-4's skip-if-stationary gate tightens meaningfully.
    float bx = 0.f, by = 0.f, bz = 0.f;
    const float ALPHA = 0.005f;        // half-life ≈ 140 samples
    const float STILL_MAG2 = 0.01f;    // threshold (rad/s)²

    while (g_running.load()) {
        int ident = ALooper_pollOnce(100 /* ms */, nullptr, nullptr, nullptr);
        if (ident == IMU_IDENT) {
            ASensorEvent ev;
            while (ASensorEventQueue_getEvents(q, &ev, 1) > 0) {
                float x = ev.vector.x, y = ev.vector.y, z = ev.vector.z;
                float mx = x - bx, my = y - by, mz = z - bz;
                if (mx*mx + my*my + mz*mz < STILL_MAG2) {
                    bx += ALPHA * (x - bx);
                    by += ALPHA * (y - by);
                    bz += ALPHA * (z - bz);
                    mx = x - bx; my = y - by; mz = z - bz;
                }
                // ASensorEvent.timestamp is CLOCK_BOOTTIME nanoseconds — exactly
                // what SENSOR_TIMESTAMP in CameraMetadata uses, so Step 4 can
                // intersect exposure intervals against these timestamps.
                push_sample((uint64_t)ev.timestamp, mx, my, mz);
                batch_n++;
            }
            // Low-rate telemetry: update achieved rate every 400 samples (~2 s).
            if (batch_n >= 400) {
                uint64_t now = boot_ns();
                double dt = (now - batch_t0) / 1e9;
                if (dt > 0.01)
                    g_hdr->sample_rate_hz = (uint32_t)(batch_n / dt + 0.5);
                batch_t0 = now; batch_n = 0;
            }
        }
    }

    ASensorEventQueue_disableSensor(q, gyro);
    ASensorManager_destroyEventQueue(mgr, q);
}

// Public API — called once from main() right after mkdir()+write_ctrl().
void imu_start() {
    if (g_running.exchange(true)) return;
    std::thread(imu_thread_main).detach();
}

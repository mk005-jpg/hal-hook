// jpeg_rescale.cpp — see header. Minimal NDK-only pipeline:
//
//   JPEG file (frame_<slot>.jpg)                 size S×T
//     └─▶ AImageDecoder → RGBA8888               size S×T
//           └─▶ bilinear resample (inline)       size dst_w × dst_h
//                 └─▶ AndroidBitmap_compress     JPEG at quality
//                       └─▶ caller writes to BLOB dmabuf
//
// NDK API 30+ is assumed; both AImageDecoder and AndroidBitmap_compress ship
// since Android 11 (API 30). We're targeting android-33 in CMake so this is
// always available. No libjpeg-turbo link, no vendor .so lookup.
//
// One-shot per BLOB (decode + scale + encode). Typical cost on a Pixel 7a for
// a 3024² master → 2000×1500 output:  ~15 ms decode, ~10 ms scale, ~30 ms
// encode = ~55 ms per BLOB. BLOBs are rare (one per KYC capture), so this is
// acceptable without a cache. If we need to cache, key (slot_mtime × dst_w ×
// dst_h) and keep the last 4 encodes — later work.

#include "jpeg_rescale.h"

#include <android/bitmap.h>
#include <android/data_space.h>   // ADATASPACE_SRGB (API 28+)
#include <android/imagedecoder.h>
#include <android/log.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define TAG "cam_hook_jpg"
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)

// Growable byte buffer for AndroidBitmap_compress's write callback. Starts at
// 64 KiB, doubles as needed, capped at 32 MiB (a 4080×3072 quality-95 JPEG is
// ~2 MiB — 32 MiB is a sanity ceiling).
namespace {
struct EncBuf {
    uint8_t *data;
    size_t   len;
    size_t   cap;
};
constexpr size_t ENC_INIT_CAP = 64 * 1024;
constexpr size_t ENC_MAX_CAP  = 32 * 1024 * 1024;

bool enc_grow(EncBuf *b, size_t need) {
    if (need <= b->cap) return true;
    size_t ncap = b->cap ? b->cap : ENC_INIT_CAP;
    while (ncap < need) {
        if (ncap >= ENC_MAX_CAP) { ncap = 0; break; }
        ncap *= 2;
        if (ncap > ENC_MAX_CAP) ncap = ENC_MAX_CAP;
    }
    if (ncap == 0 || ncap < need) return false;
    uint8_t *nd = (uint8_t *)realloc(b->data, ncap);
    if (!nd) return false;
    b->data = nd; b->cap = ncap;
    return true;
}

// Matches AndroidBitmap_CompressWriteFunc (typedef in <android/bitmap.h>):
//   bool (*)(void *userContext, const void *data, size_t size)
bool enc_write(void *ctx, const void *data, size_t size) {
    EncBuf *b = (EncBuf *)ctx;
    if (!enc_grow(b, b->len + size)) return false;
    memcpy(b->data + b->len, data, size);
    b->len += size;
    return true;
}

// Bilinear RGBA resample. src is a packed RGBA buffer sw×sh (stride = sw*4).
// dst is caller-allocated RGBA, dst_stride bytes per row. No gamma correction —
// Y channel of a KYC document doesn't need linear-light resample (and screen
// JPEGs aren't tagged with their colorspace anyway, so gamma-correct resampling
// could shift midtones vs what the SDK expects).
void bilinear_rgba(const uint8_t *src, int sw, int sh,
                   uint8_t *dst, int dw, int dh) {
    if (dw <= 0 || dh <= 0) return;
    // x_ratio / y_ratio in 16.16 fixed point so the inner loop is int-only.
    const int64_t xr = ((int64_t)(sw - 1) << 16) / (dw > 1 ? dw - 1 : 1);
    const int64_t yr = ((int64_t)(sh - 1) << 16) / (dh > 1 ? dh - 1 : 1);
    for (int y = 0; y < dh; y++) {
        int64_t sy = (int64_t)y * yr;
        int y0 = (int)(sy >> 16);
        int yf = (int)(sy & 0xFFFF);
        if (y0 >= sh - 1) { y0 = sh - 1; yf = 0; }
        const uint8_t *row0 = src + (size_t)y0 * sw * 4;
        const uint8_t *row1 = src + (size_t)(y0 + (yf ? 1 : 0)) * sw * 4;
        uint8_t *drow = dst + (size_t)y * dw * 4;
        for (int x = 0; x < dw; x++) {
            int64_t sx = (int64_t)x * xr;
            int x0 = (int)(sx >> 16);
            int xf = (int)(sx & 0xFFFF);
            if (x0 >= sw - 1) { x0 = sw - 1; xf = 0; }
            const uint8_t *p00 = row0 + x0 * 4;
            const uint8_t *p01 = row0 + (x0 + (xf ? 1 : 0)) * 4;
            const uint8_t *p10 = row1 + x0 * 4;
            const uint8_t *p11 = row1 + (x0 + (xf ? 1 : 0)) * 4;
            // 2D bilinear via two 1D lerps. Shift back out of 16.16.
            for (int c = 0; c < 4; c++) {
                int top = p00[c] + (((p01[c] - p00[c]) * xf) >> 16);
                int bot = p10[c] + (((p11[c] - p10[c]) * xf) >> 16);
                int v   = top + (((bot - top) * yf) >> 16);
                drow[x * 4 + c] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
            }
        }
    }
}
} // anon

// Shared decode→scale→encode once we have a decoder in hand.
static int rescale_with_decoder(AImageDecoder *dec,
                                int dst_w, int dst_h, int quality,
                                uint8_t **out_ptr, size_t *out_len) {
    *out_ptr = nullptr; *out_len = 0;
    if (!dec || dst_w <= 0 || dst_h <= 0) return -EINVAL;

    // Force RGBA8888 output so our bilinear + Android encode agree on layout.
    if (AImageDecoder_setAndroidBitmapFormat(dec, ANDROID_BITMAP_FORMAT_RGBA_8888) != 0) {
        LOGW("setAndroidBitmapFormat failed");
        return -EIO;
    }
    const AImageDecoderHeaderInfo *hdr = AImageDecoder_getHeaderInfo(dec);
    int sw = AImageDecoderHeaderInfo_getWidth(hdr);
    int sh = AImageDecoderHeaderInfo_getHeight(hdr);
    if (sw <= 0 || sh <= 0) return -EINVAL;

    // If AImageDecoder supports native-scale decode (AImageDecoder_setTargetSize
    // lands at API 30), prefer it — the decoder will do a cheap integer-ratio
    // IDCT-scale when sw/dst_w is a power of two and a built-in bilinear
    // otherwise, often faster than our software bilinear and with better
    // antialiasing. If the target size isn't accepted, fall through to a
    // same-size decode + software resample.
    bool native_scale = (AImageDecoder_setTargetSize(dec, dst_w, dst_h) == 0);

    int out_w = native_scale ? dst_w : sw;
    int out_h = native_scale ? dst_h : sh;
    size_t out_stride = (size_t)out_w * 4;
    size_t out_bytes  = out_stride * (size_t)out_h;

    uint8_t *rgba = (uint8_t *)malloc(out_bytes);
    if (!rgba) return -ENOMEM;
    int dr = AImageDecoder_decodeImage(dec, rgba, out_stride, out_bytes);
    if (dr != ANDROID_IMAGE_DECODER_SUCCESS) {
        LOGW("decodeImage failed: %d", dr);
        free(rgba);
        return -EIO;
    }

    // Software bilinear only when native scale wasn't accepted.
    uint8_t *final_rgba = rgba;
    bool owns_final = true;
    if (!native_scale && (sw != dst_w || sh != dst_h)) {
        uint8_t *scaled = (uint8_t *)malloc((size_t)dst_w * dst_h * 4);
        if (!scaled) { free(rgba); return -ENOMEM; }
        bilinear_rgba(rgba, sw, sh, scaled, dst_w, dst_h);
        free(rgba);
        final_rgba = scaled;
    }

    // Encode RGBA → JPEG via NDK AndroidBitmap_compress (API 30+).
    AndroidBitmapInfo info{};
    info.width  = (uint32_t)dst_w;
    info.height = (uint32_t)dst_h;
    info.stride = (uint32_t)(dst_w * 4);
    info.format = ANDROID_BITMAP_FORMAT_RGBA_8888;
    info.flags  = 0;
    EncBuf eb{};
    int cr = AndroidBitmap_compress(&info, ADATASPACE_SRGB, final_rgba,
                                     ANDROID_BITMAP_COMPRESS_FORMAT_JPEG,
                                     quality, &eb, enc_write);
    if (owns_final) free(final_rgba);
    if (cr != 0) {
        LOGW("AndroidBitmap_compress failed: %d", cr);
        free(eb.data);
        return -EIO;
    }
    *out_ptr = eb.data;
    *out_len = eb.len;
    return 0;
}

int jpeg_rescale_file_to_mem(const char *src_path,
                             int dst_w, int dst_h, int quality,
                             uint8_t **out_ptr, size_t *out_len) {
    *out_ptr = nullptr; *out_len = 0;
    if (!src_path) return -EINVAL;
    int fd = open(src_path, O_RDONLY);
    if (fd < 0) { LOGW("open(%s): %s", src_path, strerror(errno)); return -errno; }

    AImageDecoder *dec = nullptr;
    int cr = AImageDecoder_createFromFd(fd, &dec);
    // AImageDecoder dup's the fd internally per NDK contract — safe to close.
    close(fd);
    if (cr != ANDROID_IMAGE_DECODER_SUCCESS || !dec) {
        LOGW("AImageDecoder_createFromFd(%s): %d", src_path, cr);
        return -EIO;
    }
    int rc = rescale_with_decoder(dec, dst_w, dst_h, quality, out_ptr, out_len);
    AImageDecoder_delete(dec);
    return rc;
}

int jpeg_rescale_mem_to_mem(const uint8_t *src_jpeg, size_t src_len,
                            int dst_w, int dst_h, int quality,
                            uint8_t **out_ptr, size_t *out_len) {
    *out_ptr = nullptr; *out_len = 0;
    if (!src_jpeg || src_len == 0) return -EINVAL;
    AImageDecoder *dec = nullptr;
    int cr = AImageDecoder_createFromBuffer(src_jpeg, src_len, &dec);
    if (cr != ANDROID_IMAGE_DECODER_SUCCESS || !dec) {
        LOGW("AImageDecoder_createFromBuffer: %d", cr);
        return -EIO;
    }
    int rc = rescale_with_decoder(dec, dst_w, dst_h, quality, out_ptr, out_len);
    AImageDecoder_delete(dec);
    return rc;
}

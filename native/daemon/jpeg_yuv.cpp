#include "jpeg_yuv.h"

#include <android/imagedecoder.h>
#include <android/bitmap.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static inline unsigned char clamp8(int v) {
    return v < 0 ? 0 : (v > 255 ? 255 : (unsigned char)v);
}

static bool write_all(const char *path, const void *buf, size_t len) {
    if (!path) return true;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    bool ok = write(fd, buf, len) == (ssize_t)len;
    close(fd);
    return ok;
}

bool jpeg_to_yuv(const char *jpg, const char *nv21_out, const char *nv12_out,
                 const char *dim_out, int max_side) {
    int fd = open(jpg, O_RDONLY);
    if (fd < 0) return false;

    AImageDecoder *dec = nullptr;
    int rc = AImageDecoder_createFromFd(fd, &dec);
    if (rc != ANDROID_IMAGE_DECODER_SUCCESS || !dec) { close(fd); return false; }

    const AImageDecoderHeaderInfo *info = AImageDecoder_getHeaderInfo(dec);
    int w = AImageDecoderHeaderInfo_getWidth(info);
    int h = AImageDecoderHeaderInfo_getHeight(info);

    if (max_side > 0 && (w > max_side || h > max_side)) {
        float sc = (float)max_side / (w > h ? w : h);
        w = (int)(w * sc); h = (int)(h * sc);
    }
    w &= ~1; h &= ~1;                 // even dims for 4:2:0
    if (w < 2 || h < 2) { AImageDecoder_delete(dec); close(fd); return false; }
    AImageDecoder_setTargetSize(dec, w, h);
    AImageDecoder_setAndroidBitmapFormat(dec, ANDROID_BITMAP_FORMAT_RGBA_8888);

    size_t stride = AImageDecoder_getMinimumStride(dec);
    size_t pxsize = stride * (size_t)h;
    unsigned char *px = (unsigned char *)malloc(pxsize);
    if (!px) { AImageDecoder_delete(dec); close(fd); return false; }

    rc = AImageDecoder_decodeImage(dec, px, stride, pxsize);
    AImageDecoder_delete(dec);
    close(fd);
    if (rc != ANDROID_IMAGE_DECODER_SUCCESS) { free(px); return false; }

    size_t ysize = (size_t)w * h;
    size_t total = ysize * 3 / 2;
    unsigned char *nv21 = (unsigned char *)malloc(total);
    unsigned char *nv12 = (unsigned char *)malloc(total);
    if (!nv21 || !nv12) { free(px); free(nv21); free(nv12); return false; }

    unsigned char *vu = nv21 + ysize;   // NV21 chroma (V,U)
    unsigned char *uv = nv12 + ysize;   // NV12 chroma (U,V)
    size_t ci = 0;
    for (int y = 0; y < h; y++) {
        const unsigned char *row = px + (size_t)y * stride;
        for (int x = 0; x < w; x++) {
            int r = row[x * 4], g = row[x * 4 + 1], b = row[x * 4 + 2];
            int Y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
            unsigned char yv = clamp8(Y);
            nv21[(size_t)y * w + x] = yv;
            nv12[(size_t)y * w + x] = yv;
            if ((y & 1) == 0 && (x & 1) == 0) {
                int U = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;  // Cb
                int V = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;   // Cr
                vu[ci]     = clamp8(V); vu[ci + 1] = clamp8(U);           // NV21: V,U
                uv[ci]     = clamp8(U); uv[ci + 1] = clamp8(V);           // NV12: U,V
                ci += 2;
            }
        }
    }
    free(px);

    bool ok = write_all(nv21_out, nv21, total);
    write_all(nv12_out, nv12, total);
    if (dim_out) {
        char d[32];
        int n = snprintf(d, sizeof(d), "%dx%d", w, h);
        write_all(dim_out, d, (size_t)n);
    }
    free(nv21);
    free(nv12);
    return ok;
}

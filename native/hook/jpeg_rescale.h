// jpeg_rescale.h — decode a stored JPEG, resample to arbitrary (dst_w, dst_h),
// and re-encode to JPEG so write_blob can overwrite the KYC app's BLOB buffer
// with a JPEG whose self-described W×H MATCHES the stream's negotiated dims
// (CameraX does `decodeRegion(cropRect = stream_w × stream_h)` on the decoded
// bitmap — if a 1920×1080 JPEG lands in a 2000×1500 stream the crop falls off
// the edge and returns black). Uses NDK-only APIs (AImageDecoder + AndroidBitmap
// _compress, both API 30+) so no third-party deps.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

// Decode `src_path` (JPEG), scale (bilinear) to dst_w × dst_h, and encode as a
// JPEG at `quality` into a newly-allocated buffer returned via *out_ptr /
// *out_len. Caller owns *out_ptr and must free() it.
//   src_path: JPEG file on disk (frame_<slot>.jpg)
//   dst_w, dst_h: target output dims — the KYC stream's negotiated s->width / s->height
//   quality: 1..100 (92 is a good default; matches the app's Export quality)
//   out_ptr, out_len: returned JPEG bytes
// Returns 0 on success, negative errno on failure. On failure *out_ptr=nullptr.
int jpeg_rescale_file_to_mem(const char *src_path,
                             int dst_w, int dst_h, int quality,
                             uint8_t **out_ptr, size_t *out_len);

// Same as above but reads the source JPEG from an already-mmapped buffer. Used
// if the caller wants to avoid an extra open()/read() (frame_<slot>.jpg is
// usually small enough that the kernel cache makes the file-path variant free).
int jpeg_rescale_mem_to_mem(const uint8_t *src_jpeg, size_t src_len,
                            int dst_w, int dst_h, int quality,
                            uint8_t **out_ptr, size_t *out_len);

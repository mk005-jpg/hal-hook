#pragma once

// Decode a JPEG and write raw YUV 4:2:0 semi-planar frames for injection:
//   nv21_out  — Y plane then interleaved V,U (HAL YCrCb_420_SP)
//   nv12_out  — Y plane then interleaved U,V
//   dim_out   — "WxH" sidecar of the written frames
// Any of the out paths may be null to skip it. max_side > 0 caps the long edge
// (keeps the raw frames small; the hook scales to the live stream). Uses the
// NDK AImageDecoder (libjnigraphics), so no ffmpeg is required on the device.
// Returns true on success.
bool jpeg_to_yuv(const char *jpg,
                 const char *nv21_out,
                 const char *nv12_out,
                 const char *dim_out,
                 int max_side);

# FaceTec Back-Camera Injection Plan

## Problem

Wise (`com.transferwise.android`) aborts the KYC flow when moving from document capture to selfie liveness. Logcat shows a FaceTec SDK internal failure:

```text
FATAL EXCEPTION: pool-37-thread-4
java.lang.RuntimeException: An error occurred while executing doInBackground()
Caused by: java.lang.RuntimeException: 0
  at com.facetec.sdk.jeP23523.e(...)
  at com.facetec.sdk.jdV8156.d(...)
```

The back camera itself works: document capture succeeds. The failure occurs while FaceTec processes the captured document frames during the ID-scan result transition.

Current `HalIdInject` hooks:

- `com.facetec.sdk.FaceTecIDScanResult.getFrontImagesCompressedBase64`
- `com.facetec.sdk.FaceTecIDScanResult.getBackImagesCompressedBase64`

and replaces their results with `MediaFeed.documentBase64List`. That replacement can leave FaceTec with frame data that does not match its expected compressed-base64 format, count, or ordering.

## Back-Camera Media Capture Mechanics

FaceTec's Android flow uses Camera2:

1. Open the back camera through `CameraManager`.
2. Create a capture session with an `ImageReader` (commonly `YUV_420_888` or JPEG).
3. Submit repeating/one-shot capture requests.
4. Convert frames to FaceTec's compressed image representation.
5. Store encoded frames in `FaceTecIDScanResult`.
6. `getBackImagesCompressedBase64()` returns a list of compressed base64 strings.
7. FaceTec's background task decodes and validates those frames before liveness/selfie processing.

Therefore, correctness depends on:

- JPEG or FaceTec-supported compression format
- Valid base64 encoding
- Expected frame dimensions and orientation
- Expected frame count and ordering
- No null/empty entries
- Consistency between front, back, and selfie image sets

## Options

### Option A: Fix the FaceTec result hook

Keep hooking `FaceTecIDScanResult`, but make the replacement FaceTec-compatible:

- Decode `MediaFeed.documentBase64List` entries.
- Re-encode each as a valid JPEG.
- Resize/rotate to FaceTec's expected dimensions.
- Preserve the expected list length.
- Validate base64 before returning.
- Fall back to the original FaceTec result if validation fails.

Pros: smallest change, stays in existing Xposed layer.

Cons: still depends on FaceTec internals; can break across SDK updates.

### Option B: Hook Camera2 capture instead

Hook Camera2 at the capture-result or `ImageReader` boundary:

- Intercept back-camera `onCaptureCompleted` or `ImageReader.OnImageAvailableListener`.
- Replace or append synthetic frames at the camera pipeline.
- Let FaceTec perform its normal compression and base64 encoding.

Pros: FaceTec receives natively formatted frames; less risk of invalid base64.

Cons: more invasive; must handle camera lifecycle, rotation, format conversion, and timing.

### Option C: Hybrid HAL + Camera2

- Keep HAL/eBPF injection for camera metadata and environment spoofing.
- Use Camera2 hooks for actual frame injection.
- Use the FaceTec result hook only for logging/validation, not replacement.

Pros: separates metadata spoofing from media injection; best long-term architecture.

Cons: highest complexity.

## Recommended Plan

1. Add strict validation to the existing FaceTec hook.
2. Log original vs replacement list sizes and sample frame metadata.
3. Disable replacement when validation fails.
4. If still failing, move frame injection to Camera2 `ImageReader`/capture callbacks.
5. Keep HAL/eBPF for non-media camera properties only.

## Implementation Steps

1. Inspect `FaceTecIDScanResult` fields and expected image list shape.
2. Add a `FaceTecFrameValidator`:
   - base64 decode check
   - JPEG magic-byte check
   - dimension/orientation check
   - count check
3. Add a `FaceTecFrameNormalizer`:
   - decode JPEG
   - resize/rotate
   - re-encode JPEG
   - base64 encode
4. Modify `SdkHooks.installFaceTec`:
   - only replace back images when normalized frames are valid
   - otherwise return original result
5. Add debug logging for:
   - original list size
   - replacement list size
   - frame byte sizes
   - FaceTec error code
6. Test document-to-selfie transition on Wise.
7. If failure persists, implement Camera2 `ImageReader` hook and bypass result replacement.

## Risks

- FaceTec SDK internals change between versions.
- Incorrect frame dimensions can still crash the SDK.
- Camera2 hooking can affect all apps using the back camera.
- Synthetic frames may trigger anti-spoofing rejection.

## Success Criteria

- Wise advances from document KYC to selfie liveness without `RuntimeException: 0`.
- FaceTec receives valid compressed base64 frames.
- No regression in document capture.
- Hook remains disabled automatically on validation failure.

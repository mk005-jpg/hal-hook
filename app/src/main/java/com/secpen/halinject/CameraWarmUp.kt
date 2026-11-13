package com.secpen.halinject

import android.content.Context
import android.graphics.SurfaceTexture
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.CaptureResult
import android.hardware.camera2.TotalCaptureResult
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.view.Surface
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/**
 * Open the BACK camera just long enough for the HAL hook (running inside
 * cameraserver) to see at least one real sensor frame and dump a fresh
 * real_yuv.nv21 to its control dir. No preview is shown; the single
 * SurfaceTexture target is invisible.
 *
 * Why this exists: snapYuvTo() asks the daemon to copy the hook's cached
 * real_yuv, but that cache is only refreshed while SOMEONE is streaming
 * the camera. From the HAL app itself nobody normally is — so without a
 * warm-up the BLOB backdrop is the dim/stale frame from whatever app
 * last used the camera.
 *
 * Usage:
 *   val warm = CameraWarmUp(ctx)
 *   val gotFrame = warm.openAndAwaitFirstFrame(800)   // blocks ≤800ms
 *   bridge.snapYuvTo(dst)                              // snap while open
 *   warm.close()
 *
 * Caller MUST hold android.permission.CAMERA.
 */
class CameraWarmUp(private val ctx: Context) {

    private var device: CameraDevice? = null
    private var session: CameraCaptureSession? = null
    private var surface: Surface? = null
    private var texture: SurfaceTexture? = null
    private var thread: HandlerThread? = null
    private var handler: Handler? = null

    /**
     * Open the back camera, start a repeating preview-template request, and
     * block up to [timeoutMs] for AUTO-EXPOSURE to converge. Just waiting
     * for the first frame is not enough: the first few frames after open
     * are near-black (sensor at min exposure) and snapping then captures
     * a black real_yuv. We explicitly wait for AE_STATE == CONVERGED or
     * LOCKED (fallback: 20 frames ≈ 666 ms at 30 fps).
     *
     * Returns true on success, false on permission / open timeout / no AE
     * convergence within the budget.
     */
    fun openAndAwaitFirstFrame(timeoutMs: Long): Boolean {
        val mgr = ctx.getSystemService(Context.CAMERA_SERVICE) as CameraManager
        val backId = pickBackId(mgr) ?: return false.also {
            Log.w(TAG, "no back camera id") }

        thread = HandlerThread("hal-warmup").also { it.start() }
        handler = Handler(thread!!.looper)

        val opened = CountDownLatch(1)
        val aeReady = CountDownLatch(1)

        try {
            mgr.openCamera(backId, object : CameraDevice.StateCallback() {
                override fun onOpened(cam: CameraDevice) {
                    device = cam
                    // 640x480 preview — cheapest size we can request, matches
                    // the hook's ctrl_yuv=640 threshold so the compositor fires
                    // and the snap trigger inside write_yuv runs.
                    texture = SurfaceTexture(0).also { it.setDefaultBufferSize(640, 480) }
                    surface = Surface(texture)
                    val req = cam.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW).apply {
                        addTarget(surface!!)
                        // Make auto-exposure as aggressive as possible so the
                        // first usable frame arrives fast. CONTROL_MODE_AUTO
                        // is the default but we set it explicitly to be safe.
                        set(CaptureRequest.CONTROL_MODE,
                            android.hardware.camera2.CameraMetadata.CONTROL_MODE_AUTO)
                        set(CaptureRequest.CONTROL_AE_MODE,
                            android.hardware.camera2.CameraMetadata.CONTROL_AE_MODE_ON)
                        set(CaptureRequest.CONTROL_AWB_MODE,
                            android.hardware.camera2.CameraMetadata.CONTROL_AWB_MODE_AUTO)
                        set(CaptureRequest.CONTROL_AF_MODE,
                            android.hardware.camera2.CameraMetadata
                                .CONTROL_AF_MODE_CONTINUOUS_PICTURE)
                    }
                    cam.createCaptureSession(listOf(surface),
                        object : CameraCaptureSession.StateCallback() {
                            override fun onConfigured(s: CameraCaptureSession) {
                                session = s
                                s.setRepeatingRequest(req.build(),
                                    object : CameraCaptureSession.CaptureCallback() {
                                        private var frames = 0
                                        override fun onCaptureCompleted(
                                            s: CameraCaptureSession,
                                            r: CaptureRequest,
                                            result: TotalCaptureResult
                                        ) {
                                            frames++
                                            val ae = result.get(CaptureResult.CONTROL_AE_STATE)
                                            val converged =
                                                ae == CaptureResult.CONTROL_AE_STATE_CONVERGED ||
                                                ae == CaptureResult.CONTROL_AE_STATE_LOCKED ||
                                                ae == CaptureResult.CONTROL_AE_STATE_FLASH_REQUIRED
                                            if (converged || frames >= 20) {
                                                if (aeReady.count > 0)
                                                    Log.d(TAG, "ae ready frame=$frames state=$ae")
                                                aeReady.countDown()
                                            }
                                        }
                                    }, handler)
                                opened.countDown()
                            }
                            override fun onConfigureFailed(s: CameraCaptureSession) {
                                Log.w(TAG, "session configure failed")
                                opened.countDown()
                            }
                        }, handler)
                }
                override fun onDisconnected(cam: CameraDevice) {
                    cam.close(); device = null; opened.countDown()
                }
                override fun onError(cam: CameraDevice, err: Int) {
                    Log.w(TAG, "camera open err=$err"); cam.close(); device = null
                    opened.countDown()
                }
            }, handler)
        } catch (se: SecurityException) {
            Log.w(TAG, "CAMERA permission missing"); return false
        } catch (e: Exception) {
            Log.e(TAG, "openCamera threw", e); return false
        }

        // Wait for session to be configured, then for AE to converge.
        if (!opened.await(timeoutMs, TimeUnit.MILLISECONDS)) {
            Log.w(TAG, "open timeout"); return false
        }
        if (device == null || session == null) return false
        return aeReady.await(timeoutMs, TimeUnit.MILLISECONDS)
    }

    fun close() {
        try { session?.stopRepeating() } catch (_: Exception) {}
        try { session?.close() } catch (_: Exception) {}
        try { device?.close() } catch (_: Exception) {}
        try { surface?.release() } catch (_: Exception) {}
        try { texture?.release() } catch (_: Exception) {}
        try { thread?.quitSafely() } catch (_: Exception) {}
        session = null; device = null; surface = null; texture = null
        thread = null; handler = null
    }

    private fun pickBackId(mgr: CameraManager): String? {
        return try {
            mgr.cameraIdList.firstOrNull { id ->
                val ch = mgr.getCameraCharacteristics(id)
                ch.get(CameraCharacteristics.LENS_FACING) ==
                    CameraCharacteristics.LENS_FACING_BACK
            }
        } catch (_: Exception) { null }
    }

    companion object { private const val TAG = "CameraWarmUp" }
}

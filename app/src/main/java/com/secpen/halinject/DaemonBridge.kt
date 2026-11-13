package com.secpen.halinject

import android.net.LocalSocket
import android.net.LocalSocketAddress
import android.util.Log
import org.json.JSONObject
import java.io.*
import java.util.concurrent.atomic.AtomicBoolean

private const val TAG = "DaemonBridge"
private const val SOCK = "/data/local/tmp/hal_inject/daemon.sock"

data class DaemonStatus(
    val connected: Boolean = false,
    val active: Boolean    = false,
    val fridaPid: Int      = -1,
    val backNode: String   = "",
    val imagePath: String  = "",
    val camTarget: String  = "back",   // which facing is being injected
    val camCurrent: String = "",       // which camera is currently streaming
    val slot: String       = "idfront", // active media slot
    val fmt: String        = "nv21"     // YUV chroma order
)

class DaemonBridge {

    private val running = AtomicBoolean(false)
    var onStatus: ((DaemonStatus) -> Unit)? = null

    // ── One-shot command ───────────────────────────────────────────────────────
    private fun send(cmd: String): String {
        return try {
            val sock = LocalSocket()
            sock.connect(LocalSocketAddress(SOCK, LocalSocketAddress.Namespace.FILESYSTEM))
            sock.outputStream.write((cmd + "\n").toByteArray())
            sock.outputStream.flush()
            val line = BufferedReader(InputStreamReader(sock.inputStream)).readLine() ?: "ERROR no response"
            sock.close()
            line
        } catch (e: Exception) {
            Log.e(TAG, "send($cmd): ${e.message}")
            "ERROR ${e.message}"
        }
    }

    fun setImage(path: String): Boolean = send("SET_IMAGE $path").startsWith("OK")

    // Hand the daemon two prepared slot JPEGs: yuvPath (preview/analysis, rotated
    // for the consumer) and blobPath (the still-capture, saved as-is). The daemon
    // builds frame_<slot>.nv21/.nv12 from yuvPath and frame_<slot>.jpg from blobPath.
    fun setSlotImage(slot: String, yuvPath: String, blobPath: String): Boolean =
        send("SET_SLOT_IMAGE $slot $yuvPath $blobPath").startsWith("OK")

    // Enable YUV (back-camera video) injection for streams >= width px; 0 disables.
    fun setYuvWidth(width: Int): Boolean = send("SET_YUV_WIDTH $width").startsWith("OK")

    // Which camera facing to inject: "back" (front passes through), "front", "both".
    fun setCam(mode: String): Boolean = send("SET_CAM $mode").startsWith("OK")

    // Which prepared media slot to inject: "idfront" or "idback".
    fun setSlot(slot: String): Boolean = send("SET_SLOT $slot").startsWith("OK")

    // YUV chroma order injected into YUV streams: "nv21" or "nv12".
    fun setFmt(fmt: String): Boolean = send("SET_FMT $fmt").startsWith("OK")

    // Set the compositor's region (ctrl_region) normalized 0..1 to the sensor
    // frame. 4-arg = manual aspect (hook still letterboxes source into it); 3-arg
    // = auto-aspect (size as rect-H fraction; hook derives W from source dims).
    // The app calls this from the user's Place-Frame placement on Export so each
    // KYC target's expected ID position becomes the real inject rect.
    // Opt in to the Step-1 region compositor. Daemon defaults this to OFF
    // (legacy full-overwrite) so a session that never calls setComposite is
    // bit-identical to the pre-compositor build — the regression baseline.
    fun setComposite(on: Boolean): Boolean =
        send("SET_COMPOSITE ${if (on) 1 else 0}").startsWith("OK")

    // Content-PAD (physical-document anti-spoof defense): paper-fibre texture
    // + sparse laminate specular pinpoints on the injected ID region. Defeats
    // FaceTec IDScan / IDnow "likely digital screen" detectors. 0 = off,
    // ~50 = natural paper match, 100 = aggressive, >100 = exaggerate.
    fun setContentPad(percent: Int): Boolean =
        send("SET_CONTENT_PAD ${percent.coerceIn(0, 200)}").startsWith("OK")

    // Force a specific BLOB rescale target (w=0 and/or h=0 → AUTO: hook matches
    // the KYC-negotiated stream dims at return-time). Written through to
    // ctrl_blob_res_w / _h so the hook picks it up on its next BLOB.
    fun setBlobRes(w: Int, h: Int): Boolean =
        send("SET_BLOB_RES ${w.coerceAtLeast(0)} ${h.coerceAtLeast(0)}").startsWith("OK")

    // Synchronous snapshot: asks the hook to dump the next PRE-composite real
    // YUV frame (via /data/local/tmp/hal_inject/snap_yuv trigger), then the
    // daemon (root) copies the file to `dstPath` with mode 0644 so the app
    // can read it. Returns null on failure, else "WxH" dims string.
    // Needs the full stack active: cameraserver + hook + composite_enabled.
    // Returns null on failure, else "WxH" dims (with "STALE " prefix when
    // the daemon served the cached frame because no stream was active —
    // the HAL app's normal case, since it doesn't hold the camera during
    // Export). Caller renders the dims the same either way; the STALE tag
    // is just for a status/toast hint.
    fun snapYuvTo(dstPath: String): String? {
        val r = send("SNAP_YUV $dstPath")
        return when {
            r.startsWith("OK STALE ") -> "STALE " + r.removePrefix("OK STALE ").trim()
            r.startsWith("OK ")       -> r.removePrefix("OK ").trim()
            else                      -> null
        }
    }

    fun setRegion(cx: Float, cy: Float, w: Float, h: Float): Boolean =
        send("SET_REGION $cx $cy $w $h").startsWith("OK")

    fun setRegionAuto(cx: Float, cy: Float, size: Float): Boolean =
        send("SET_REGION $cx $cy $size").startsWith("OK")

    fun start(): Boolean = send("START").startsWith("OK")

    fun stop(): Boolean = send("STOP").startsWith("OK")

    fun backNode(): String = send("BACK_NODE").trim()

    fun fetchStatus(): DaemonStatus {
        val resp = send("STATUS")
        if (!resp.startsWith("STATUS ")) return DaemonStatus(connected = false)
        return try {
            val json = JSONObject(resp.removePrefix("STATUS ").trim())
            DaemonStatus(
                connected  = true,
                active     = json.optBoolean("active", false),
                fridaPid   = json.optInt("frida_pid", -1),
                backNode   = json.optString("back_node", ""),
                imagePath  = json.optString("image", ""),
                camTarget  = json.optString("cam_target", "back"),
                camCurrent = json.optString("cam_current", ""),
                slot       = json.optString("slot", "idfront"),
                fmt        = json.optString("fmt", "nv21")
            )
        } catch (e: Exception) {
            DaemonStatus(connected = false)
        }
    }

    // ── Polling thread ────────────────────────────────────────────────────────
    fun startPolling() {
        if (running.getAndSet(true)) return
        Thread {
            while (running.get()) {
                val s = fetchStatus()
                onStatus?.invoke(s)
                Thread.sleep(1500)
            }
        }.also { it.isDaemon = true }.start()
    }

    fun stopPolling() { running.set(false) }
}

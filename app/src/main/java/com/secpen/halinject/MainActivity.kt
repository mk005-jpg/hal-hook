package com.secpen.halinject

import android.app.Activity
import android.content.Intent
import android.graphics.BitmapFactory
import android.graphics.Color
import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.widget.*
import androidx.appcompat.app.AppCompatActivity
import java.io.File

// Target apps — all 8 KYC targets (informational only; injection applies to
// whichever app is using the back camera at the time)
private val TARGETS = listOf(
    "com.deblock.deblockapp"        to "Deblock (FaceTec + Regula)",
    "tech.pnlfin.finom"             to "Finom (SumSub)",
    "com.moneybookers.skrillpayments" to "Skrill (Jumio)",
    "com.kaching.merchant"          to "Kaching (Onfido)",
    "com.bunq.android"              to "Bunq (Incode)",
    "vivid.money"                   to "Vivid (Veriff)",
    "com.transferwise.android"      to "Wise (FaceTec)",
    "eu.qonto.qonto"                to "Qonto (Fourthline/Onfido)"
)

private const val REQ_IMAGE = 1001

class MainActivity : AppCompatActivity() {

    private val bridge = DaemonBridge()
    private val handler = Handler(Looper.getMainLooper())

    // Views
    private lateinit var tvStatus:    TextView
    private lateinit var tvNode:      TextView
    private lateinit var ivPreview:   ImageView
    private lateinit var tvImagePath: TextView
    private lateinit var btnPick:     Button
    private lateinit var btnToggle:   Button
    private lateinit var llTargets:   LinearLayout
    private lateinit var tvStats:     TextView
    private lateinit var tvCam:       TextView

    private var injecting = false
    private var imagePath: String? = null
    private var initPushed = false   // pushed YUV/chroma defaults to the daemon once
    private var autoArmed = false    // auto-sent START once, when the daemon first connected

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        buildUi()
        ensureDaemon()

        bridge.onStatus = { status ->
            handler.post {
                applyStatus(status)
            }
        }
        bridge.startPolling()
    }

    override fun onDestroy() {
        super.onDestroy()
        bridge.stopPolling()
    }

    // ── Activity result: image picked ─────────────────────────────────────────
    override fun onActivityResult(req: Int, result: Int, data: Intent?) {
        super.onActivityResult(req, result, data)
        if (req != REQ_IMAGE || result != Activity.RESULT_OK) return
        val uri: Uri = data?.data ?: return
        toast("Preparing image…")

        Thread {
            val path = ImagePrep.prepare(this, uri)
            handler.post {
                if (path != null) {
                    imagePath = path
                    tvImagePath.text = "Image: ${File(path).name}"
                    val bmp = BitmapFactory.decodeFile(path)
                    if (bmp != null) ivPreview.setImageBitmap(bmp)
                    bridge.setImage(path)
                    toast("Frame ready")
                } else {
                    toast("Image preparation failed")
                }
            }
        }.start()
    }

    // ── UI ────────────────────────────────────────────────────────────────────
    private fun buildUi() {
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(24, 24, 24, 24)
            setBackgroundColor(Color.parseColor("#1a1a2e"))
        }
        setContentView(root)

        // Title
        root.addView(textView("HAL Inject — Pixel 7a", 22f, Color.WHITE, true))
        root.addView(textView("Injects the targeted camera · the other passes through", 13f, Color.GRAY))
        root.addView(divider())

        // Status card
        tvStatus = textView("Daemon: checking…", 14f, Color.YELLOW)
        tvNode   = textView("Back cam node: detecting…", 12f, Color.LTGRAY)
        root.addView(card(tvStatus, tvNode))

        // Image section
        ivPreview   = ImageView(this).apply {
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 220)
            scaleType = ImageView.ScaleType.CENTER_CROP
            setBackgroundColor(Color.parseColor("#2a2a3e"))
        }
        tvImagePath = textView("No image selected", 12f, Color.LTGRAY)
        btnPick     = button("Pick Injection Frame") { pickImage() }
        val btnPlaceFrame = button("Place Frame (Advanced)") {
            startActivity(Intent(this, FramePlacementActivity::class.java))
        }
        root.addView(card(ivPreview, tvImagePath, btnPick, btnPlaceFrame))

        // YUV (back-camera video / liveness) injection control.
        // BLOB still-capture is always injected; YUV must be opted in because
        // enabling it affects back-camera video streams only (front cam untouched).
        val yuvRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity     = Gravity.CENTER_VERTICAL
        }
        val etYuvWidth = EditText(this).apply {
            setText("640")
            inputType = InputType.TYPE_CLASS_NUMBER
            setTextColor(Color.WHITE)
            layoutParams = LinearLayout.LayoutParams(200, LinearLayout.LayoutParams.WRAP_CONTENT)
        }
        val swYuv = Switch(this).apply {
            text = "YUV video ≥ (px)"
            setTextColor(Color.WHITE)
            layoutParams = LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f)
            setOnCheckedChangeListener { _, checked ->
                val w = if (checked) (etYuvWidth.text.toString().toIntOrNull() ?: 640) else 0
                Thread { bridge.setYuvWidth(w) }.start()
                toast(if (checked) "YUV inject ≥ ${w}px" else "YUV inject off")
            }
            // ON by default: KYC document/liveness flows are always YUV_420_888
            // streams (never BLOB), so YUV injection must be on for them to work.
            isChecked = true
        }
        yuvRow.addView(swYuv)
        yuvRow.addView(etYuvWidth)
        val swFmt = Switch(this).apply {
            text = "Chroma order: NV12 (off = NV21)"
            setTextColor(Color.WHITE)
            setOnCheckedChangeListener { _, checked ->
                val f = if (checked) "nv12" else "nv21"
                Thread { bridge.setFmt(f) }.start()
                toast("YUV chroma: $f")
            }
        }
        root.addView(card(
            textView("Liveness / video (advanced)", 13f, Color.WHITE, true),
            yuvRow,
            swFmt,
            textView("YUV covers the active camera per the target below.", 11f, Color.GRAY)
        ))

        // Camera-facing target: which camera gets injected; the other passes
        // through. Also toggled on-device by a volume-up double-press (for ID
        // flows: back for the document scan, front for the selfie/liveness).
        val camRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity     = Gravity.CENTER_VERTICAL
        }
        fun camBtn(label: String, mode: String) = button(label) {
            Thread { bridge.setCam(mode) }.start()
            toast("Inject target: $mode")
        }.also { it.layoutParams = LinearLayout.LayoutParams(0, 110, 1f).apply { setMargins(4,4,4,4) } }
        camRow.addView(camBtn("Back", "back"))
        camRow.addView(camBtn("Front", "front"))
        camRow.addView(camBtn("Both", "both"))
        tvCam = textView("Target: back · active: — · slot: idfront", 12f, Color.LTGRAY)
        root.addView(card(
            textView("Inject camera — Vol+ ×2 switches camera, Vol− ×2 switches ID slot", 13f, Color.WHITE, true),
            camRow,
            tvCam
        ))

        // Target apps list
        llTargets = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        TARGETS.forEach { (pkg, label) ->
            llTargets.addView(targetRow(pkg, label))
        }
        val scroll = ScrollView(this).apply {
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f)
        }
        scroll.addView(card(textView("Target Apps (informational)", 13f, Color.WHITE, true), llTargets))
        root.addView(scroll)

        // Stats
        tvStats = textView("Frames: —", 12f, Color.LTGRAY)
        root.addView(card(tvStats))

        // Toggle button
        btnToggle = button("START INJECTION", Color.parseColor("#00c853")) {
            toggleInjection()
        }
        root.addView(btnToggle)
    }

    private fun applyStatus(s: DaemonStatus) {
        if (!s.connected) {
            tvStatus.text = "Daemon: NOT CONNECTED"
            tvStatus.setTextColor(Color.RED)
            return
        }
        tvStatus.text = "Daemon: connected | inject ${if (s.active) "ACTIVE" else "idle"}"
        tvStatus.setTextColor(if (s.active) Color.parseColor("#00c853") else Color.YELLOW)
        // Once the daemon is reachable, push the KYC-sane defaults (YUV injection
        // on at ≥640px, NV21 chroma) so document/liveness streams inject without
        // the user having to toggle anything. Idempotent; done once per launch.
        if (!initPushed) {
            initPushed = true
            Thread { bridge.setYuvWidth(640); bridge.setFmt("nv21") }.start()
        }
        // Auto-arm the hook once the daemon is reachable, so simply opening the
        // app loads the hook into cameraserver without a manual START tap. The
        // in-cameraserver hook no-ops until a frame/slot file exists, so arming
        // before any image is selected is safe. One-shot per launch; a user STOP
        // is respected (autoArmed stays set, so we never fight a manual stop).
        if (!autoArmed && !s.active) {
            autoArmed = true
            Thread { bridge.start() }.start()
        }
        if (s.backNode.isNotEmpty()) tvNode.text = "Back cam: ${s.backNode}"
        tvCam.text = "Target: ${s.camTarget} · active: ${if (s.camCurrent.isEmpty()) "—" else s.camCurrent} · slot: ${s.slot} · ${s.fmt}"
        if (s.active && !injecting) {
            injecting = true
            btnToggle.text = "STOP INJECTION"
            btnToggle.setBackgroundColor(Color.parseColor("#d32f2f"))
        } else if (!s.active && injecting) {
            injecting = false
            btnToggle.text = "START INJECTION"
            btnToggle.setBackgroundColor(Color.parseColor("#00c853"))
        }
        // Read stats file
        readStatsFile()
    }

    private fun readStatsFile() {
        Thread {
            try {
                val f = File("/data/local/tmp/hal_inject/stats.json")
                if (f.exists()) {
                    val json = org.json.JSONObject(f.readText())
                    // The hook writes {"frames":N,"active":bool}; show the live
                    // frame count so the user can confirm injection without adb.
                    val frames = json.optInt("frames", 0)
                    val active = json.optBoolean("active", false)
                    handler.post {
                        tvStats.text = "Injected frames: $frames" + if (active) "  (active)" else ""
                    }
                }
            } catch (_: Exception) {}
        }.start()
    }

    private fun toggleInjection() {
        // No single-image requirement: frames may have been prepared via the
        // Frame Placement slots (idfront/idback) instead of "Pick Injection Frame".
        // The in-cameraserver hook safely skips if no frame file exists yet, so
        // arming with slots-only (the normal ID front/back workflow) must work.
        Thread {
            if (!injecting) {
                bridge.start()
            } else {
                bridge.stop()
            }
        }.start()
    }

    private fun pickImage() {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "image/*"
        }
        startActivityForResult(intent, REQ_IMAGE)
    }

    private fun ensureDaemon() {
        Thread {
            try {
                // setsid + full redirection so the daemon detaches into its own
                // session and survives this su invocation tearing down (a bare
                // "… &" dies when the su shell exits). The daemon's single-instance
                // guard makes a duplicate launch (boot module already started it) a
                // harmless no-op.
                val proc = Runtime.getRuntime().exec(arrayOf("su", "-c",
                    "setsid /data/local/tmp/hal_inject/hal_inject_daemon </dev/null >/data/local/tmp/hal_inject/daemon.log 2>&1 &"))
                proc.waitFor()
            } catch (_: Exception) {}
        }.start()
    }

    // ── View helpers ──────────────────────────────────────────────────────────
    private fun textView(text: String, size: Float, color: Int, bold: Boolean = false) =
        TextView(this).apply {
            this.text = text
            textSize  = size
            setTextColor(color)
            if (bold) setTypeface(null, android.graphics.Typeface.BOLD)
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT).also { it.setMargins(0,4,0,4) }
        }

    private fun button(label: String, color: Int = Color.parseColor("#3949ab"), action: () -> Unit) =
        Button(this).apply {
            text = label
            setTextColor(Color.WHITE)
            setBackgroundColor(color)
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 120).also { it.setMargins(0, 8, 0, 8) }
            setOnClickListener { action() }
        }

    private fun divider() = View(this).apply {
        setBackgroundColor(Color.DKGRAY)
        layoutParams = LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 1)
            .also { it.setMargins(0, 8, 0, 8) }
    }

    private fun card(vararg views: View) = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        setBackgroundColor(Color.parseColor("#16213e"))
        setPadding(20, 16, 20, 16)
        layoutParams = LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT,
            LinearLayout.LayoutParams.WRAP_CONTENT).also { it.setMargins(0, 8, 0, 8) }
        views.forEach { addView(it) }
    }

    private fun targetRow(pkg: String, label: String) = LinearLayout(this).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity     = Gravity.CENTER_VERTICAL
        setPadding(0, 6, 0, 6)

        val dot = View(context).apply {
            setBackgroundColor(Color.parseColor("#00c853"))
            layoutParams = LinearLayout.LayoutParams(12, 12).also { it.setMargins(0, 0, 12, 0) }
        }
        val tv = TextView(context).apply {
            text = label
            textSize = 12f
            setTextColor(Color.LTGRAY)
        }
        addView(dot)
        addView(tv)
    }

    private fun toast(msg: String) = Toast.makeText(this, msg, Toast.LENGTH_SHORT).show()
}

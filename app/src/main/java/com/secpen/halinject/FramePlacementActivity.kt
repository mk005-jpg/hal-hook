package com.secpen.halinject

import android.Manifest
import android.app.Activity
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.*
import android.net.Uri
import android.os.Bundle
import android.view.MotionEvent
import android.view.View
import android.widget.*
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.app.AlertDialog
import androidx.core.app.ActivityCompat
import androidx.exifinterface.media.ExifInterface
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min

class FramePlacementActivity : AppCompatActivity() {

    private companion object {
        const val REQ_BG    = 2001
        const val REQ_DOC   = 2002
        const val REQ_FLASH = 2003
        const val REQ_GUIDE = 2004
        const val REQ_CAM   = 2010   // runtime CAMERA permission for warm-up
    }

    private lateinit var frameCanvas: FrameCanvas
    private lateinit var tvStatus: TextView
    private val bridge = DaemonBridge()

    // Two media slots: ID front side / ID back side. The slot being edited/exported.
    private var slot = "idfront"
    private lateinit var slotIdf: Button
    private lateinit var slotIdb: Button

    // PREVIEW/analysis (YUV) rotation in degrees CW. Those streams get the sensor
    // rotation applied by the consumer, so the frame is pre-rotated to land upright.
    // 270° CW is the Pixel 7a back-camera default (confirmed against Skrill/Jumio).
    private var rotDeg = 270
    private lateinit var rotBtn: Button

    // CAPTURE (BLOB still) fine-rotation in degrees CW, applied to the CARD before it
    // is fit upright into the landscape capture frame. Default 0 (the card source is
    // already upright); cycle to 90/270 if the loaded card image is itself rotated.
    private var captureRot = 0
    private lateinit var captureRotBtn: Button

    // CAPTURE fill — zoom of the WHOLE image over the cover scale that fills the frame.
    // 100% = exact cover (the whole image fills the screen, only aspect overflow is cropped);
    // >100 zooms in (crops more), <100 pulls back (letterbox). Tunable per target.
    private var captureFillPct = 100
    // Corner-radius dial for the cropped doc (percent of width). 0 = sharp.
    // ISO 7810 ID-1 spec: 3.18 mm / 85.6 mm = 3.71% — real cards use this.
    private var cropRadiusPct = 3.71f
    private var captureNoisePct = 100   // BLOB still sensor-noise strength %; 0 = off
    private var contentPadPct   = 0     // Content-PAD strength %; defeats FaceTec/IDnow "digital screen" detector. 0 = off.
    private lateinit var captureFillBtn: Button

    // CAPTURE output resolution (the still JPEG written into the BLOB). 1920×1080 is the
    // landscape still Wise/Veriff use; tunable per target via the Res button.
    // (0, 0) = AUTO: hook rescales BLOB to match whatever the KYC app
    // negotiated at runtime (s->width × s->height). User can force a specific
    // size via the Res button as an override (some SDKs reject mismatched
    // aspects even at "right" dims).
    private var outW = 0
    private var outH = 0
    private lateinit var resBtn: Button
    private lateinit var lockBtn: Button

    // Saved per-target capture profiles (fill %, rotations, output res, aspect lock),
    // persisted in SharedPreferences so they survive reinstalls and can be recalled per
    // KYC target (Wise, Veriff, Jumio, …).
    private lateinit var targetSpinner: Spinner
    private lateinit var targetNameField: EditText
    private var suppressSpinner = false
    private val prefs by lazy { getSharedPreferences("hal_targets", Context.MODE_PRIVATE) }

    // Crop: lets a FULL PHOTO (card on a surface) be cut down to the card before export,
    // so the capture-resize sizes the card — not the whole photo — to the frame.
    private lateinit var cropBtn: Button

    // Per-slot in-memory state: each slot keeps its own loaded images + placement, so
    // switching slots and coming back preserves the position (persists until Reset).
    private class SlotState {
        var doc: Bitmap? = null
        var bg: Bitmap? = null
        var flash: Bitmap? = null
        var rect = RectF()
        var hasRect = false
    }
    private val slots = mutableMapOf("idfront" to SlotState(), "idback" to SlotState())
    private lateinit var resetBtn: Button

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Color.parseColor("#1a1a2e"))
            setPadding(8, 8, 8, 8)
        }

        // Media slot selector — prepare two ID images (front/back side). The active
        // slot (also switchable on-device by a volume-down double-press) is injected.
        val slotRow = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        slotIdf = slotButton("ID Front", "idfront")
        slotIdb = slotButton("ID Back", "idback")
        resetBtn = Button(this).apply {
            text = "Reset"
            setTextColor(Color.WHITE)
            setBackgroundColor(Color.parseColor("#c62828"))
            setOnClickListener { resetSlot() }
            layoutParams = LinearLayout.LayoutParams(0, -2, 0.6f).apply { setMargins(2, 4, 2, 4) }
        }
        slotRow.addView(slotIdf)
        slotRow.addView(slotIdb)
        slotRow.addView(resetBtn)
        root.addView(slotRow, LinearLayout.LayoutParams(-1, -2))

        frameCanvas = FrameCanvas(this)
        root.addView(frameCanvas, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f))
        highlightSlot()

        // Button row
        val btnRow = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        addBtn(btnRow, "Background") { pick(REQ_BG) }
        addBtn(btnRow, "Document")   { pick(REQ_DOC) }
        cropBtn = Button(this).apply {
            text = "Crop"
            setTextColor(Color.WHITE)
            setBackgroundColor(Color.parseColor("#ff7043"))
            setOnClickListener { toggleCrop() }
            layoutParams = LinearLayout.LayoutParams(0, -2, 1f).apply { setMargins(2, 4, 2, 4) }
        }
        btnRow.addView(cropBtn)
        addBtn(btnRow, "Flash")      { pick(REQ_FLASH) }
        addBtn(btnRow, "Save Crop")  { saveCropToFile() }
        addBtn(btnRow, "Export ▶")   { exportAndSend() }
        root.addView(btnRow, LinearLayout.LayoutParams(-1, -2))

        // Second button row — KYC guide + clear (full-screen alignment overlay).
        // Load a SCREENSHOT of the KYC app's guide rectangle: it fills the canvas
        // edge-to-edge (no sizing), you drag the doc rect onto its guide box,
        // Export, done. The guide is never exported — pure placement aid.
        val guideRow = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        addBtn(guideRow, "KYC Guide")   { pick(REQ_GUIDE) }
        addBtn(guideRow, "Clear Guide") {
            frameCanvas.clearGuide()
            tvStatus.text = "Guide cleared"
        }
        root.addView(guideRow, LinearLayout.LayoutParams(-1, -2))

        // Row 1: preview rotation + capture rotation (independent — preview streams
        // get the consumer's sensor rotation, the saved still-capture does not).
        val ctrlRow = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        rotBtn = Button(this).apply {
            text = "Preview: ${rotDeg}°"
            setTextColor(Color.WHITE)
            setBackgroundColor(Color.parseColor("#3949ab"))
            setOnClickListener {
                rotDeg = (rotDeg + 90) % 360
                text = "Preview: ${rotDeg}°"
                tvStatus.text = "Preview rot ${rotDeg}° — tap Export ▶ to apply"
            }
            layoutParams = LinearLayout.LayoutParams(0, -2, 1f).apply { setMargins(2, 0, 2, 4) }
        }
        captureRotBtn = Button(this).apply {
            text = "Capture: ${captureRot}°"
            setTextColor(Color.WHITE)
            setBackgroundColor(Color.parseColor("#5e35b1"))
            setOnClickListener {
                captureRot = (captureRot + 90) % 360
                text = "Capture: ${captureRot}°"
                tvStatus.text = "Capture rot ${captureRot}° — tap Export ▶ to apply"
            }
            layoutParams = LinearLayout.LayoutParams(0, -2, 1f).apply { setMargins(2, 0, 2, 4) }
        }
        captureFillBtn = Button(this).apply {
            text = "Fill: ${captureFillPct}%"
            setTextColor(Color.WHITE)
            setBackgroundColor(Color.parseColor("#00838f"))
            setOnClickListener {
                val steps = intArrayOf(100, 110, 120, 130, 90)
                val i = steps.indexOf(captureFillPct)
                captureFillPct = steps[(if (i < 0) 2 else i + 1) % steps.size]
                text = "Fill: ${captureFillPct}%"
                tvStatus.text = "Capture fill ${captureFillPct}% — tap Export ▶ to apply"
            }
            layoutParams = LinearLayout.LayoutParams(0, -2, 1f).apply { setMargins(2, 0, 2, 4) }
        }
        val noiseBtn = Button(this).apply {
            text = if (captureNoisePct > 0) "Noise: ${captureNoisePct}%" else "Noise: off"
            setTextColor(Color.WHITE)
            setBackgroundColor(Color.parseColor("#455a64"))
            setOnClickListener {
                val steps = intArrayOf(100, 130, 0, 80)
                val i = steps.indexOf(captureNoisePct)
                captureNoisePct = steps[(if (i < 0) 0 else i + 1) % steps.size]
                text = if (captureNoisePct > 0) "Noise: ${captureNoisePct}%" else "Noise: off"
                tvStatus.text = "BLOB noise ${if (captureNoisePct > 0) "${captureNoisePct}%" else "off"} — tap Export ▶ to apply"
            }
            layoutParams = LinearLayout.LayoutParams(0, -2, 1f).apply { setMargins(2, 0, 2, 4) }
        }
        val radiusBtn = Button(this).apply {
            text = "R: %.1f%%".format(cropRadiusPct)
            setTextColor(Color.WHITE)
            setBackgroundColor(Color.parseColor("#5d4037"))
            setOnClickListener {
                // Cycle: off → 2% → 3.71% (ISO) → 5% → 7%
                val steps = floatArrayOf(0f, 2f, 3.71f, 5f, 7f)
                val i = steps.indexOfFirst { kotlin.math.abs(it - cropRadiusPct) < 0.05f }
                cropRadiusPct = steps[(if (i < 0) 2 else i + 1) % steps.size]
                text = if (cropRadiusPct < 0.05f) "R: off"
                       else "R: %.1f%%".format(cropRadiusPct)
                tvStatus.text = if (cropRadiusPct < 0.05f)
                    "Corner radius: sharp — re-tap Crop → Apply to re-render"
                    else "Corner radius: %.2f%% of width — re-tap Crop → Apply to re-render"
                        .format(cropRadiusPct)
            }
            layoutParams = LinearLayout.LayoutParams(0, -2, 1f).apply { setMargins(2, 0, 2, 4) }
        }
        val padBtn = Button(this).apply {
            text = if (contentPadPct > 0) "PAD: ${contentPadPct}%" else "PAD: off"
            setTextColor(Color.WHITE)
            setBackgroundColor(Color.parseColor("#6a1b9a"))
            setOnClickListener {
                // Cycle: off → 40 → 60 → 100 → 150 → off. 50-ish is "natural
                // paper match"; >100 exaggerates (useful for seeing what the
                // SDK ought to notice but doesn't). Pushed to daemon immediately
                // AND on next Export (which calls setContentPad too).
                val steps = intArrayOf(0, 40, 60, 100, 150)
                val i = steps.indexOf(contentPadPct)
                contentPadPct = steps[(if (i < 0) 0 else i + 1) % steps.size]
                text = if (contentPadPct > 0) "PAD: ${contentPadPct}%" else "PAD: off"
                Thread { bridge.setContentPad(contentPadPct) }.start()
                tvStatus.text = "Content-PAD: " + (if (contentPadPct > 0)
                    "${contentPadPct}% (fibre + specular on inject region)"
                    else "off (clean injected pixels)")
            }
            layoutParams = LinearLayout.LayoutParams(0, -2, 1f).apply { setMargins(2, 0, 2, 4) }
        }
        ctrlRow.addView(rotBtn)
        ctrlRow.addView(captureRotBtn)
        ctrlRow.addView(captureFillBtn)
        ctrlRow.addView(noiseBtn)
        ctrlRow.addView(radiusBtn)
        ctrlRow.addView(padBtn)
        root.addView(ctrlRow, LinearLayout.LayoutParams(-1, -2))

        // Row 2: aspect lock + capture output resolution.
        val lockResRow = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        lockBtn = Button(this).apply {
            text = "Aspect: LOCK"
            setTextColor(Color.WHITE)
            setBackgroundColor(Color.parseColor("#00897b"))
            setOnClickListener {
                frameCanvas.lockAspect = !frameCanvas.lockAspect
                text = if (frameCanvas.lockAspect) "Aspect: LOCK" else "Aspect: FREE"
                setBackgroundColor(Color.parseColor(if (frameCanvas.lockAspect) "#00897b" else "#3949ab"))
                tvStatus.text = if (frameCanvas.lockAspect)
                    "Aspect locked — resize keeps the ID's H:W ratio"
                    else "Aspect free — resize can distort the ID"
            }
            layoutParams = LinearLayout.LayoutParams(0, -2, 1f).apply { setMargins(2, 0, 2, 4) }
        }
        resBtn = Button(this).apply {
            text = resLabel()
            setTextColor(Color.WHITE)
            setBackgroundColor(Color.parseColor("#6d4c41"))
            setOnClickListener {
                // (0,0) = AUTO: hook rescales the BLOB to match whatever the
                // KYC app negotiated at runtime (s->width × s->height). Any
                // other entry forces that resolution via ctrl_blob_res_w/h —
                // useful when an SDK rejects "right dims, wrong aspect".
                // Square = myTU "square-center" mode. 4032×3024 = Wise's
                // negotiated BLOB (Pixel 7a sensor max 4:3). 4080×3072 =
                // alt-max some SDKs negotiate instead.
                val steps = arrayOf(
                    intArrayOf(0, 0),                            // AUTO (hook matches stream)
                    intArrayOf(4032, 3024),                       // Wise / most 4:3 KYCs
                    intArrayOf(4080, 3072),                       // alt sensor max
                    intArrayOf(1920, 1080), intArrayOf(1280, 720),
                    intArrayOf(2560, 1440), intArrayOf(1080, 1920),
                    intArrayOf(2048, 2048), intArrayOf(3024, 3024))
                val i = steps.indexOfFirst { it[0] == outW && it[1] == outH }
                val n = steps[(if (i < 0) 0 else i + 1) % steps.size]
                outW = n[0]; outH = n[1]
                text = resLabel()
                // Push override to the daemon so even old ctrl files get
                // overwritten (zeros = "auto", hook falls back to stream dims).
                Thread { bridge.setBlobRes(outW, outH) }.start()
                tvStatus.text = if (outW == 0 || outH == 0)
                    "Capture res: AUTO — hook matches KYC's negotiated stream dims"
                    else "Capture res ${outW}×${outH} — tap Export ▶ to apply"
            }
            layoutParams = LinearLayout.LayoutParams(0, -2, 1f).apply { setMargins(2, 0, 2, 4) }
        }
        // Dedicated "reset to AUTO" button — one tap, no cycling through the
        // whole Res carousel. Addresses the common stuck-state where a prior
        // myTU/square test left outW=outH=3024 and persisted it across
        // launches, so Wise/landscape KYCs captured deformed until the user
        // noticed and manually cycled Res back to AUTO.
        val autoBtn = Button(this).apply {
            text = "AUTO"
            setTextColor(Color.WHITE)
            setBackgroundColor(Color.parseColor("#2e7d32"))
            setOnClickListener {
                outW = 0; outH = 0
                resBtn.text = resLabel()
                Thread { bridge.setBlobRes(0, 0) }.start()
                tvStatus.text = "Capture res reset to AUTO — hook matches KYC stream dims"
            }
            layoutParams = LinearLayout.LayoutParams(0, -2, 0.5f).apply { setMargins(2, 0, 2, 4) }
        }
        lockResRow.addView(lockBtn)
        lockResRow.addView(resBtn)
        lockResRow.addView(autoBtn)
        root.addView(lockResRow, LinearLayout.LayoutParams(-1, -2))

        // Row 3a: name a target profile and save the current capture dimensions under it.
        val targetSaveRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = android.view.Gravity.CENTER_VERTICAL
        }
        targetNameField = EditText(this).apply {
            hint = "target name (e.g. Wise)"
            setTextColor(Color.WHITE); setHintTextColor(Color.GRAY)
            textSize = 14f; setSingleLine()
        }
        targetSaveRow.addView(targetNameField, LinearLayout.LayoutParams(0, -2, 1f))
        addBtn(targetSaveRow, "Save") { saveTargetFromField() }
        root.addView(targetSaveRow, LinearLayout.LayoutParams(-1, -2))

        // Row 3b: recall a saved target (loads its dimensions + red placement guide).
        val targetLoadRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = android.view.Gravity.CENTER_VERTICAL
        }
        targetLoadRow.addView(TextView(this).apply {
            text = "Load "; setTextColor(Color.LTGRAY); textSize = 13f
        })
        targetSpinner = Spinner(this)
        targetSpinner.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(p: AdapterView<*>, v: View?, pos: Int, id: Long) {
                if (suppressSpinner || pos == 0) return
                val name = p.getItemAtPosition(pos) as String
                loadTarget(name)
                targetNameField.setText(name)   // prefill so it's easy to re-save/overwrite
            }
            override fun onNothingSelected(p: AdapterView<*>) {}
        }
        targetLoadRow.addView(targetSpinner, LinearLayout.LayoutParams(0, -2, 1f))
        addBtn(targetLoadRow, "Del")  { deleteSelectedTarget() }
        root.addView(targetLoadRow, LinearLayout.LayoutParams(-1, -2))
        refreshTargetSpinner()

        // Opacity slider
        val opRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = android.view.Gravity.CENTER_VERTICAL
            setPadding(8, 4, 8, 4)
        }
        opRow.addView(TextView(this).apply {
            text = "Opacity "; setTextColor(Color.LTGRAY); textSize = 13f
        })
        val slider = SeekBar(this).apply {
            max = 255; progress = 255
            setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(s: SeekBar, p: Int, u: Boolean) {
                    frameCanvas.docAlpha = p; frameCanvas.invalidate()
                }
                override fun onStartTrackingTouch(s: SeekBar) {}
                override fun onStopTrackingTouch(s: SeekBar) {}
            })
        }
        opRow.addView(slider, LinearLayout.LayoutParams(0, -2, 1f))
        root.addView(opRow, LinearLayout.LayoutParams(-1, -2))

        tvStatus = TextView(this).apply {
            text = "Load background + document, then drag/resize"
            setTextColor(Color.LTGRAY); textSize = 12f; setPadding(8, 4, 8, 4)
        }
        root.addView(tvStatus, LinearLayout.LayoutParams(-1, -2))

        setContentView(root)
        title = "Frame Placement"

        // Rehydrate the session the user last had on screen: loaded images per
        // slot (cached as PNG/JPEG in cacheDir) + placement rect + capture
        // profile. Deferred to the first layout pass so FrameCanvas has a
        // non-zero size to resolve the normalised doc-rect against.
        frameCanvas.post { restoreLastSession() }
    }

    // Lifecycle: an in-flight Export may still be writing to the daemon when
    // the user leaves. Snapshot the session for rehydrate-on-return, and let
    // the Export's safeUi() guard drop any UI write that races with teardown.
    override fun onPause() {
        super.onPause()
        try { saveLastSession() } catch (_: Throwable) {}
    }

    override fun onRequestPermissionsResult(
        requestCode: Int, permissions: Array<out String>, grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == REQ_CAM &&
            grantResults.isNotEmpty() &&
            grantResults[0] == PackageManager.PERMISSION_GRANTED) {
            // User approved CAMERA — resume the Export they tried to run.
            exportAndSend()
        } else if (requestCode == REQ_CAM) {
            tvStatus.text = "CAMERA denied — backdrop will be stale or black"
        }
    }

    private fun addBtn(parent: LinearLayout, label: String, action: () -> Unit) {
        parent.addView(Button(this).apply {
            text = label
            setTextColor(Color.WHITE)
            setBackgroundColor(Color.parseColor("#3949ab"))
            setOnClickListener { action() }
        }, LinearLayout.LayoutParams(0, -2, 1f).apply { setMargins(2, 4, 2, 4) })
    }

    private fun slotButton(label: String, s: String) = Button(this).apply {
        text = label
        setTextColor(Color.WHITE)
        setOnClickListener { switchSlot(s) }
        layoutParams = LinearLayout.LayoutParams(0, -2, 1f).apply { setMargins(2, 4, 2, 4) }
    }

    // Switch slots, preserving each slot's loaded images + placement in memory.
    private fun switchSlot(s: String) {
        if (::frameCanvas.isInitialized && s != slot) saveCanvasToSlot()
        slot = s
        if (::frameCanvas.isInitialized) {
            val st = slots[s]!!
            frameCanvas.restoreState(st.doc, st.bg, st.flash, if (st.hasRect) st.rect else null)
            cropBtn.text = "Crop"
            tvStatus.text = "Slot $s — " +
                (if (st.doc != null) "restored placement" else "empty, load a Document")
        }
        highlightSlot()
        Thread { bridge.setSlot(s) }.start()
    }

    // Capture the live canvas state into the current slot (called before leaving it).
    private fun saveCanvasToSlot() {
        val st = slots[slot]!!
        st.doc = frameCanvas.docBmp
        st.bg = frameCanvas.backgroundBmp
        st.flash = frameCanvas.flashBmp
        st.rect = frameCanvas.getDocRect()
        st.hasRect = !st.rect.isEmpty
    }

    // Reset clears the current slot (recycles its images) back to empty.
    // Capture the bitmap refs FIRST, null out the canvas via restoreState,
    // then recycle — so any redraw in between sees null (skip draw) rather
    // than a recycled pointer (throwIfCannotDraw crash).
    private fun resetSlot() {
        val st = slots[slot]!!
        val oldDoc = frameCanvas.docBmp
        val oldBg  = frameCanvas.backgroundBmp
        val oldFl  = frameCanvas.flashBmp
        st.doc = null; st.bg = null; st.flash = null; st.rect = RectF(); st.hasRect = false
        frameCanvas.restoreState(null, null, null, null)
        frameCanvas.guideRect = null
        oldDoc?.recycle(); oldBg?.recycle(); oldFl?.recycle()
        cropBtn.text = "Crop"
        tvStatus.text = "Slot $slot reset — load a Document"
    }

    private fun highlightSlot() {
        val on = Color.parseColor("#00897b"); val off = Color.parseColor("#3949ab")
        slotIdf.setBackgroundColor(if (slot == "idfront") on else off)
        slotIdb.setBackgroundColor(if (slot == "idback")  on else off)
    }

    private fun pick(req: Int) {
        startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "image/*"
        }, req)
    }

    @Suppress("DEPRECATION")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (resultCode != Activity.RESULT_OK) return
        val uri = data?.data ?: return
        val bmp = loadBitmap(uri) ?: run { tvStatus.text = "Failed to decode image"; return }
        when (requestCode) {
            REQ_BG -> {
                // Swap-then-recycle: previously recycle-then-swap left a
                // window where backgroundBmp/docBmp/flashBmp pointed at a
                // recycled bitmap. A redraw in that window (layout pass,
                // invalidate from the saveCanvasToSlot-adjacent code) hit
                // BaseCanvas.throwIfCannotDraw and crashed the app.
                val oldBg = frameCanvas.backgroundBmp
                frameCanvas.setBackground(bmp); saveCanvasToSlot()
                oldBg?.recycle()
                tvStatus.text = "BG: ${bmp.width}×${bmp.height}"
            }
            REQ_DOC -> {
                val oldDoc = frameCanvas.docBmp
                frameCanvas.setDocument(bmp); saveCanvasToSlot()
                if (oldDoc != null && oldDoc != bmp) oldDoc.recycle()
                tvStatus.text = "Doc: ${bmp.width}×${bmp.height} — place it, then Export ▶"
            }
            REQ_FLASH -> {
                if (frameCanvas.docBmp == null) {
                    bmp.recycle()
                    tvStatus.text = "Load + place the Document first, then Flash"
                } else {
                    val oldFlash = frameCanvas.flashBmp
                    frameCanvas.setFlashDoc(bmp); saveCanvasToSlot()
                    oldFlash?.recycle()
                    tvStatus.text = "Flash loaded — placement LOCKED; see-through overlay for alignment. Export to apply."
                }
            }
            REQ_GUIDE -> {
                // Full-canvas KYC guide overlay. The canvas aspect ≈ phone screen,
                // so a straight-from-screen KYC screenshot aligns 1:1 with where
                // the ID will actually land in the camera preview.
                val oldGuide = frameCanvas.guideBmp
                frameCanvas.setGuide(bmp)
                oldGuide?.recycle()
                tvStatus.text = "Guide: ${bmp.width}×${bmp.height} — drag Doc onto the guide's box, then Export ▶"
            }
        }
        // Persist the pick immediately — the Activity could be killed before
        // onPause fires (process-death scenario on low-mem devices). Catch
        // Throwable (not just Exception) because JSONObject on Android throws
        // NoSuchMethodError for unsupported numeric overloads, which Exception
        // misses and kills the activity.
        try { saveLastSession() } catch (_: Throwable) {}
    }

    private fun loadBitmap(uri: Uri): Bitmap? = try {
        contentResolver.openInputStream(uri)?.use { BitmapFactory.decodeStream(it) }
    } catch (_: Exception) { null }

    private fun exportAndSend() {
        val doc = frameCanvas.docBmp ?: run { tvStatus.text = "No document loaded"; return }
        // COMPOSITOR mode (ctrl_composite=1): export the RAW cropped doc as the
        // YUV source; the hook paints it into the sensor rect driven by setRegion
        // below (user's Place-Frame placement). Real camera scene passes through
        // EVERYWHERE outside the rect — physical-tunnel benefit kept. KYC Guide
        // + Background stay visual-only in Place Frame (alignment references),
        // so they don't ride into the injection and don't overwrite the real view.
        // Capture (BLOB) still cover-fills the landscape still output because
        // the uploaded JPEG is a document photo, not a scene.
        tvStatus.text = "Exporting…"

        // Option-B real-scene BLOB backdrop. Flow:
        //   1. Warm up the back camera ourselves (SurfaceTexture target, no
        //      preview UI) so cameraserver streams RIGHT NOW. Without this,
        //      snapYuvTo returns whatever stale cached frame the hook had
        //      from the last app that used the camera — often dim or dark.
        //   2. Wait for the first frame so the hook has definitely populated
        //      real_yuv in its own cameraserver-owned file.
        //   3. Ask the daemon to copy real_yuv.{nv21,dim} into our cache
        //      dir with mode 0644 (SELinux denies cameraserver writing
        //      there directly).
        //   4. Close the warm-up session.
        // The CAMERA permission check is here (not onCreate) so the user
        // only sees the prompt the first time they hit Export.
        val snapDst = java.io.File(cacheDir, "real_yuv.nv21").absolutePath
        val snapDims: String? = if (ActivityCompat.checkSelfPermission(this,
                Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            ActivityCompat.requestPermissions(this,
                arrayOf(Manifest.permission.CAMERA), REQ_CAM)
            tvStatus.text = "Granting CAMERA — Export resumes after"
            // Consent dialog is async; onRequestPermissionsResult kicks off
            // another exportAndSend() when the user approves.
            return
        } else {
            val warm = CameraWarmUp(this)
            try {
                val gotFrame = warm.openAndAwaitFirstFrame(1500)
                val dims = bridge.snapYuvTo(snapDst)   // fresh snap while open
                if (!gotFrame) tvStatus.text = "Camera warm-up timeout"
                dims
            } finally {
                // Always close — leaks the CameraDevice otherwise, and the
                // next Export fails with "camera in use".
                warm.close()
            }
        }

        // DEEP-COPY the live canvas bitmaps on the UI thread BEFORE handing to
        // the worker. A Kotlin val just captures a reference — the Activity and
        // the worker point at the SAME native pixel buffer. UI-thread handlers
        // (onActivityResult REQ_BG/DOC/FLASH/GUIDE, resetSlot, clearGuide) call
        // oldBmp?.recycle() while the worker is still drawing it → crash. Only
        // a full pixel copy (bmp.copy()) survives a concurrent recycle. The
        // copies are recycled in the finally below once the worker is done.
        fun deepCopy(src: Bitmap?): Bitmap? = src?.let {
            try { it.copy(it.config ?: Bitmap.Config.ARGB_8888, false) } catch (_: Throwable) { null }
        }
        val snapBg    = deepCopy(frameCanvas.backgroundBmp)
        val snapFlash = deepCopy(frameCanvas.flashBmp)
        val snapGuide = deepCopy(frameCanvas.guideBmp)
        // `doc` above is the live docBmp pointer; also deep-copy it so a
        // concurrent resetSlot / REQ_DOC recycle can't rip pixels out from
        // under the worker. If the copy OOMs we bail cleanly.
        val docSnap = deepCopy(doc) ?: run {
            tvStatus.text = "Doc copy OOM — free memory and retry"
            snapBg?.recycle(); snapFlash?.recycle(); snapGuide?.recycle()
            return
        }

        Thread {
            try {
                // YUV (preview): raw clean doc; hook's compositor places it at
                // ctrl_region (pushed below by setRegion from Place-Frame placement).
                // Preview source: alias docSnap (we already own it — see the
                // deep-copy above). rotatedJpeg only recycles bitmaps it
                // allocates itself (`if (bmp !== src) bmp.recycle()`) so this
                // alias survives the yuvJpeg pass; the single recycle happens
                // in the finally below on docSnap.
                val previewComposite = docSnap

                // BLOB (still capture) — two modes picked by the Res button:
                //  • SQUARE (outW == outH): myTU "square-center" mode. The
                //    Travel Union app runs IdvImagePipeline.processImage() on
                //    the captured file, which takes min(W,H), cuts a centered
                //    square (3.5% inset per side), and down-samples to
                //    <=1732×1732. Any user-placed position is discarded — only
                //    what sits inside the centered square survives. So we draw
                //    the doc CENTERED, sized to fill the square with a safety
                //    margin inside the 3.5% trim.
                //  • LANDSCAPE/PORTRAIT: render at the camera's post-JPEG-
                //    rotation PORTRAIT dims (outH × outW) for KYCs that keep
                //    the full frame and only honour EXIF orientation.
                // snapYuvTo() above already blocked until the hook dumped +
                // the daemon copied, so the snapshot's ready IF snapDims !=
                // null. Falls back to user-loaded Background / KYC Guide /
                // null (pure black) when the snap failed (no camera stream).
                val realScene = if (snapDims != null) loadSnappedRealYuv() else null
                val backdrop = realScene ?: snapBg ?: snapGuide
                // Pick compose dims: user override wins, else 3024×2268 landscape
                // (close to Pixel 7a's max BLOB — gives the hook enough pixels
                // to downscale cleanly to any smaller KYC-negotiated stream dim).
                // Use outW/outH DIRECTLY — the historic swap `capW = outH` composed
                // in portrait regardless of user choice, which the hook's resample
                // then stretched into landscape (Wise 4032×3024) and produced
                // deformed captures. User selects landscape (4032×3024) → compose
                // landscape. User selects portrait (1080×1920) → compose portrait.
                val composeW = if (outW > 0 && outH > 0) outW else 3024
                val composeH = if (outW > 0 && outH > 0) outH else 2268
                val squareMode = (composeW == composeH)
                val capW = composeW
                val capH = composeH

                // Stage aspect = what the USER visually places against on-screen,
                // NOT what the backdrop happens to be:
                //   guide loaded  → guide aspect (user aligned to its ID box)
                //   bg loaded     → bg aspect    (user aligned to the bg photo)
                //   neither       → full canvas  (user placed against full view)
                // The backdrop just fills stageRect afterwards; realScene aspect
                // doesn't drive placement. Using backdrop aspect here used to
                // shift the doc every time the snap resolution changed.
                val stageAspect: Float? = when {
                    snapGuide != null -> snapGuide.width.toFloat() / snapGuide.height
                    snapBg    != null -> snapBg.width.toFloat()    / snapBg.height
                    else              -> null
                }
                val stageRect: RectF = if (stageAspect == null) {
                    RectF(0f, 0f, capW.toFloat(), capH.toFloat())
                } else {
                    val ca = capW.toFloat() / capH
                    val sw: Float; val sh: Float
                    if (stageAspect > ca) { sw = capW.toFloat(); sh = capW / stageAspect }
                    else                  { sh = capH.toFloat(); sw = capH * stageAspect }
                    val sl = (capW - sw) / 2f
                    val st = (capH - sh) / 2f
                    RectF(sl, st, sl + sw, st + sh)
                }

                // Doc destination rect inside the capture canvas.
                val blobDst: RectF = if (squareMode) {
                    // myTU square-center: the IdvImagePipeline crop is
                    //   inset = 3.5% per side  →  survival area = 93% of
                    //   min(W,H). We draw the doc inside an inner square
                    //   smaller than that so myTU's final tile shows the
                    //   whole ID with a small background frame around it.
                    // captureFillPct controls the size (100% = fills 86% of
                    // the canvas, leaving a 7% safety margin inside the
                    // 3.5% trim). ID aspect preserved by letterbox-fit of
                    // a square-max rect to the document aspect.
                    val fill = (captureFillPct / 100f).coerceIn(0.4f, 0.95f)
                    val side = capW * 0.86f * fill
                    val docSrc = snapFlash ?: docSnap
                    val da = docSrc.width.toFloat() / docSrc.height
                    val dw: Float; val dh: Float
                    if (da >= 1f) { dw = side; dh = side / da }
                    else          { dh = side; dw = side * da }
                    val cx = capW / 2f; val cy = capH / 2f
                    RectF(cx - dw / 2f, cy - dh / 2f, cx + dw / 2f, cy + dh / 2f)
                } else {
                    // Normal: map user's Place-Frame placement through the
                    // SAME visual stage they placed against on-screen onto the
                    // capture canvas. Priority must match stageAspect above:
                    // guide > bg > full view. Previous `backdrop != null → guide`
                    // check fell through to full view whenever only a bg (no guide)
                    // was loaded, so placement normalized against the WRONG rect
                    // and shifted on export.
                    val stage: RectF = when {
                        snapGuide != null -> frameCanvas.getGuideViewRect()
                        snapBg    != null -> frameCanvas.getBackgroundViewRect()
                        else              -> RectF(0f, 0f, frameCanvas.width.toFloat(), frameCanvas.height.toFloat())
                    }
                    val vr = frameCanvas.getDocRect()
                    val nx0 = ((vr.left   - stage.left) / stage.width() ).coerceIn(0f, 1f)
                    val ny0 = ((vr.top    - stage.top)  / stage.height()).coerceIn(0f, 1f)
                    val nx1 = ((vr.right  - stage.left) / stage.width() ).coerceIn(0f, 1f)
                    val ny1 = ((vr.bottom - stage.top)  / stage.height()).coerceIn(0f, 1f)
                    RectF(
                        stageRect.left + nx0 * stageRect.width(),
                        stageRect.top  + ny0 * stageRect.height(),
                        stageRect.left + nx1 * stageRect.width(),
                        stageRect.top  + ny1 * stageRect.height()
                    )
                }

                val captureComposite = Bitmap.createBitmap(capW, capH, Bitmap.Config.ARGB_8888)
                val _cv = Canvas(captureComposite)
                _cv.drawColor(Color.BLACK)
                if (backdrop != null) {
                    _cv.drawBitmap(backdrop, null, stageRect, Paint(Paint.FILTER_BITMAP_FLAG))
                }
                val blobSrc = snapFlash ?: docSnap
                // Mirror the on-screen round-corner treatment into the exported
                // JPEG: clipPath so the corners OUTSIDE the round rect keep the
                // backdrop pixels (real scene) that were drawn just above. If
                // radius is off, draw the full rectangle normally.
                if (cropRadiusPct > 0.001f) {
                    val rPx = (cropRadiusPct * 0.01f) * blobDst.width()
                    val clipSave = _cv.save()
                    val blobPath = Path().apply { addRoundRect(blobDst, rPx, rPx, Path.Direction.CW) }
                    _cv.clipPath(blobPath)
                    _cv.drawBitmap(blobSrc, null, blobDst, Paint(Paint.FILTER_BITMAP_FLAG))
                    _cv.restoreToCount(clipSave)
                } else {
                    _cv.drawBitmap(blobSrc, null, blobDst, Paint(Paint.FILTER_BITMAP_FLAG))
                }

                // The app can't write /data/local/tmp (shell_data_file): save the
                // composites to the app's own cache and let the root daemon ingest
                // them (SET_SLOT_IMAGE). Each is pre-rotated — rotDeg for the YUV
                // preview/analysis streams, captureRot for the BLOB still.
                fun rotatedJpeg(src: Bitmap, deg: Int, name: String, noise: Boolean = false): File {
                    // Only create a new bitmap when we need to mutate (rotate or
                    // add noise). For deg==0 and no-noise we just read src for
                    // the JPEG compress and never recycle it — caller owns src.
                    // Earlier version used `Bitmap.createBitmap(src)` which on
                    // some Android builds returns the SAME bitmap; the trailing
                    // bmp.recycle() then killed the caller's Activity-owned
                    // docBmp (next onDraw crashed with "recycled bitmap").
                    var bmp: Bitmap = if (deg % 360 != 0) {
                        val m = Matrix().apply { postRotate(deg.toFloat()) }
                        Bitmap.createBitmap(src, 0, 0, src.width, src.height, m, true)
                    } else src
                    // BLOB only: bake single-encode sensor grain so the uploaded still
                    // isn't suspiciously clean. The YUV source stays CLEAN — the hook
                    // adds its own live-matched per-frame noise there (no double-noise).
                    if (noise && captureNoisePct > 0) {
                        val nb = addSensorNoise(bmp, captureNoisePct / 100f * 2.5f)
                        if (bmp !== src) bmp.recycle()
                        bmp = nb
                    }
                    val f = File(cacheDir, name)
                    FileOutputStream(f).use { bmp.compress(Bitmap.CompressFormat.JPEG, 92, it) }
                    f.setReadable(true, false)
                    // Only recycle if we own this bitmap — never the caller's.
                    if (bmp !== src) bmp.recycle()
                    return f
                }
                // Honor the preview-rotation button for both paths — the hook keeps aspect
                // right regardless, and letting the user rotate the YUV source is the only
                // way to compensate for KYC consumers that don't apply the sensor rotation.
                val yuvJpeg  = rotatedJpeg(previewComposite, rotDeg, "${slot}_yuv.jpg")             // consumer re-rotates; CLEAN
                // Apply captureRot to the BLOB — previously hard-coded 0, which
                // meant the Capture-rotation button had no effect on stills and
                // KYCs that keep the raw stream orientation got the wrong angle.
                val blobJpeg = rotatedJpeg(captureComposite, captureRot, "${slot}_blob.jpg", noise = true)  // uploaded still: grain baked in
                // The saved still (BLOB) is the only output with EXIF a verifier can
                // read; stamp it with Pixel 7a device metadata so it looks like a
                // genuine on-device capture (Bitmap.compress emits none).
                writeDeviceExif(blobJpeg, flashed = snapFlash != null)
                // previewComposite aliases docSnap — recycled in finally.
                captureComposite.recycle()

                // Daemon defaults ctrl_composite=0 (bit-identical to the legacy
                // build). Flip to ON here so our YUV + BLOB injection actually
                // runs through the region compositor + all realism passes.
                bridge.setComposite(true)
                // Content-PAD is a hook-side flag; the Export is a natural
                // place to re-push it in case the daemon/hook was restarted
                // since the user last cycled the button.
                bridge.setContentPad(contentPadPct)
                // Same reasoning for the BLOB rescale target.
                bridge.setBlobRes(outW, outH)
                val ok = bridge.setSlotImage(slot, yuvJpeg.absolutePath, blobJpeg.absolutePath)
                bridge.setSlot(slot)        // make the exported slot the active one
                // Ensure the YUV (document/liveness) injection path is enabled —
                // KYC streams are YUV_420_888, so without this a placed ID would
                // never be injected. Idempotent; no hook reload (that's START).
                bridge.setYuvWidth(640)
                // Dynamic region: convert the user's Place-Frame placement into the
                // hook's ctrl_region so the ID lands at the EXACT position they picked
                // (each KYC wants a different guide rectangle). The canvas the user
                // placed on is portrait (phone screen); the sensor is landscape rotated
                // 90° CW for display. Mapping (display → sensor, normalized 0..1):
                //   sensor_cx = 1 - display_cy     (sensor x reversed from display y)
                //   sensor_cy = display_cx         (sensor y matches display x)
                //   sensor_w  = display_h          (axes swapped)
                //   sensor_h  = display_w
                // This mapping holds when rotDeg == 0; other rotations change it, so
                // we defer non-zero rotation to a legacy static region for now.
                // Always push the user's Place-Frame placement to ctrl_region so
                // every re-Export moves/resizes the injected rect to match what
                // the user drew — harmless in full-overwrite mode (hook ignores
                // it there), critical in composite mode. The display→sensor
                // mapping is the 90° CW consumer rotation Pixel 7a applies,
                // independent of the YUV source's own rotDeg.
                run {
                    val cw = frameCanvas.width.toFloat()
                    val ch = frameCanvas.height.toFloat()
                    val vr = frameCanvas.getDocRect()
                    val dcx = ((vr.left + vr.right)  * 0.5f) / cw
                    val dcy = ((vr.top  + vr.bottom) * 0.5f) / ch
                    val dw  = vr.width()  / cw
                    val dh  = vr.height() / ch
                    val sensorCx = (1f - dcy).coerceIn(0.02f, 0.98f)
                    val sensorCy = dcx.coerceIn(0.02f, 0.98f)
                    val sensorW  = dh.coerceIn(0.1f, 0.98f)
                    val sensorH  = dw.coerceIn(0.1f, 0.98f)
                    bridge.setRegion(sensorCx, sensorCy, sensorW, sensorH)
                }
                val capNote = if (snapFlash != null) "flashed capture" else "capture ${captureRot}°"
                safeUi {
                    tvStatus.text = if (ok)
                        "✓ Slot $slot ready (preview ${rotDeg}° / $capNote) — tap START if idle"
                        else "✗ daemon error — is it connected?"
                }
            } catch (e: Throwable) {
                safeUi { tvStatus.text = "Export failed: ${e.message}" }
            } finally {
                // Recycle our worker-owned snapshots. docSnap also backs
                // previewComposite; captureComposite is already recycled above
                // on the success path (null-safe recycle is a noop).
                try { docSnap.recycle() } catch (_: Throwable) {}
                try { snapBg?.recycle() } catch (_: Throwable) {}
                try { snapFlash?.recycle() } catch (_: Throwable) {}
                try { snapGuide?.recycle() } catch (_: Throwable) {}
            }
        }.start()
    }

    // Lifecycle-safe runOnUiThread: drops the update when the Activity is
    // going away so an in-flight Export can't hit a torn-down tvStatus.
    private fun safeUi(block: () -> Unit) {
        if (isFinishing || isDestroyed) return
        runOnUiThread { if (!isFinishing && !isDestroyed) block() }
    }

    // ─── Session persistence (auto-save / restore across activity exit) ─────────
    // Writes each loaded bitmap as a JPEG into cacheDir with a slot-keyed name,
    // and records placement + capture settings into SharedPreferences. No user
    // action needed — happens on every image pick and in onPause. Restored by
    // restoreLastSession() in onCreate so the user comes back to exactly what
    // they had before switching away.
    private fun sessionFile(slotName: String, kind: String): File =
        File(cacheDir, "session_${slotName}_${kind}.jpg")

    // OOM-safe decode of a persisted session file, capped to maxDim on the long
    // edge. Returns null if the file is missing / corrupt / decode OOMs; the
    // bad file is deleted so the next launch starts clean instead of crash-
    // looping on the same input.
    private fun safeDecodeSessionFile(f: File, maxDim: Int): Bitmap? {
        if (!f.isFile) return null
        return try {
            // Bounds-only pass to compute inSampleSize cheaply (no pixel decode).
            val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
            BitmapFactory.decodeFile(f.absolutePath, bounds)
            if (bounds.outWidth <= 0 || bounds.outHeight <= 0) {
                f.delete(); return null
            }
            var sample = 1
            val longest = maxOf(bounds.outWidth, bounds.outHeight)
            while (longest / sample > maxDim) sample *= 2
            val opts = BitmapFactory.Options().apply {
                inSampleSize = sample
                inPreferredConfig = Bitmap.Config.ARGB_8888
            }
            BitmapFactory.decodeFile(f.absolutePath, opts)
        } catch (_: Throwable) {
            // OOM or decode error — drop the file so we don't retry next launch.
            try { f.delete() } catch (_: Throwable) {}
            null
        }
    }

    private fun saveBitmapToSession(slotName: String, kind: String, bmp: Bitmap?) {
        val f = sessionFile(slotName, kind)
        if (bmp == null) { if (f.exists()) f.delete(); return }
        try {
            FileOutputStream(f).use { bmp.compress(Bitmap.CompressFormat.JPEG, 92, it) }
        } catch (_: Exception) {}
    }

    private fun saveLastSession() {
        // Capture the live canvas back into the slot map so the current slot's
        // placement is up to date before writing anything out.
        if (::frameCanvas.isInitialized) saveCanvasToSlot()
        val o = JSONObject()
            .put("slot", slot)
            .put("fill", captureFillPct).put("noise", captureNoisePct)
            .put("capRot", captureRot).put("preRot", rotDeg)
            .put("outW", outW).put("outH", outH)
            .put("cropRadius", cropRadiusPct.toDouble())   // JSONObject has no put(String, float)
            .put("contentPad", contentPadPct)
            .put("lock", if (::frameCanvas.isInitialized) frameCanvas.lockAspect else true)
        val vw = if (::frameCanvas.isInitialized) frameCanvas.width.toFloat() else 0f
        val vh = if (::frameCanvas.isInitialized) frameCanvas.height.toFloat() else 0f
        val slotJson = JSONObject()
        for ((name, st) in slots) {
            val s = JSONObject()
            saveBitmapToSession(name, "doc",   st.doc)
            saveBitmapToSession(name, "bg",    st.bg)
            saveBitmapToSession(name, "flash", st.flash)
            if (st.hasRect && vw > 0f && vh > 0f && !st.rect.isEmpty) {
                s.put("rl", st.rect.left / vw).put("rt", st.rect.top / vh)
                    .put("rr", st.rect.right / vw).put("rb", st.rect.bottom / vh)
            }
            slotJson.put(name, s)
        }
        o.put("slots", slotJson)
        prefs.edit().putString("last_session", o.toString()).apply()
    }

    private fun restoreLastSession() {
        val s = prefs.getString("last_session", null) ?: return
        val o = try { JSONObject(s) } catch (_: Exception) { return }
        captureFillPct  = o.optInt("fill",   captureFillPct)
        captureNoisePct = o.optInt("noise",  captureNoisePct)
        captureRot      = o.optInt("capRot", captureRot)
        rotDeg          = o.optInt("preRot", rotDeg)
        outW            = o.optInt("outW",   outW)
        outH            = o.optInt("outH",   outH)
        cropRadiusPct   = o.optDouble("cropRadius", cropRadiusPct.toDouble()).toFloat()
        contentPadPct   = o.optInt("contentPad", contentPadPct)
        frameCanvas.lockAspect = o.optBoolean("lock", frameCanvas.lockAspect)
        val slotJson = o.optJSONObject("slots") ?: JSONObject()
        val vw = frameCanvas.width.toFloat(); val vh = frameCanvas.height.toFloat()
        for ((name, st) in slots) {
            val sj = slotJson.optJSONObject(name) ?: continue
            val docF = sessionFile(name, "doc");   val bgF = sessionFile(name, "bg")
            val flF  = sessionFile(name, "flash")
            // OOM-safe decode: the saved JPEG may be small on disk but 64 MP
            // phone photos round-trip to >200 MB ARGB_8888. inSampleSize caps
            // the max dim; Throwable catches OOM too (BitmapFactory throws
            // OutOfMemoryError, not an Exception). On failure the cache file
            // is dropped so the next launch doesn't crash-loop on the same
            // bad input.
            st.doc   = safeDecodeSessionFile(docF, maxDim = 3072)
            st.bg    = safeDecodeSessionFile(bgF,  maxDim = 2048)
            st.flash = safeDecodeSessionFile(flF,  maxDim = 3072)
            if (sj.has("rl") && vw > 0f && vh > 0f) {
                st.rect = RectF(
                    sj.optDouble("rl").toFloat() * vw, sj.optDouble("rt").toFloat() * vh,
                    sj.optDouble("rr").toFloat() * vw, sj.optDouble("rb").toFloat() * vh)
                st.hasRect = !st.rect.isEmpty
            }
        }
        // Validate stored slot name — a forward-compat value / tampered prefs /
        // migration from an older build would otherwise NPE on slots[slot]!!.
        val targetSlot = o.optString("slot", slot)
        slot = if (slots.containsKey(targetSlot)) targetSlot else "idfront"
        val st = slots[slot]!!
        frameCanvas.restoreState(st.doc, st.bg, st.flash,
                                 if (st.hasRect) st.rect else null)
        frameCanvas.docRadiusPct = cropRadiusPct
        highlightSlot()
        updateControlLabels()
        val doc = st.doc
        tvStatus.text = if (doc != null)
            "Restored last session (${slot}, doc ${doc.width}×${doc.height})"
        else "Slot $slot: no saved image"
    }

    // ─── Target profiles (save / recall capture dimensions per KYC target) ──────
    private fun targetNames(): List<String> =
        prefs.getStringSet("names", emptySet())!!.sorted()

    private fun refreshTargetSpinner(select: String? = null) {
        val names = listOf("— target —") + targetNames()
        suppressSpinner = true
        targetSpinner.adapter = ArrayAdapter(
            this, android.R.layout.simple_spinner_dropdown_item, names)
        val idx = if (select != null) names.indexOf(select).coerceAtLeast(0) else 0
        targetSpinner.setSelection(idx)
        targetSpinner.post { suppressSpinner = false }
    }

    private fun saveTarget(name: String) {
        val o = JSONObject()
            .put("fill", captureFillPct).put("capRot", captureRot).put("preRot", rotDeg).put("noise", captureNoisePct)
            .put("outW", outW).put("outH", outH).put("lock", frameCanvas.lockAspect)
        // Record where the media was placed (normalised to the canvas) → red guide on load.
        val r = frameCanvas.getDocRect()
        val vw = frameCanvas.width.toFloat(); val vh = frameCanvas.height.toFloat()
        if (vw > 0f && vh > 0f && !r.isEmpty) {
            o.put("gl", r.left / vw).put("gt", r.top / vh)
                .put("gr", r.right / vw).put("gb", r.bottom / vh)
        }
        val names = targetNames().toMutableSet().apply { add(name) }
        prefs.edit().putString("t:$name", o.toString()).putStringSet("names", names).apply()
    }

    private fun loadTarget(name: String) {
        val s = prefs.getString("t:$name", null) ?: return
        val o = JSONObject(s)
        captureFillPct = o.optInt("fill", captureFillPct)
        captureNoisePct = o.optInt("noise", captureNoisePct)
        captureRot     = o.optInt("capRot", captureRot)
        rotDeg         = o.optInt("preRot", rotDeg)
        outW           = o.optInt("outW", outW)
        outH           = o.optInt("outH", outH)
        frameCanvas.lockAspect = o.optBoolean("lock", frameCanvas.lockAspect)
        updateControlLabels()
        // Red placement guide: where the media sat when this target was saved.
        val vw = frameCanvas.width.toFloat(); val vh = frameCanvas.height.toFloat()
        frameCanvas.guideRect = if (o.has("gl") && vw > 0f && vh > 0f)
            RectF(o.optDouble("gl").toFloat() * vw, o.optDouble("gt").toFloat() * vh,
                  o.optDouble("gr").toFloat() * vw, o.optDouble("gb").toFloat() * vh)
            else null
        frameCanvas.invalidate()
        tvStatus.text = "Target '$name': fill ${captureFillPct}%, cap ${captureRot}°, " +
            "prev ${rotDeg}°, ${outW}×${outH} — red outline = saved placement"
    }

    private fun deleteSelectedTarget() {
        val n = targetSpinner.selectedItem as? String
        if (n == null || n == "— target —") { tvStatus.text = "Pick a target to delete"; return }
        val names = targetNames().toMutableSet().apply { remove(n) }
        prefs.edit().remove("t:$n").putStringSet("names", names).apply()
        refreshTargetSpinner()
        tvStatus.text = "Deleted target '$n'"
    }

    private fun saveTargetFromField() {
        val n = targetNameField.text.toString().trim()
        if (n.isEmpty()) { tvStatus.text = "Type a target name, then Save"; return }
        saveTarget(n); refreshTargetSpinner(n)
        tvStatus.text = "Saved target '$n' (fill ${captureFillPct}%, ${outW}×${outH})"
    }

    private fun updateControlLabels() {
        rotBtn.text = "Preview: ${rotDeg}°"
        captureRotBtn.text = "Capture: ${captureRot}°"
        captureFillBtn.text = "Fill: ${captureFillPct}%"
        resBtn.text = resLabel()
        lockBtn.text = if (frameCanvas.lockAspect) "Aspect: LOCK" else "Aspect: FREE"
    }

    private fun resLabel(): String =
        if (outW <= 0 || outH <= 0) "Res: AUTO" else "Res: ${outW}×${outH}"

    // Crop a full photo down to just the card: first tap shows the whole photo with a crop
    // box; second tap replaces the Document with the cropped card, so the capture-resize
    // (which sizes the Document to `fill` of the frame) then sizes the CARD, not the photo.
    private fun toggleCrop() {
        if (frameCanvas.docBmp == null) { tvStatus.text = "Load a Document photo first"; return }
        if (!frameCanvas.cropMode) {
            frameCanvas.startCrop()
            cropBtn.text = "Apply ✓"
            tvStatus.text = "Drag each of the 4 corners onto the card's corners (free quad), then tap Apply ✓"
        } else {
            val warped = frameCanvas.getCropBitmap()
            val old = frameCanvas.docBmp
            frameCanvas.cropMode = false
            cropBtn.text = "Crop"
            if (warped != null) {
                // Store the UN-rounded doc (sharp rectangle). Rounding is now
                // applied as a draw-time Canvas clipPath (see onDraw) and as
                // a bake step in exportAndSend — this sidesteps the Android
                // ARGB_8888 hasAlpha-flag dance that was leaving corners
                // opaque even when the bitmap claimed to have alpha.
                frameCanvas.setDocument(warped)
                frameCanvas.docRadiusPct = cropRadiusPct     // drives the clip
                if (old != null && old != warped) old.recycle()
                saveCanvasToSlot()
                tvStatus.text = if (cropRadiusPct < 0.05f)
                    "Warped from 8-point crop — Export ▶ or Save Crop"
                    else "Warped + R=%.2f%% (clip at render) — Export ▶".format(cropRadiusPct)
            } else tvStatus.text = "Crop failed — try again"
        }
    }

    // Compose an output frame that FILLS the screen with the WHOLE image: rotate it upright
    // (rotDeg), then COVER-scale it to the output frame (w×h) so it reaches every edge — no
    // small card on a background. `fill` is a zoom over the cover scale: 1.0 = exact cover
    // (the whole image fills, only the aspect overflow is centre-cropped); >1 zooms in, <1
    // pulls back (letterboxing onto the dark frame). Landscape output keeps a KYC still from
    // being cropped to a strip; a portrait/rotated source is uprighted via rotDeg first.
    // Per-channel sensor-like grain (shot+read approximation) → new mutable copy.
    // Single-encode (applied before JPEG compress) so there is NO double-JPEG tell.
    // Cheap xorshift Gaussian-ish: sum of three byte-slices of one state per channel.
    private fun addSensorNoise(src: Bitmap, sigma: Float): Bitmap {
        val w = src.width; val h = src.height
        val px = IntArray(w * h)
        src.getPixels(px, 0, w, 0, 0, w, h)
        var s = (w * 73856093) xor (h * 19349663) xor 0x9E3779B7.toInt()
        if (s == 0) s = 1
        val k = sigma / 127.5f
        fun nz(): Int {
            s = s xor (s shl 13); s = s xor (s ushr 17); s = s xor (s shl 5)
            val sum = ((s ushr 24) and 0xFF) + ((s ushr 16) and 0xFF) + ((s ushr 8) and 0xFF) - 382
            val v = sum * k
            return if (v < 0f) (v - 0.5f).toInt() else (v + 0.5f).toInt()
        }
        fun cl(v: Int) = if (v < 0) 0 else if (v > 255) 255 else v
        for (i in px.indices) {
            val c = px[i]
            val a = c and -0x1000000                       // preserve alpha
            val r = cl(((c ushr 16) and 0xFF) + nz())
            val g = cl(((c ushr 8) and 0xFF) + nz())
            val b = cl((c and 0xFF) + nz())
            px[i] = a or (r shl 16) or (g shl 8) or b
        }
        val out = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
        out.setPixels(px, 0, w, 0, 0, w, h)
        return out
    }

    // Round the corners of the cropped doc at a user-tunable radius. `pct`
    // is % of the bitmap width. Real ID-1 cards (ISO 7810) use 3.18/85.6 =
    // 3.71 %. 0 % returns the input unchanged (sharp corners). Returns a
    // new ARGB bitmap with alpha=0 outside the rounded rect — the downstream
    // compositor alpha-blends with the real scene at those corners so no
    // black edge is seen in the injected output; standalone PNG save shows
    // them as transparent (viewer-dependent).
    private fun roundIsoCorners(src: Bitmap, pct: Float): Bitmap {
        if (pct <= 0.001f) return src
        val radiusPx = (pct * 0.01f) * src.width.toFloat()
        val out = Bitmap.createBitmap(src.width, src.height, Bitmap.Config.ARGB_8888)
        out.eraseColor(0x00000000)              // explicit transparent fill
        out.setHasAlpha(true)                   // force the alpha flag — some
                                                // code paths flip it off after
                                                // Canvas draws and the bitmap
                                                // then renders opaque even
                                                // where alpha=0 in the pixels.
        val canvas = Canvas(out)
        val paint = Paint(Paint.ANTI_ALIAS_FLAG)
        paint.shader = BitmapShader(src, Shader.TileMode.CLAMP, Shader.TileMode.CLAMP)
        canvas.drawRoundRect(RectF(0f, 0f, src.width.toFloat(), src.height.toFloat()),
                             radiusPx, radiusPx, paint)
        out.setHasAlpha(true)                   // again — some devices clobber
                                                // it after a shader-draw pass
        return out
    }

    // Apply a subtle radial BARREL distortion to the cropped doc. Our crop
    // is perfectly rectilinear; a real photo of the same card taken through
    // a phone lens has a trace of barrel (~2-5 %) even after the ISP's own
    // correction — rectilinear edges are a splice tell when overlaid onto
    // an otherwise barrel-distorted background. k1 = 0.05 gives a very mild
    // outward bow; combine with roundIsoCorners above for an edge profile
    // that reads as "card photographed by a phone" rather than "scanned".
    private fun applyBarrelDistortion(src: Bitmap, k1: Float = 0.05f): Bitmap {
        val w = src.width; val h = src.height
        val srcPx = IntArray(w * h); src.getPixels(srcPx, 0, w, 0, 0, w, h)
        val dstPx = IntArray(w * h)
        val cx = w * 0.5f; val cy = h * 0.5f
        val norm = 1f / minOf(cx, cy)
        for (y in 0 until h) {
            val ny = (y - cy) * norm
            val ny2 = ny * ny
            for (x in 0 until w) {
                val nx = (x - cx) * norm
                val r2 = nx * nx + ny2
                val factor = 1f + k1 * r2
                val sx = (cx + (x - cx) * factor).toInt()
                val sy = (cy + (y - cy) * factor).toInt()
                dstPx[y * w + x] = if (sx in 0 until w && sy in 0 until h)
                    srcPx[sy * w + sx] else 0
            }
        }
        val out = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
        out.setPixels(dstPx, 0, w, 0, 0, w, h)
        return out
    }

    // Save the current cropped Document bitmap to /sdcard/Pictures/hal-inject/
    // as PNG (lossless, keeps the rounded alpha edges intact for re-use).
    // Notifies MediaScanner so the file shows up in Photos immediately.
    private fun saveCropToFile() {
        val doc = frameCanvas.docBmp ?: run {
            tvStatus.text = "No crop to save — tap Crop first"; return
        }
        try {
            val dir = File("/sdcard/Pictures/hal-inject")
            dir.mkdirs()
            val ts = java.text.SimpleDateFormat("yyyyMMdd-HHmmss", java.util.Locale.US)
                       .format(java.util.Date())
            val f = File(dir, "crop-$ts.png")
            FileOutputStream(f).use { doc.compress(Bitmap.CompressFormat.PNG, 100, it) }
            sendBroadcast(Intent(Intent.ACTION_MEDIA_SCANNER_SCAN_FILE, Uri.fromFile(f)))
            tvStatus.text = "Saved ${f.name} (${doc.width}×${doc.height})"
        } catch (e: Exception) {
            tvStatus.text = "Save failed: ${e.message}"
        }
    }

    // Read the hook's just-snapped PRE-composite real-sensor YUV frame from
    // /data/local/tmp/hal_inject/real_yuv.{nv21,dim} and decode it to a
    // Bitmap we can use as the BLOB backdrop. Returns null if the files
    // aren't present (hook didn't capture, snap timing missed, or compositor
    // disabled). Uses YuvImage → JPEG → BitmapFactory round-trip; the small
    // re-encode cost is one-off per Export and keeps JPEG work out of the
    // hook (see "Option B" roadmap note).
    private fun loadSnappedRealYuv(): Bitmap? = try {
        // Daemon-copied snapshot in our own cache dir (app-readable). The
        // daemon writes both files with mode 0644 so no SELinux grief.
        val nv21 = java.io.File(cacheDir, "real_yuv.nv21")
        val dim  = java.io.File(cacheDir, "real_yuv.dim")
        if (!nv21.isFile || !dim.isFile) null else {
            val wh = dim.readText().trim().split("x")
            val w = wh[0].toInt(); val h = wh[1].toInt()
            val bytes = nv21.readBytes()
            val yuv = android.graphics.YuvImage(bytes, android.graphics.ImageFormat.NV21,
                                                w, h, null)
            val baos = java.io.ByteArrayOutputStream()
            yuv.compressToJpeg(android.graphics.Rect(0, 0, w, h), 92, baos)
            BitmapFactory.decodeByteArray(baos.toByteArray(), 0, baos.size())
        }
    } catch (e: Exception) {
        null
    }

    private fun composeCaptureLandscape(imgIn: Bitmap, rotDeg: Int, fill: Float,
                                        w: Int, h: Int): Bitmap {
        val img = if (rotDeg % 360 != 0) {
            val m = Matrix().apply { postRotate(rotDeg.toFloat()) }
            Bitmap.createBitmap(imgIn, 0, 0, imgIn.width, imgIn.height, m, true)
        } else imgIn
        val out = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
        val cv = Canvas(out)
        cv.drawColor(Color.parseColor("#20242b"))
        val s = maxOf(w.toFloat() / img.width, h.toFloat() / img.height) * fill   // cover × zoom
        val dw = img.width * s; val dh = img.height * s
        val left = (w - dw) / 2f; val top = (h - dh) / 2f
        cv.drawBitmap(img, null, RectF(left, top, left + dw, top + dh),
            Paint(Paint.FILTER_BITMAP_FLAG))
        if (img !== imgIn) img.recycle()
        return out
    }

    // Stamp the saved still (BLOB) with Pixel 7a camera EXIF so a verifier that reads
    // image metadata sees a genuine-looking on-device capture — Bitmap.compress emits
    // none, and a metadata-free "camera photo" is itself a tell. Authorized KYC test.
    private fun writeDeviceExif(f: File, flashed: Boolean) {
        try {
            val now = java.text.SimpleDateFormat("yyyy:MM:dd HH:mm:ss", java.util.Locale.US)
                .format(java.util.Date())
            // androidx ExifInterface round-trips the rational tags (F-number, exposure
            // time) that the framework one silently dropped.
            ExifInterface(f.absolutePath).apply {
                setAttribute(ExifInterface.TAG_MAKE, "Google")
                setAttribute(ExifInterface.TAG_MODEL, "Pixel 7a")
                setAttribute(ExifInterface.TAG_SOFTWARE, "HDR+ 1.0.540994512zd")
                setAttribute(ExifInterface.TAG_ORIENTATION,
                    ExifInterface.ORIENTATION_NORMAL.toString())            // rotation baked into pixels
                setAttribute(ExifInterface.TAG_DATETIME, now)
                setAttribute(ExifInterface.TAG_DATETIME_ORIGINAL, now)
                setAttribute(ExifInterface.TAG_DATETIME_DIGITIZED, now)
                setAttribute(ExifInterface.TAG_F_NUMBER, "1.89")             // f/1.89 main cam (decimal form)
                setAttribute(ExifInterface.TAG_EXPOSURE_TIME, "0.01")        // 1/100 s (decimal form)
                setAttribute(ExifInterface.TAG_PHOTOGRAPHIC_SENSITIVITY, "100") // ISO
                setAttribute(ExifInterface.TAG_FOCAL_LENGTH, "681/100")      // 6.81mm
                setAttribute(ExifInterface.TAG_FOCAL_LENGTH_IN_35MM_FILM, "27")
                setAttribute(ExifInterface.TAG_WHITE_BALANCE, "0")           // auto
                setAttribute(ExifInterface.TAG_METERING_MODE, "5")           // pattern
                setAttribute(ExifInterface.TAG_EXPOSURE_PROGRAM, "2")        // normal
                setAttribute(ExifInterface.TAG_FLASH, if (flashed) "1" else "0")
                saveAttributes()
            }
        } catch (_: Exception) { /* non-fatal: the frame still injects without EXIF */ }
    }
}

// ─────────────────────────────────────────────────────────────────────────────

class FrameCanvas(context: Context) : View(context) {

    var backgroundBmp: Bitmap? = null; private set
    // Full-canvas KYC alignment screenshot — drawn edge-to-edge with alpha so the
    // doc rect can be dragged onto the KYC's guide box without resizing. Never
    // included in the export; the exportAndSend compositor path takes only the
    // doc at its placement, and the hook paints only there over the real scene.
    var guideBmp: Bitmap? = null;      private set
    var docBmp: Bitmap? = null;        private set
    // Rounded-corner radius as % of docRect width. 0 = sharp. Driven by the
    // app's "R: X%" dial. Applied as a Canvas clipPath at draw time so the
    // underlying bitmap never needs an alpha channel (sidesteps Android's
    // hasAlpha-flag quirks that were producing opaque black corners).
    var docRadiusPct: Float = 0f
    var docAlpha = 255
    var lockAspect = true   // keep the ID's native H:W ratio while resizing
    var cropMode = false    // true = Document shown full and docRect is a crop box over it
    var guideRect: RectF? = null   // red reference outline: where media was placed for a saved target

    // Flash (capture-only) document. When set, the placement locks and this image
    // is drawn see-through over the normal document as an alignment reference.
    var flashBmp: Bitmap? = null;      private set
    var docLocked = false;             private set

    private var docRect = RectF()
    private val handlePx: Float
    private val minDocPx: Float

    private val bgPaint     = Paint()
    private val docPaint    = Paint(Paint.ANTI_ALIAS_FLAG)
    private val checkerPaint = Paint()
    private val flashPaint  = Paint(Paint.ANTI_ALIAS_FLAG).apply { alpha = 140 } // see-through ref
    // KYC guide is drawn edge-to-edge at FULL opacity so Place Frame looks
    // exactly like the final captured still — no dim overlay fighting the
    // actual pixel values. The doc + handles draw on top so they stay visible.
    private val guideBmpPaint = Paint(Paint.ANTI_ALIAS_FLAG or Paint.FILTER_BITMAP_FLAG).apply { alpha = 255 }
    private val borderPaint = Paint().apply {
        style = Paint.Style.STROKE; color = Color.CYAN; strokeWidth = 2f; alpha = 180
    }
    private val handleFill   = Paint().apply { color = Color.WHITE }
    private val handleStroke = Paint().apply {
        color = Color.parseColor("#2979ff"); style = Paint.Style.STROKE; strokeWidth = 2f
    }
    private val hintPaint = Paint().apply { color = Color.DKGRAY; textSize = 14f }
    private val guidePaint = Paint().apply {
        style = Paint.Style.STROKE; color = Color.RED; strokeWidth = 3f
        pathEffect = android.graphics.DashPathEffect(floatArrayOf(14f, 10f), 0f)
    }

    private enum class Mode { NONE, DRAG, TL, TR, BL, BR }
    private var dragMode = Mode.NONE
    private var lastX = 0f; private var lastY = 0f

    // Free-quad crop: 4 INDEPENDENT corner points in view coords (phone-camera photos
    // of ID cards never land as perfect rectangles). Order is TL/TR/BR/BL (clockwise
    // from top-left). On Apply, a perspective warp maps this quad → a clean
    // ID-aspect rectangle (killing the surrounding wood/desk/paper the user shot
    // the ID on), then the result is edge-feathered so the compositor's own feather
    // has smooth material to blend into.
    // 4 corner points (absolute positions, same as before) + 4 edge "bow"
    // DELTAS from the natural midpoint between adjacent corners. Storing the
    // bow as a delta (not an absolute position) means moving a corner
    // doesn't leave an edge handle stale — the edge handle's visible
    // position is always midpoint(corners) + bow[i], recomputed on the fly.
    // A fresh startCrop sets all bows to 0 → inscribed quad == outer quad
    // → no distortion unless the user explicitly drags an edge handle onto
    // the ID's bowed edge.
    private val cropQuad    = arrayOf(PointF(), PointF(), PointF(), PointF())
    private val cropEdgeBow = arrayOf(PointF(), PointF(), PointF(), PointF())  // T, R, B, L
    private enum class QuadC { NONE, TL, TR, BR, BL, T, R, B, L, ALL }
    private var quadDrag = QuadC.NONE

    // Compute the current on-screen position of an edge handle (0=T, 1=R, 2=B, 3=L).
    private fun edgeHandlePos(idx: Int, out: PointF) {
        val a = cropQuad[idx]; val b = cropQuad[(idx + 1) % 4]
        out.set((a.x + b.x) * 0.5f + cropEdgeBow[idx].x,
                (a.y + b.y) * 0.5f + cropEdgeBow[idx].y)
    }
    private val featherCropPx: Int

    // Zoom loupe: shown while a crop corner is being dragged. The corner is
    // under the fingertip (occluded), so a circular magnifier floats above
    // the finger showing the source pixels around the corner at 3.5×, with a
    // crosshair pinned to the exact corner position. Vastly improves corner
    // placement precision for perspective crops of ID cards.
    private val loupePaintBorder = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE; color = Color.parseColor("#ff7043"); strokeWidth = 4f
    }
    private val loupeCrossPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.parseColor("#ff7043"); strokeWidth = 2f
    }
    private val loupeDotPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.WHITE }

    init {
        val dp = resources.displayMetrics.density
        handlePx  = 22f * dp
        minDocPx  = 48f * dp
        hintPaint.textSize = 14f * dp
        // Edge feather (output-space pixels) on the warped crop. ~16 px looks soft
        // without eating ID content; the final feather into the real scene is still
        // done by the native compositor (FEATHER_PX in libcam_hook).
        featherCropPx = 16
    }

    // NOTE: these setters do NOT recycle the previous bitmap — the Activity owns per-slot
    // bitmap lifecycles (so switching slots keeps the other slot's images alive) and recycles
    // explicitly on replace/reset.
    fun setBackground(bmp: Bitmap) {
        backgroundBmp = bmp; invalidate()
    }

    // KYC alignment guide — full-canvas overlay, drawn with alpha so the doc
    // rect is visible through it. Clearing frees the bitmap.
    fun setGuide(bmp: Bitmap) {
        guideBmp = bmp; invalidate()
    }
    fun clearGuide() {
        guideBmp?.recycle(); guideBmp = null; invalidate()
    }

    fun setDocument(bmp: Bitmap) {
        docBmp = bmp
        docLocked = false          // re-picking the document re-enables placement
        centerDoc()
        invalidate()
    }

    // Load the flash (capture-only) document: lock the placement so it can't shift
    // while aligning, and overlay it see-through on the normal doc as a reference.
    fun setFlashDoc(bmp: Bitmap) {
        flashBmp = bmp
        docLocked = true
        invalidate()
    }

    // Restore a slot's saved state (no recycling). rect null → centre the doc.
    fun restoreState(doc: Bitmap?, bg: Bitmap?, flash: Bitmap?, rect: RectF?) {
        cropMode = false
        backgroundBmp = bg
        docBmp = doc
        flashBmp = flash
        docLocked = flash != null
        if (rect != null && !rect.isEmpty) docRect = RectF(rect)
        else { docRect = RectF(); if (doc != null) centerDoc() }
        invalidate()
    }

    fun getDocRect(): RectF = RectF(docRect)

    // Returns the letterboxed background rect in view coordinates.
    fun getBackgroundViewRect(): RectF {
        val bg = backgroundBmp ?: return RectF(0f, 0f, width.toFloat(), height.toFloat())
        val bRatio = bg.width.toFloat() / bg.height
        val vRatio = width.toFloat() / height
        val (bw, bh) = if (bRatio > vRatio) Pair(width.toFloat(), width / bRatio)
                       else Pair(height * bRatio, height.toFloat())
        val bLeft = (width - bw) / 2f; val bTop = (height - bh) / 2f
        return RectF(bLeft, bTop, bLeft + bw, bTop + bh)
    }

    // KYC guide letterboxed to fit the view with preserved aspect — the
    // "stage" that the guide occupies on screen, and the frame the BLOB
    // capture reproduces 1:1. Returns the full view when no guide is loaded.
    fun getGuideViewRect(): RectF {
        val g = guideBmp ?: return RectF(0f, 0f, width.toFloat(), height.toFloat())
        val gRatio = g.width.toFloat() / g.height
        val vRatio = width.toFloat() / height
        val (gw, gh) = if (gRatio > vRatio) Pair(width.toFloat(), width / gRatio)
                       else Pair(height * gRatio, height.toFloat())
        val gLeft = (width - gw) / 2f
        val gTop  = (height - gh) / 2f
        return RectF(gLeft, gTop, gLeft + gw, gTop + gh)
    }

    // Document letterboxed to fill the view (used in crop mode to map view↔doc pixels).
    fun getDocumentViewRect(): RectF {
        val d = docBmp ?: return RectF(0f, 0f, width.toFloat(), height.toFloat())
        val dr = d.width.toFloat() / d.height
        val vr = width.toFloat() / height
        val (dw, dh) = if (dr > vr) Pair(width.toFloat(), width / dr)
                       else Pair(height * dr, height.toFloat())
        val l = (width - dw) / 2f; val t = (height - dh) / 2f
        return RectF(l, t, l + dw, t + dh)
    }

    // Enter crop mode: show the whole Document and seat a default 4-corner quad
    // (70% centered rect) over it. Each corner then drags independently so the
    // user can hug a non-rectangular photo of the ID (perspective / tilt).
    fun startCrop() {
        if (docBmp == null) return
        cropMode = true; docLocked = false
        val v = getDocumentViewRect()
        val mw = v.width() * 0.7f; val mh = v.height() * 0.7f
        val cx = v.centerX(); val cy = v.centerY()
        cropQuad[0].set(cx - mw / 2, cy - mh / 2)    // TL
        cropQuad[1].set(cx + mw / 2, cy - mh / 2)    // TR
        cropQuad[2].set(cx + mw / 2, cy + mh / 2)    // BR
        cropQuad[3].set(cx - mw / 2, cy + mh / 2)    // BL
        // Edge bows start at 0 — edge handles render at the natural midpoint
        // of each side; user drags them onto a curved ID edge to introduce
        // bow. Zero bow → inscribed quad == outer quad → no distortion.
        for (b in cropEdgeBow) b.set(0f, 0f)
        invalidate()
    }

    // Crop the Document from the user's 4-corner quad. Perspective-warps the quad
    // into a clean ID-aspect rectangle via setPolyToPoly, then alpha-feathers the
    // output edges so the downstream compositor has soft material to blend. The
    // output has the ID filling the ENTIRE canvas edge-to-edge (no wood/desk
    // backdrop), which is what the native compositor's doc-rect fill expects.
    fun getCropBitmap(): Bitmap? {
        val d = docBmp ?: return null
        val v = getDocumentViewRect()
        if (v.width() <= 0f || v.height() <= 0f) return null
        val sx = d.width / v.width(); val sy = d.height / v.height()

        // Inscribed quad: cropEdgeBow[0..3] are the T, R, B, L bow deltas
        // (zero by default = no bow). Each corner slides by the AVERAGE of
        // the two shared edge bows, so the inscribed quad stays INSIDE the
        // curved ID boundary — no desk bleed, no transparent pixels.
        val dT = cropEdgeBow[0]; val dR = cropEdgeBow[1]
        val dB = cropEdgeBow[2]; val dL = cropEdgeBow[3]
        val insc = arrayOf(
            PointF(cropQuad[0].x + (dT.x + dL.x) / 2f, cropQuad[0].y + (dT.y + dL.y) / 2f), // TL
            PointF(cropQuad[1].x + (dT.x + dR.x) / 2f, cropQuad[1].y + (dT.y + dR.y) / 2f), // TR
            PointF(cropQuad[2].x + (dB.x + dR.x) / 2f, cropQuad[2].y + (dB.y + dR.y) / 2f), // BR
            PointF(cropQuad[3].x + (dB.x + dL.x) / 2f, cropQuad[3].y + (dB.y + dL.y) / 2f), // BL
        )

        // Map the 4 inscribed corners → source-image coordinates.
        val srcPts = FloatArray(8)
        for (i in 0..3) {
            srcPts[i * 2]     = ((insc[i].x - v.left) * sx).coerceIn(0f, d.width.toFloat())
            srcPts[i * 2 + 1] = ((insc[i].y - v.top)  * sy).coerceIn(0f, d.height.toFloat())
        }

        // Target rectangle = ID-card aspect (ISO 7810 ID-1: 85.6 × 54 mm ≈ 1.586:1),
        // at 1920 wide so detail survives downstream JPEG → NV21.
        val outW = 1920
        val outH = 1210
        val dstPts = floatArrayOf(
            0f,            0f,
            outW.toFloat(), 0f,
            outW.toFloat(), outH.toFloat(),
            0f,            outH.toFloat()
        )

        val m = Matrix()
        if (!m.setPolyToPoly(srcPts, 0, dstPts, 0, 4)) return null

        // Warp into an ARGB canvas, then flatten any 1-px edge-rounding gaps onto
        // black so the JPEG pipeline carries a defined background. NOTE: crop-side
        // edge feather is intentionally OFF — the native compositor's 24-px feather
        // blends the warped ID into the REAL camera scene, which is more correct
        // than pre-baking a fade to black (black fights the live-ambient blend).
        val warped = Bitmap.createBitmap(outW, outH, Bitmap.Config.ARGB_8888)
        val cv = Canvas(warped)
        val p  = Paint(Paint.FILTER_BITMAP_FLAG or Paint.ANTI_ALIAS_FLAG)
        cv.save()
        cv.concat(m)
        cv.drawBitmap(d, 0f, 0f, p)
        cv.restore()
        return compositeOnBlack(warped)
    }

    // Multiply alpha by a linear ramp over the outermost `n` pixels of each edge.
    // Pixel-by-pixel so it works on any bitmap; crop canvases are small (~2 MP)
    // so the single pass is a few tens of ms at most and only happens on tap-Apply.
    private fun featherEdges(bmp: Bitmap, n: Int) {
        if (n <= 0) return
        val w = bmp.width; val h = bmp.height
        if (w < n * 2 || h < n * 2) return
        val px = IntArray(w * h)
        bmp.getPixels(px, 0, w, 0, 0, w, h)
        for (y in 0 until h) {
            val dyEdge = min(y, h - 1 - y)
            if (dyEdge >= n) {
                // Only the left/right strips need alpha adjustment on this row.
                for (x in 0 until n) {
                    val d = min(x, n - 1)
                    rampAlpha(px, y * w + x, d, n)
                }
                for (x in (w - n) until w) {
                    val d = min(w - 1 - x, n - 1)
                    rampAlpha(px, y * w + x, d, n)
                }
                continue
            }
            // This row is in the top or bottom band — process the full width.
            for (x in 0 until w) {
                val dxEdge = min(x, w - 1 - x)
                val d = min(dxEdge, dyEdge)
                if (d < n) rampAlpha(px, y * w + x, d, n)
            }
        }
        bmp.setPixels(px, 0, w, 0, 0, w, h)
    }

    private fun rampAlpha(px: IntArray, idx: Int, d: Int, n: Int) {
        val c = px[idx]
        val oldA = (c ushr 24) and 0xFF
        val newA = (oldA * (d + 1)) / n          // +1 so deepest edge isn't fully zero
        px[idx] = (newA shl 24) or (c and 0x00FFFFFF)
    }

    // Collapse alpha over solid black — downstream JPEG has no alpha channel, so
    // the alpha-feathered edge shows up as a soft dark fade. Source is recycled.
    private fun compositeOnBlack(bmp: Bitmap): Bitmap {
        val w = bmp.width; val h = bmp.height
        val out = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
        val cv = Canvas(out)
        cv.drawColor(Color.BLACK)
        cv.drawBitmap(bmp, 0f, 0f, null)
        bmp.recycle()
        return out
    }

    // Hit-test the 4 free-quad corners first, then fall back to "inside = drag whole".
    private fun hitQuadCorner(x: Float, y: Float): QuadC {
        if (near(x, y, cropQuad[0].x, cropQuad[0].y)) return QuadC.TL
        if (near(x, y, cropQuad[1].x, cropQuad[1].y)) return QuadC.TR
        if (near(x, y, cropQuad[2].x, cropQuad[2].y)) return QuadC.BR
        if (near(x, y, cropQuad[3].x, cropQuad[3].y)) return QuadC.BL
        // Edge midpoints: derived positions (midpoint + bow).
        val tmp = PointF()
        edgeHandlePos(0, tmp); if (near(x, y, tmp.x, tmp.y)) return QuadC.T
        edgeHandlePos(1, tmp); if (near(x, y, tmp.x, tmp.y)) return QuadC.R
        edgeHandlePos(2, tmp); if (near(x, y, tmp.x, tmp.y)) return QuadC.B
        edgeHandlePos(3, tmp); if (near(x, y, tmp.x, tmp.y)) return QuadC.L
        if (pointInQuad(x, y)) return QuadC.ALL
        return QuadC.NONE
    }

    // Point-in-polygon (ray cast) over the 4 quad vertices.
    private fun pointInQuad(x: Float, y: Float): Boolean {
        var inside = false; var j = 3
        for (i in 0..3) {
            val xi = cropQuad[i].x; val yi = cropQuad[i].y
            val xj = cropQuad[j].x; val yj = cropQuad[j].y
            if ((yi > y) != (yj > y) &&
                x < (xj - xi) * (y - yi) / (yj - yi + 1e-6f) + xi) inside = !inside
            j = i
        }
        return inside
    }

    private fun centerDoc() {
        val doc = docBmp ?: return
        val vw = width.toFloat().takeIf { it > 0f } ?: return
        val vh = height.toFloat().takeIf { it > 0f } ?: return
        val dw = vw * 0.5f
        val dh = dw * doc.height.toFloat() / doc.width.toFloat()
        docRect = RectF((vw - dw) / 2f, (vh - dh) / 2f, (vw + dw) / 2f, (vh + dh) / 2f)
    }

    override fun onSizeChanged(w: Int, h: Int, ow: Int, oh: Int) {
        super.onSizeChanged(w, h, ow, oh)
        if (docBmp != null && docRect.isEmpty) centerDoc()
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)

        if (cropMode) {
            val d = docBmp
            if (d != null) {
                canvas.drawColor(Color.parseColor("#13131f"))
                canvas.drawBitmap(d, null, getDocumentViewRect(), bgPaint)
                // 4-line polygon connecting the 4 free-moving corners.
                borderPaint.color = Color.parseColor("#ff7043")
                for (i in 0..3) {
                    val a = cropQuad[i]; val b = cropQuad[(i + 1) % 4]
                    canvas.drawLine(a.x, a.y, b.x, b.y, borderPaint)
                }
                // 4 corner handles + 4 edge-midpoint handles. Edge midpoints
                // let the user TRACE the ID's natural bowed edge so the
                // inscribed quad crops cleanly (see getCropBitmap).
                for (p in cropQuad) drawHandle(canvas, p.x, p.y)
                val ehTmp = PointF()
                for (i in 0..3) {
                    edgeHandlePos(i, ehTmp)
                    drawHandle(canvas, ehTmp.x, ehTmp.y)
                }
                // Zoom loupe over the active handle.
                val active = PointF()
                when (quadDrag) {
                    QuadC.TL -> active.set(cropQuad[0])
                    QuadC.TR -> active.set(cropQuad[1])
                    QuadC.BR -> active.set(cropQuad[2])
                    QuadC.BL -> active.set(cropQuad[3])
                    QuadC.T  -> edgeHandlePos(0, active)
                    QuadC.R  -> edgeHandlePos(1, active)
                    QuadC.B  -> edgeHandlePos(2, active)
                    QuadC.L  -> edgeHandlePos(3, active)
                    else     -> active.set(-1f, -1f)
                }
                if (active.x >= 0f) drawLoupe(canvas, d, active)
            }
            return
        }

        val bg = backgroundBmp
        if (bg != null) {
            val r = getBackgroundViewRect()
            canvas.drawBitmap(bg, null, r, bgPaint)
        } else {
            canvas.drawColor(Color.parseColor("#2a2a3e"))
            canvas.drawText("← tap Background to load", 24f, height / 2f, hintPaint)
        }

        // KYC alignment guide — full-canvas, no letterbox. Draws over the bg/
        // background-prompt so the user can see the KYC's guide rectangle to
        // line the doc up to. Never participates in Export.
        // KYC Guide — draw CONTAIN-FIT (preserve aspect, letterbox with dark
        // bars if guide aspect ≠ view aspect) so Place Frame is a true 1:1
        // mirror of the final capture.
        guideBmp?.let { g ->
            canvas.drawBitmap(g, null, getGuideViewRect(), guideBmpPaint)
        }

        val doc = docBmp
        if (doc != null && !docRect.isEmpty) {
            docPaint.alpha = docAlpha
            // Round-corner clip-path: corners OUTSIDE the round rect are never
            // touched by the doc draw, so the background / KYC guide drawn
            // earlier in onDraw remains visible through them → true per-pixel
            // transparency, no checker overlay. Radius = pct of docRect width,
            // matching ISO 7810 geometry proportionally.
            if (docRadiusPct > 0.001f) {
                val rPx = (docRadiusPct * 0.01f) * docRect.width()
                val clipSave = canvas.save()
                val path = Path().apply { addRoundRect(docRect, rPx, rPx, Path.Direction.CW) }
                canvas.clipPath(path)
                canvas.drawBitmap(doc, null, docRect, docPaint)
                canvas.restoreToCount(clipSave)
            } else {
                canvas.drawBitmap(doc, null, docRect, docPaint)
            }
            // See-through flash overlay: alignment reference for the capture image.
            flashBmp?.let { canvas.drawBitmap(it, null, docRect, flashPaint) }
            // Locked (flash loaded) → orange border, no handles; else cyan + handles.
            borderPaint.color = if (docLocked) Color.parseColor("#ff7043") else Color.CYAN
            canvas.drawRect(docRect, borderPaint)
            if (!docLocked) {
                drawHandle(canvas, docRect.left,  docRect.top)
                drawHandle(canvas, docRect.right, docRect.top)
                drawHandle(canvas, docRect.left,  docRect.bottom)
                drawHandle(canvas, docRect.right, docRect.bottom)
            }
        } else if (bg != null) {
            hintPaint.color = Color.LTGRAY
            canvas.drawText("← tap Document to load", 24f, height * 0.55f, hintPaint)
        }

        // Red dashed reference outline: where media was placed for the loaded target profile.
        guideRect?.let { canvas.drawRect(it, guidePaint) }
    }

    private fun drawHandle(canvas: Canvas, cx: Float, cy: Float) {
        canvas.drawCircle(cx, cy, handlePx / 2f, handleFill)
        canvas.drawCircle(cx, cy, handlePx / 2f, handleStroke)
    }

    // Circular zoom loupe centered on the active crop corner. The loupe shows
    // the document image area around (srcPt.x, srcPt.y) at 3.5×, with a
    // crosshair pinned to the exact corner. The loupe floats above the finger
    // by default, flipping below when the corner is near the top edge, and is
    // horizontally clamped so it always stays fully on screen.
    private fun drawLoupe(canvas: Canvas, docImg: Bitmap, srcPt: PointF) {
        val loupeR = handlePx * 3.6f          // ~80dp radius
        val zoom   = 3.5f
        val margin = handlePx * 2.4f
        // Prefer ABOVE the finger; flip below when near top; clamp horizontally.
        var dstY = srcPt.y - loupeR - margin
        if (dstY - loupeR < 4f) dstY = srcPt.y + loupeR + margin
        if (dstY + loupeR > height - 4f) dstY = srcPt.y - loupeR - margin
        val dstX = srcPt.x.coerceIn(loupeR + 4f, width - loupeR - 4f)

        // Clip to a circle; translate+scale+translate so (srcPt.x, srcPt.y) in
        // document view-coords maps to (dstX, dstY) with the magnification.
        canvas.save()
        val clipPath = Path().apply { addCircle(dstX, dstY, loupeR, Path.Direction.CW) }
        canvas.clipPath(clipPath)
        canvas.translate(dstX, dstY)
        canvas.scale(zoom, zoom)
        canvas.translate(-srcPt.x, -srcPt.y)
        canvas.drawColor(Color.parseColor("#13131f"))
        canvas.drawBitmap(docImg, null, getDocumentViewRect(), bgPaint)
        canvas.restore()

        // Border + crosshair + center dot — fixed on-screen (outside the transform)
        canvas.drawCircle(dstX, dstY, loupeR, loupePaintBorder)
        val ch = loupeR * 0.35f
        canvas.drawLine(dstX - ch, dstY, dstX + ch, dstY, loupeCrossPaint)
        canvas.drawLine(dstX, dstY - ch, dstX, dstY + ch, loupeCrossPaint)
        canvas.drawCircle(dstX, dstY, 3f, loupeDotPaint)
    }

    override fun onTouchEvent(ev: MotionEvent): Boolean {
        val x = ev.x; val y = ev.y
        when (ev.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                if (cropMode) {
                    quadDrag = hitQuadCorner(x, y); lastX = x; lastY = y
                    return quadDrag != QuadC.NONE
                }
                if (docLocked) return false     // placement locked while flash is loaded
                dragMode = hitTest(x, y); lastX = x; lastY = y
                return dragMode != Mode.NONE
            }
            MotionEvent.ACTION_POINTER_DOWN -> {
                // Two-finger-freeze: a second finger touching the screen
                // while a crop handle is being dragged COMMITS the handle at
                // its current position and ends the drag. The primary finger
                // can then lift without jostling the point — critical for
                // sub-pixel placement on a bowed ID edge.
                if (cropMode && quadDrag != QuadC.NONE) {
                    quadDrag = QuadC.NONE
                    invalidate(); return true
                }
                return super.onTouchEvent(ev)
            }
            MotionEvent.ACTION_MOVE -> {
                val dx = x - lastX; val dy = y - lastY
                if (cropMode && quadDrag != QuadC.NONE) {
                    // Free 8-point edit: each corner + edge midpoint moves
                    // independently; a drag from inside the quad translates
                    // all 8 points together.
                    when (quadDrag) {
                        QuadC.TL  -> { cropQuad[0].x += dx; cropQuad[0].y += dy }
                        QuadC.TR  -> { cropQuad[1].x += dx; cropQuad[1].y += dy }
                        QuadC.BR  -> { cropQuad[2].x += dx; cropQuad[2].y += dy }
                        QuadC.BL  -> { cropQuad[3].x += dx; cropQuad[3].y += dy }
                        QuadC.T   -> { cropEdgeBow[0].x += dx; cropEdgeBow[0].y += dy }
                        QuadC.R   -> { cropEdgeBow[1].x += dx; cropEdgeBow[1].y += dy }
                        QuadC.B   -> { cropEdgeBow[2].x += dx; cropEdgeBow[2].y += dy }
                        QuadC.L   -> { cropEdgeBow[3].x += dx; cropEdgeBow[3].y += dy }
                        QuadC.ALL -> for (p in cropQuad) { p.x += dx; p.y += dy }  // bow is relative, unaffected
                        else -> {}
                    }
                    lastX = x; lastY = y; invalidate(); return true
                }
                if (dragMode == Mode.DRAG) {
                    docRect.offset(dx, dy)
                } else if (dragMode != Mode.NONE) {
                    if (lockAspect && docBmp != null && !cropMode) resizeLocked(dragMode, x)
                    else resizeFree(dragMode, dx, dy)
                }
                lastX = x; lastY = y; invalidate(); return true
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                dragMode = Mode.NONE; quadDrag = QuadC.NONE; return true
            }
        }
        return super.onTouchEvent(ev)
    }

    // Free resize: each corner moves independently (can distort the ID).
    private fun resizeFree(mode: Mode, dx: Float, dy: Float) {
        when (mode) {
            Mode.TL -> {
                docRect.left = min(docRect.left + dx, docRect.right  - minDocPx)
                docRect.top  = min(docRect.top  + dy, docRect.bottom - minDocPx)
            }
            Mode.TR -> {
                docRect.right = max(docRect.right + dx, docRect.left + minDocPx)
                docRect.top   = min(docRect.top   + dy, docRect.bottom - minDocPx)
            }
            Mode.BL -> {
                docRect.left   = min(docRect.left   + dx, docRect.right - minDocPx)
                docRect.bottom = max(docRect.bottom + dy, docRect.top   + minDocPx)
            }
            Mode.BR -> {
                docRect.right  = max(docRect.right  + dx, docRect.left + minDocPx)
                docRect.bottom = max(docRect.bottom + dy, docRect.top  + minDocPx)
            }
            else -> {}
        }
    }

    // Aspect-locked resize: width follows the dragged corner's X, height derives
    // from the ID's native H:W ratio, anchored at the opposite corner.
    private fun resizeLocked(mode: Mode, x: Float) {
        val doc = docBmp ?: return
        val aspect = doc.width.toFloat() / doc.height.toFloat()   // w / h
        when (mode) {
            Mode.BR -> {   // anchor TL
                val w = max(x - docRect.left, minDocPx)
                docRect.right  = docRect.left + w
                docRect.bottom = docRect.top  + w / aspect
            }
            Mode.TL -> {   // anchor BR
                val w = max(docRect.right - x, minDocPx)
                docRect.left = docRect.right  - w
                docRect.top  = docRect.bottom - w / aspect
            }
            Mode.TR -> {   // anchor BL
                val w = max(x - docRect.left, minDocPx)
                docRect.right = docRect.left + w
                docRect.top   = docRect.bottom - w / aspect
            }
            Mode.BL -> {   // anchor TR
                val w = max(docRect.right - x, minDocPx)
                docRect.left   = docRect.right - w
                docRect.bottom = docRect.top + w / aspect
            }
            else -> {}
        }
    }

    private fun hitTest(x: Float, y: Float): Mode {
        if (docBmp == null || docRect.isEmpty) return Mode.NONE
        if (near(x, y, docRect.left,  docRect.top))    return Mode.TL
        if (near(x, y, docRect.right, docRect.top))    return Mode.TR
        if (near(x, y, docRect.left,  docRect.bottom)) return Mode.BL
        if (near(x, y, docRect.right, docRect.bottom)) return Mode.BR
        if (docRect.contains(x, y)) return Mode.DRAG
        return Mode.NONE
    }

    private fun near(x: Float, y: Float, cx: Float, cy: Float) =
        abs(x - cx) <= handlePx && abs(y - cy) <= handlePx
}

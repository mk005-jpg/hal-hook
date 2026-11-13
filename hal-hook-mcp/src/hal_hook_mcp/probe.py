"""Target-app camera-mechanics probe.

Launches (or attaches to) a target package, streams logcat for a bounded
duration, and extracts:

  * Camera2 open/configure calls (CameraManager, CameraDevice, SessionCreator)
  * Negotiated stream dimensions for YUV preview + BLOB still
  * Known KYC SDK markers (Sumsub, FaceTec, iProov, Onfido, Jumio, Incode,
    Persona, Veriff, IDnow)
  * Hook-side injection evidence from `cam_hook` (returnBuffer BLOB dims,
    write_blob rescale targets, write_yuv activity)
  * Auto-capture state-machine transitions where the SDK logs them

Returns a structured JSON report suitable for an agent to reason about:
what dims the KYC expects, what the hook delivered, and the delta between
them (the common failure mode).
"""
from __future__ import annotations

import re
import time
from collections import Counter
from dataclasses import dataclass
from typing import Optional

from .monitor import logcat_stream, pgrep
from .parsers import Event
from .transport import adb_shell

# ── SDK fingerprint patterns ────────────────────────────────────────────────
#
# Tags / substrings we grep for across logcat. Multiple vendors sometimes use
# the same class name; we track by tag+substring to reduce false matches.

SDK_MARKERS = {
    "sumsub":   [r"com\.sumsub\.sns", r"SumsubSDK", r"SNSMobileSDK"],
    "facetec":  [r"com\.facetec\.sdk", r"ZoOm", r"FaceTecSessionActivity",
                 r"FaceTecIDScan", r"libPhoenixAndroid", r"liboz_native"],
    "iproov":   [r"com\.iproov", r"com/pure/internal", r"PureLive", r"Flashmark"],
    "onfido":   [r"com\.onfido\.android\.sdk", r"OnfidoSdk", r"Onfido_Capture"],
    "jumio":    [r"com\.jumio", r"Netverify", r"JMS_", r"jumio_"],
    "incode":   [r"com\.incode\.welcome", r"incode\.OnboardingFragment"],
    "persona":  [r"com\.withpersona\.sdk2", r"withpersona/inquiry"],
    "veriff":   [r"com\.veriff\.sdk", r"VeriffSdk"],
    "idnow":    [r"de\.idnow", r"IDnowSDK", r"idnow\.Camera"],
    "fourthline": [r"com\.fourthline", r"OrcaActivity"],
    "mlkit_ocr": [r"libmlkit_google_ocr_pipeline", r"google\.mlkit\.vision\.text"],
}

# Camera2 lifecycle hints the framework emits on most devices.
CAM_PATTERNS = {
    "camera_open":      re.compile(r"CameraDevice.*(open|onOpened).*id[: =]+(\d+)", re.I),
    "session_configured": re.compile(r"CameraCaptureSession.*(onConfigured|createCaptureSession)"),
    "stream_config":    re.compile(r"(preview|still|yuv|blob|jpeg).{0,30}(\d{3,5})\s*[x×]\s*(\d{3,5})", re.I),
    "capture_request":  re.compile(r"(CaptureRequest|CaptureResult).{0,80}template[_ ]?(\d)", re.I),
}


@dataclass(slots=True)
class ProbeReport:
    """What the agent gets back. All lists are truncated to ~50 items to keep
    payload small; set `verbose=True` on the tool to opt out."""
    package: str
    duration_s: float
    camera_opened: bool
    cameraserver_pid: Optional[int]
    libcam_hook_maps: int

    sdk_detected: list[str]                 # SDK keys whose markers appeared
    sdk_evidence: dict                      # key -> first matching line

    negotiated_streams: list[dict]          # [{kind, w, h, format?, source}]
    hook_blob_returns: list[dict]           # from cam_hook returnBuffer BLOB
    hook_rescales: list[dict]               # from cam_hook write_blob RESCALE
    hook_yuv_writes: list[dict]             # from cam_hook write_yuv

    # Mismatch analysis — the "deformed capture" killer.
    dim_match: Optional[dict]               # {app_requested_WxH, hook_delivered_WxH, matches: bool}

    event_counts: dict                      # kind -> count summary
    sample_other_events: list[str]          # first 20 unparsed cam_hook body lines


def _maybe_launch(package: str) -> None:
    """Best-effort: open the target so the user doesn't have to tap it.
    Silently ignores failure — some packages don't have a launcher activity."""
    adb_shell(f"monkey -p {package} -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1",
              as_root=False, timeout=5.0)


def probe(package: str, duration_s: float = 30.0, *,
          launch: bool = True, verbose: bool = False) -> dict:
    """Run the probe. Blocking for `duration_s`."""
    if launch:
        _maybe_launch(package)
        time.sleep(1.5)  # give the splash a moment

    sdk_hits: Counter[str] = Counter()
    sdk_evidence: dict = {}
    negotiated: list[dict] = []
    blob_returns: list[dict] = []
    rescales: list[dict] = []
    yuv_writes: list[dict] = []
    other_bodies: list[str] = []
    event_counts: Counter[str] = Counter()

    pkg_patterns = [re.compile(p) for ps in SDK_MARKERS.values() for p in ps]

    def on_event(ev: Event) -> None:
        event_counts[ev.kind] += 1
        if ev.kind == "blob_return":
            blob_returns.append({"ts": ev.ts, **ev.fields})
        elif ev.kind == "blob_rescale":
            rescales.append({"ts": ev.ts, **ev.fields})
        elif ev.kind == "blob_ok":
            # add-in to rescales so agents see the write dims even when the
            # RESCALE line is gated out
            rescales.append({"ts": ev.ts, "kind": "write_ok", **ev.fields})
        elif ev.kind == "yuv_write":
            yuv_writes.append({"ts": ev.ts, **ev.fields})
        elif ev.kind == "other":
            if len(other_bodies) < 20:
                other_bodies.append(ev.body)

    # cam_hook events — bounded stream.
    hook_events = logcat_stream(duration_s, grep="cam_hook", on_event=on_event)

    # Parallel: grab a second stream slice for SDK markers (not tag-restricted).
    #   We don't start a second parallel stream (adb limits), instead we dump
    #   the full recent logcat and grep after the fact.
    # Use enough backlog to cover duration_s plus app startup.
    from .monitor import logcat_dump
    full = logcat_dump(lines=max(400, int(duration_s * 30)))
    for ln in full.splitlines():
        for sdk, pats in SDK_MARKERS.items():
            for pat in pats:
                if re.search(pat, ln):
                    if sdk not in sdk_evidence:
                        sdk_evidence[sdk] = ln.strip()[:220]
                    sdk_hits[sdk] += 1
                    break
        for kind, pat in CAM_PATTERNS.items():
            m = pat.search(ln)
            if m and kind == "stream_config":
                try:
                    w = int(m.group(2)); h = int(m.group(3))
                    label = m.group(1)
                    negotiated.append({"kind": label, "w": w, "h": h,
                                       "source": "framework_log"})
                except Exception:
                    pass

    # Deduplicate negotiated streams by (w, h, kind) first-seen.
    seen = set()
    neg_dedup = []
    for s in negotiated:
        k = (s.get("kind"), s.get("w"), s.get("h"))
        if k in seen: continue
        seen.add(k); neg_dedup.append(s)

    # Dim-mismatch analysis: cross reference the app's negotiated BLOB dims
    # against the hook's most recent rescale target.
    dim_match = None
    if blob_returns and rescales:
        app_wh = (blob_returns[-1].get("w"), blob_returns[-1].get("h"))
        last_resc = next((r for r in reversed(rescales) if "w" in r), None)
        hook_wh = (last_resc.get("tw") or last_resc.get("w"),
                   last_resc.get("th") or last_resc.get("h")) if last_resc else (None, None)
        dim_match = {
            "app_requested": f"{app_wh[0]}x{app_wh[1]}" if all(app_wh) else None,
            "hook_delivered": f"{hook_wh[0]}x{hook_wh[1]}" if all(hook_wh) else None,
            "matches": app_wh == hook_wh,
        }

    # cameraserver + hook mapping.
    from .monitor import cameraserver_hook_status
    cs = cameraserver_hook_status()

    pids = pgrep(f"^{re.escape(package)}$")
    opened = bool(pids)

    rep = ProbeReport(
        package=package,
        duration_s=duration_s,
        camera_opened=opened,
        cameraserver_pid=cs["cameraserver_pid"],
        libcam_hook_maps=cs["libcam_hook_maps"],
        sdk_detected=sorted(sdk_hits.keys()),
        sdk_evidence=sdk_evidence,
        negotiated_streams=neg_dedup[:50],
        hook_blob_returns=blob_returns[:50] if not verbose else blob_returns,
        hook_rescales=rescales[:50] if not verbose else rescales,
        hook_yuv_writes=yuv_writes[:50] if not verbose else yuv_writes,
        dim_match=dim_match,
        event_counts=dict(event_counts),
        sample_other_events=other_bodies,
    )
    # dataclass -> dict for JSON serialization in the MCP layer
    from dataclasses import asdict
    return asdict(rep)

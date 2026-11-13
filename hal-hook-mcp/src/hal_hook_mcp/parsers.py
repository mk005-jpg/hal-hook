"""Hook-logcat line parsers — turn `cam_hook` log lines into structured events.

The hook is chatty and all its diagnostics land in logcat tagged `cam_hook`.
Rather than ask agents to grep by hand, we classify common lines into typed
events so the probe/monitor tools return queryable JSON.

If a line doesn't match any pattern we yield a raw Event(kind="other", ...).
"""
from __future__ import annotations

import re
from dataclasses import asdict, dataclass
from typing import Optional

# Logcat long-format lines (adb logcat): date time pid tid level TAG: body
LOGCAT_RE = re.compile(
    r"^(?P<ts>\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\s+(?P<pid>\d+)\s+(?P<tid>\d+)\s+"
    r"(?P<level>[VDIWEF])\s+(?P<tag>[^:]+):\s+(?P<body>.*)$"
)

# Key cam_hook event shapes — see native/hook/libcam_hook.cpp for the emitters.
#   returnBuffer BLOB: fmt=33 4032x3024 numFds=2 facing_ok=1 should_inject=1 (target=0 cur=0)
#   write_blob RESCALE: src=/data/local/tmp/hal_inject/frame_idfront.jpg → 4032x3024 jpeg=1454880 B
#   write_blob OK: fd=36 jpeg_len=1454880 buf_size=18354176 copy=1454880 path=... target=4032x3024
#   write_yuv #1: 1920x1080 stride=1920 path=...
#   snap_yuv: dumped 1600x1200 real YUV to ...
#   meta fn=0 ts=... exp=... iso=... gains=...
BLOB_RETURN_RE = re.compile(
    r"returnBuffer BLOB:\s+fmt=(?P<fmt>\d+)\s+(?P<w>\d+)x(?P<h>\d+)\s+numFds=(?P<numFds>\d+)"
    r"\s+facing_ok=(?P<facing_ok>\d)\s+should_inject=(?P<should_inject>\d)"
    r"\s+\(target=(?P<target>-?\d+)\s+cur=(?P<cur>-?\d+)\)")
BLOB_RESCALE_RE = re.compile(
    r"write_blob RESCALE:\s+src=(?P<src>\S+)\s+→\s+(?P<w>\d+)x(?P<h>\d+)\s+jpeg=(?P<bytes>\d+)")
BLOB_OK_RE = re.compile(
    r"write_blob OK:\s+fd=(?P<fd>\d+)\s+jpeg_len=(?P<jpeg_len>\d+)\s+buf_size=(?P<buf_size>\d+)"
    r"\s+copy=(?P<copy>\d+)\s+path=(?P<path>\S+)\s+target=(?P<tw>\d+)x(?P<th>\d+)")
YUV_WRITE_RE = re.compile(
    r"write_yuv #(?P<seq>\d+):\s+(?P<w>\d+)x(?P<h>\d+)\s+stride=(?P<stride>\d+)\s+path=(?P<path>\S+)")
SNAP_RE = re.compile(
    r"snap_yuv: dumped (?P<w>\d+)x(?P<h>\d+)")
META_RE = re.compile(
    r"meta fn=(?P<fn>\d+)\s+ts=(?P<ts>\d+)ns\s+exp=(?P<exp>\d+)ns\s+iso=(?P<iso>\d+)")


@dataclass(slots=True)
class Event:
    """Structured hook-log event. `kind` names the pattern; `fields` holds
    the matched group dict. `ts`/`pid`/`tid` come from the logcat header."""
    kind: str                 # blob_return | blob_rescale | blob_ok | yuv_write | snap | meta | other
    ts: str                   # "MM-DD HH:MM:SS.sss" from logcat
    pid: Optional[int]
    tid: Optional[int]
    body: str                 # the full body after TAG:
    fields: dict              # match groups coerced to int where sensible

    def to_json(self) -> dict:
        d = asdict(self)
        return d


def _coerce(d: dict) -> dict:
    """Best-effort int coercion of regex groupdict values."""
    out = {}
    for k, v in d.items():
        if v is None:
            out[k] = None; continue
        try:
            out[k] = int(v)
        except (ValueError, TypeError):
            out[k] = v
    return out


def parse_line(line: str) -> Optional[Event]:
    """Parse one logcat line. Returns None for lines that don't match our TAG."""
    m = LOGCAT_RE.match(line)
    if not m:
        return None
    if m.group("tag").strip() != "cam_hook":
        return None
    body = m.group("body")
    ts = m.group("ts")
    pid = int(m.group("pid"))
    tid = int(m.group("tid"))

    for kind, pat in (
        ("blob_return", BLOB_RETURN_RE),
        ("blob_rescale", BLOB_RESCALE_RE),
        ("blob_ok", BLOB_OK_RE),
        ("yuv_write", YUV_WRITE_RE),
        ("snap", SNAP_RE),
        ("meta", META_RE),
    ):
        mm = pat.search(body)
        if mm:
            return Event(kind=kind, ts=ts, pid=pid, tid=tid, body=body,
                         fields=_coerce(mm.groupdict()))
    return Event(kind="other", ts=ts, pid=pid, tid=tid, body=body, fields={})


def parse_many(text: str) -> list[Event]:
    out: list[Event] = []
    for ln in text.splitlines():
        ev = parse_line(ln)
        if ev is not None:
            out.append(ev)
    return out

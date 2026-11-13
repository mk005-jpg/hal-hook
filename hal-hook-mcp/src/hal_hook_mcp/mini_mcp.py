"""mini_mcp — stdlib-only MCP server over stdio.

Implements just enough of the Model Context Protocol (JSON-RPC 2.0, one JSON
object per line) to expose typed tools + resources. Rewritten from scratch
because the official `mcp` SDK transitively depends on Rust-compiled wheels
(pydantic-core, rpds-py, cryptography) that pip cannot build on aarch64-
android targets, which breaks the on-device deployment story completely.

Spec reference: https://modelcontextprotocol.io/specification/2024-11-05
Methods handled: initialize, notifications/initialized, tools/list,
tools/call, resources/list, resources/read, ping. Everything else gets a
JSON-RPC method-not-found error.

Zero deps outside the stdlib. Thread-safe handlers (we never spawn, so no
race). Writes go to stdout one line at a time — stderr is for logs.
"""
from __future__ import annotations

import base64
import json
import logging
import sys
import traceback
from dataclasses import dataclass, field
from typing import Any, Callable, Optional

log = logging.getLogger(__name__)

PROTOCOL_VERSION = "2024-11-05"


@dataclass(slots=True)
class Tool:
    name: str
    description: str
    input_schema: dict
    handler: Callable[[dict], Any]   # returns anything JSON-serializable


@dataclass(slots=True)
class Resource:
    uri: str
    name: str
    description: str
    mime_type: str
    reader: Callable[[str], str]      # reader(uri) -> text


@dataclass(slots=True)
class Server:
    name: str
    version: str = "0.1.0"
    tools: dict[str, Tool] = field(default_factory=dict)
    resources: dict[str, Resource] = field(default_factory=dict)
    _initialized: bool = False

    def tool(self, name: str, description: str, input_schema: dict):
        """Decorator: @srv.tool('name', 'desc', {schema}) over fn(args)->any."""
        def wrap(fn: Callable[[dict], Any]):
            self.tools[name] = Tool(name, description, input_schema, fn)
            return fn
        return wrap

    def resource(self, uri: str, name: str, description: str, mime_type: str):
        def wrap(fn: Callable[[str], str]):
            self.resources[uri] = Resource(uri, name, description, mime_type, fn)
            return fn
        return wrap

    # ── JSON-RPC dispatch ────────────────────────────────────────────────

    def _handle(self, req: dict) -> Optional[dict]:
        """Returns a JSON-RPC response dict, or None for pure notifications."""
        rpc_id = req.get("id")
        method = req.get("method")
        params = req.get("params") or {}

        try:
            if method == "initialize":
                # Even if the client sends a newer protocolVersion, we reply
                # with ours; it's the client's job to downgrade features.
                return _ok(rpc_id, {
                    "protocolVersion": PROTOCOL_VERSION,
                    "capabilities": {
                        "tools":     {"listChanged": False},
                        "resources": {"subscribe": False, "listChanged": False},
                    },
                    "serverInfo": {"name": self.name, "version": self.version},
                })

            if method == "notifications/initialized":
                self._initialized = True
                return None  # notifications never get a reply

            if method == "ping":
                return _ok(rpc_id, {})

            if method == "tools/list":
                return _ok(rpc_id, {
                    "tools": [{
                        "name": t.name,
                        "description": t.description,
                        "inputSchema": t.input_schema,
                    } for t in self.tools.values()]
                })

            if method == "tools/call":
                name = params.get("name")
                args = params.get("arguments") or {}
                tool = self.tools.get(name)
                if tool is None:
                    return _err(rpc_id, -32601, f"unknown tool: {name!r}")
                raw = tool.handler(args)
                # Spec: content is a list of {type:"text"|"image"|"resource", ...}.
                # We default to one text block with JSON-encoded payload for
                # structured output; handlers that want plain text should
                # return a str.
                if isinstance(raw, str):
                    content = [{"type": "text", "text": raw}]
                elif isinstance(raw, list) and all(isinstance(x, dict) and "type" in x for x in raw):
                    content = raw  # already in content-block shape
                else:
                    content = [{"type": "text",
                                "text": json.dumps(raw, indent=2, default=str)}]
                return _ok(rpc_id, {"content": content, "isError": False})

            if method == "resources/list":
                return _ok(rpc_id, {
                    "resources": [{
                        "uri": r.uri, "name": r.name,
                        "description": r.description, "mimeType": r.mime_type,
                    } for r in self.resources.values()]
                })

            if method == "resources/read":
                uri = params.get("uri")
                res = self.resources.get(uri)
                if res is None:
                    return _err(rpc_id, -32602, f"unknown resource: {uri!r}")
                text = res.reader(uri)
                return _ok(rpc_id, {
                    "contents": [{
                        "uri": uri, "mimeType": res.mime_type, "text": text,
                    }]
                })

            if method and method.startswith("notifications/"):
                return None  # ignore unknown notifications silently

            return _err(rpc_id, -32601, f"method not found: {method!r}")

        except Exception as e:
            log.exception("handler %s threw", method)
            return _err(rpc_id, -32603,
                        f"internal error: {type(e).__name__}: {e}",
                        data={"traceback": traceback.format_exc()})

    # ── stdio main loop ──────────────────────────────────────────────────

    def run_stdio(self) -> None:
        """Read one JSON-RPC object per line from stdin; write one per line to
        stdout. Loops until stdin EOF. Unparseable lines get JSON-RPC parse
        errors; unrecognized methods get method-not-found."""
        # unbuffered line writes
        out = sys.stdout
        for raw in sys.stdin:
            raw = raw.strip()
            if not raw:
                continue
            try:
                req = json.loads(raw)
            except json.JSONDecodeError as e:
                _write(out, _err(None, -32700, f"parse error: {e}"))
                continue
            if isinstance(req, list):
                # Batch — spec says batch is optional; iterate.
                resps = [r for r in (self._handle(r) for r in req) if r is not None]
                if resps:
                    _write(out, resps)
                continue
            if not isinstance(req, dict) or "method" not in req:
                _write(out, _err(req.get("id") if isinstance(req, dict) else None,
                                 -32600, "invalid request"))
                continue
            resp = self._handle(req)
            if resp is not None:
                _write(out, resp)


# ── helpers ──────────────────────────────────────────────────────────────

def _ok(rpc_id: Any, result: Any) -> dict:
    return {"jsonrpc": "2.0", "id": rpc_id, "result": result}


def _err(rpc_id: Any, code: int, message: str, data: Any = None) -> dict:
    e = {"code": code, "message": message}
    if data is not None:
        e["data"] = data
    return {"jsonrpc": "2.0", "id": rpc_id, "error": e}


def _write(out, obj: Any) -> None:
    out.write(json.dumps(obj, ensure_ascii=False) + "\n")
    out.flush()


def text_content(text: str) -> list[dict]:
    """Convenience for handlers that want a plain text content block."""
    return [{"type": "text", "text": text}]


def image_content(data: bytes, mime: str = "image/jpeg") -> list[dict]:
    return [{"type": "image",
             "data": base64.b64encode(data).decode("ascii"),
             "mimeType": mime}]

#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""A fake MCP server for Prospero's tests, over standard input and
output: newline-delimited JSON-RPC, as the stdio transport has it.

It lists its tools two to a page, so that a client must follow
nextCursor, and gives instructions. Its tools:

  echo {text}       "echo: TEXT" (read-only)
  image {}          a small PNG
  big {n}           n bytes of text
  slow {seconds}    answers after a while, unless cancelled first
  fail {}           a result with isError
  refuse {}         a JSON-RPC error
  crash {}          exits at once, after a line on standard error
  structured {}     structuredContent only
  where {}          its working directory and FAKE_MCP_WORD

After notifications/initialized it sends the client a ping and a
roots/list request, and logs the responses. Every message it reads, and
those responses, is appended as a JSON line to the file FAKE_MCP_LOG
names, if set. FAKE_MCP_VERSION sets the protocol version it answers.

Run as the harness would: uv run tests/fake_mcp.py
"""

import json
import os
import struct
import sys
import threading
import time
import zlib

LOCK = threading.Lock()
CANCELLED = set()

TOOLS = [
    {"name": "echo", "description": "Repeat the text.",
     "inputSchema": {"$schema": "https://json-schema.org/draft/2020-12/schema",
                     "type": "object", "properties": {"text": {"type": "string"}},
                     "required": ["text"]},
     "annotations": {"readOnlyHint": True}},
    {"name": "image", "description": "Draw a picture.", "inputSchema": {}},
    {"name": "big", "description": "Say a lot.",
     "inputSchema": {"type": "object", "properties": {"n": {"type": "integer"}}}},
    {"name": "slow", "description": "Take a while.",
     "inputSchema": {"type": "object", "properties": {"seconds": {"type": "number"}}}},
    {"name": "fail", "description": "Fail.", "inputSchema": {"type": "object"}},
    {"name": "refuse", "description": "Refuse.", "inputSchema": {"type": "object"}},
    {"name": "crash", "description": "Crash.", "inputSchema": {"type": "object"}},
    {"name": "structured", "description": "Answer in structure.",
     "inputSchema": {"type": "object"}},
    {"name": "where", "description": "Say where it runs.",
     "inputSchema": {"type": "object"}},
]
PAGE = 2


def log(obj):
    path = os.environ.get("FAKE_MCP_LOG")
    if path:
        with LOCK, open(path, "a") as f:
            f.write(json.dumps(obj) + "\n")


def send(obj):
    with LOCK:
        sys.stdout.write(json.dumps(obj) + "\n")
        sys.stdout.flush()


def png(w, h):
    rows = b"".join(b"\x00" + bytes([200, 40, 40]) * w for _ in range(h))

    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data +
                struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff))
    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def text(t, error=False):
    r = {"content": [{"type": "text", "text": t}]}
    if error:
        r["isError"] = True
    return r


def call(rid, name, args):
    import base64
    if name == "echo":
        return text("echo: " + str(args.get("text", "")))
    if name == "image":
        return {"content": [{"type": "text", "text": "A red square."},
                            {"type": "image", "mimeType": "image/png",
                             "data": base64.b64encode(png(8, 8)).decode()}]}
    if name == "big":
        return text("x" * int(args.get("n", 100000)))
    if name == "slow":
        end = time.monotonic() + float(args.get("seconds", 5))
        while time.monotonic() < end:
            with LOCK:
                if rid in CANCELLED:
                    return None
            time.sleep(0.05)
        return text("slept")
    if name == "fail":
        return text("it went wrong", error=True)
    if name == "crash":
        sys.stderr.write("fake mcp: crashing as asked\n")
        sys.stderr.flush()
        os._exit(3)
    if name == "structured":
        return {"content": [], "structuredContent": {"answer": 42}}
    if name == "where":
        return text(os.getcwd() + "\n" + os.environ.get("FAKE_MCP_WORD", ""))
    raise KeyError(name)


def oncall(rid, params):
    name = params.get("name")
    args = params.get("arguments") or {}
    if name == "refuse":
        send({"jsonrpc": "2.0", "id": rid, "error": {"code": -32000, "message": "no, thanks"}})
        return
    try:
        r = call(rid, name, args)
    except KeyError:
        send({"jsonrpc": "2.0", "id": rid,
              "error": {"code": -32602, "message": f"unknown tool: {name}"}})
        return
    if r is not None:
        send({"jsonrpc": "2.0", "id": rid, "result": r})


def main():
    sys.stderr.write("fake mcp: started\n")
    sys.stderr.flush()
    version = os.environ.get("FAKE_MCP_VERSION", "2025-06-18")
    for line in sys.stdin:
        if not line.strip():
            continue
        msg = json.loads(line)
        log(msg)
        method = msg.get("method")
        rid = msg.get("id")
        if method is None:
            continue  # a response to our requests: logged
        if method == "initialize":
            send({"jsonrpc": "2.0", "id": rid, "result": {
                "protocolVersion": version,
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "fake", "version": "1"},
                "instructions": "Use echo to repeat things."}})
        elif method == "notifications/initialized":
            send({"jsonrpc": "2.0", "id": "s1", "method": "ping"})
            send({"jsonrpc": "2.0", "id": "s2", "method": "roots/list"})
            send({"jsonrpc": "2.0", "method": "notifications/message",
                  "params": {"level": "info", "data": "hello"}})
        elif method == "notifications/cancelled":
            with LOCK:
                CANCELLED.add(msg["params"]["requestId"])
        elif method == "tools/list":
            at = int((msg.get("params") or {}).get("cursor") or 0)
            res = {"tools": TOOLS[at:at+PAGE]}
            if at + PAGE < len(TOOLS):
                res["nextCursor"] = str(at + PAGE)
            send({"jsonrpc": "2.0", "id": rid, "result": res})
        elif method == "tools/call":
            threading.Thread(target=oncall, args=(rid, msg.get("params") or {}),
                             daemon=True).start()
        elif rid is not None:
            send({"jsonrpc": "2.0", "id": rid,
                  "error": {"code": -32601, "message": "method not found"}})
    sys.stderr.write("fake mcp: input closed\n")


if __name__ == "__main__":
    main()

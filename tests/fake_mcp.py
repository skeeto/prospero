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

With --http it serves Streamable HTTP instead, on 127.0.0.1 at a port
it picks and prints as its first line: a POST for each message, JSON
responses but for tools/call, whose response is a stream of events (a
notification, a ping to the client, then the result). initialize gives
a session id, which every later request must carry (404 otherwise), and
DELETE ends. Logged messages carry the request's headers as "_http".
More tools there:

  forget {}         answers, then forgets every session
  flaky {}          HTTP 503 the first time for its request id

With FAKE_MCP_TOKEN set, a request without "Authorization: Bearer
TOKEN" gets 401.

With --oauth too, the server at /mcp wants a token that it issued
itself, as MCP's authorization has it: a 401 names its protected
resource metadata, which names an authorization server at /as, whose
metadata gives /as/register (dynamic registration), /as/authorize
(redirecting at once to the client's redirect with a code and the
state, or with error=access_denied when FAKE_MCP_DENY is set) and
/as/token (codes, checked against their PKCE challenge, and refresh
tokens). Tokens last until the tool "expire" revokes the access
tokens. These requests are logged as {"_oauth": PATH, ...}.

Run as the harness would: uv run tests/fake_mcp.py [--http [--oauth]]
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
HTTP_TOOLS = [
    {"name": "forget", "description": "Forget the sessions.", "inputSchema": {}},
    {"name": "flaky", "description": "Fail once.", "inputSchema": {}},
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


# Streamable HTTP

SESSIONS = set()
FLAKED = set()
OAUTH = {"clients": {}, "codes": {}, "access": set(), "refresh": set(), "n": 0}
# A client registered beforehand, as FAKE_MCP_CLIENT's JSON object:
# client_id, redirect_uris, and any client_secret the token requests
# must then carry.
if os.environ.get("FAKE_MCP_CLIENT"):
    _c = json.loads(os.environ["FAKE_MCP_CLIENT"])
    OAUTH["clients"][_c["client_id"]] = _c


def serve_http():
    import http.server
    import uuid

    version = os.environ.get("FAKE_MCP_VERSION", "2025-06-18")
    token = os.environ.get("FAKE_MCP_TOKEN")
    tools = TOOLS + HTTP_TOOLS
    oauth = "--oauth" in sys.argv[1:]
    if oauth:
        tools = tools + [{"name": "expire", "description": "Expire the tokens.",
                          "inputSchema": {}}]
    base = {}

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *args):
            pass

        def reply(self, status, body=b"", ctype="application/json", extra=()):
            self.send_response(status)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            for k, v in extra:
                self.send_header(k, v)
            self.end_headers()
            self.wfile.write(body)

        def heads(self):
            return {k: self.headers.get(k) for k in
                    ("Mcp-Session-Id", "MCP-Protocol-Version", "Authorization", "Accept",
                     "Content-Type", "X-Extra")}

        def authorized(self):
            if oauth:
                got = self.headers.get("Authorization") or ""
                with LOCK:
                    ok = got.startswith("Bearer ") and got[7:] in OAUTH["access"]
                if not ok:
                    meta = base["url"] + "/.well-known/oauth-protected-resource/mcp"
                    self.reply(401, b'{"error":"invalid_token"}', extra=[
                        ("WWW-Authenticate",
                         f'Bearer error="invalid_token", resource_metadata="{meta}"')])
                return ok
            if token and self.headers.get("Authorization") != f"Bearer {token}":
                self.reply(401, b'{"error":"unauthorized"}', extra=[
                    ("WWW-Authenticate", 'Bearer realm="fake"')])
                return False
            return True

        def do_DELETE(self):
            log({"_http": self.heads(), "_delete": 1})
            with LOCK:
                SESSIONS.discard(self.headers.get("Mcp-Session-Id"))
            self.reply(200)

        def do_GET(self):
            import urllib.parse
            u = urllib.parse.urlsplit(self.path)
            q = dict(urllib.parse.parse_qsl(u.query))
            log({"_oauth": u.path, **q})
            b = base["url"]
            if not oauth:
                self.reply(404)
            elif u.path == "/.well-known/oauth-protected-resource/mcp":
                self.json(200, {"resource": b + "/mcp", "authorization_servers": [b + "/as"],
                                "scopes_supported": ["tools", "more"]})
            elif u.path == "/.well-known/oauth-authorization-server/as":
                self.json(200, {"issuer": b + "/as",
                                "authorization_endpoint": b + "/as/authorize",
                                "token_endpoint": b + "/as/token",
                                "registration_endpoint": b + "/as/register",
                                "code_challenge_methods_supported": ["S256"]})
            elif u.path == "/as/authorize":
                with LOCK:
                    c = OAUTH["clients"].get(q.get("client_id"))
                redirect = q.get("redirect_uri", "")
                if (not c or redirect not in c["redirect_uris"] or
                        q.get("response_type") != "code" or
                        q.get("code_challenge_method") != "S256" or
                        q.get("resource") != b + "/mcp" or not q.get("state")):
                    self.reply(400, b'{"error":"invalid_request"}')
                    return
                if os.environ.get("FAKE_MCP_DENY"):
                    to = redirect + "?" + urllib.parse.urlencode(
                        {"error": "access_denied", "state": q["state"]})
                else:
                    with LOCK:
                        OAUTH["n"] += 1
                        code = f"code-{OAUTH['n']}"
                        OAUTH["codes"][code] = {
                            "client": q["client_id"], "redirect": redirect,
                            "challenge": q.get("code_challenge"), "scope": q.get("scope")}
                    to = redirect + "?" + urllib.parse.urlencode(
                        {"code": code, "state": q["state"]})
                self.send_response(302)
                self.send_header("Location", to)
                self.send_header("Content-Length", "0")
                self.end_headers()
            else:
                self.reply(404)

        def json(self, status, obj):
            self.reply(status, json.dumps(obj).encode())

        def tokens(self):
            with LOCK:
                OAUTH["n"] += 1
                n = OAUTH["n"]
                OAUTH["access"].add(f"at-{n}")
            return f"at-{n}", n

        def oauthpost(self, path, body):
            import base64
            import hashlib
            import urllib.parse
            if path == "/as/register":
                req = json.loads(body)
                log({"_oauth": path, **req})
                with LOCK:
                    OAUTH["n"] += 1
                    cid = f"client-{OAUTH['n']}"
                    OAUTH["clients"][cid] = req
                self.json(201, {"client_id": cid, **req})
                return
            form = dict(urllib.parse.parse_qsl(body.decode()))
            log({"_oauth": path, **form})
            if form.get("resource") != base["url"] + "/mcp":
                self.json(400, {"error": "invalid_target"})
            elif form.get("grant_type") == "authorization_code":
                with LOCK:
                    c = OAUTH["codes"].pop(form.get("code"), None)
                v = form.get("code_verifier", "").encode()
                want = base64.urlsafe_b64encode(hashlib.sha256(v).digest()).rstrip(b"=")
                with LOCK:
                    secret = OAUTH["clients"].get(form.get("client_id"), {}).get("client_secret")
                if (not c or c["client"] != form.get("client_id") or
                        c["redirect"] != form.get("redirect_uri") or
                        (secret and secret != form.get("client_secret")) or
                        want.decode() != c["challenge"]):
                    self.json(400, {"error": "invalid_grant",
                                    "error_description": "that code is not for you"})
                    return
                access, n = self.tokens()
                with LOCK:
                    OAUTH["refresh"].add(f"rt-{n}")
                self.json(200, {"access_token": access, "token_type": "Bearer",
                                "expires_in": 3600, "refresh_token": f"rt-{n}"})
            elif form.get("grant_type") == "refresh_token":
                with LOCK:
                    known = form.get("refresh_token") in OAUTH["refresh"]
                if not known:
                    self.json(400, {"error": "invalid_grant"})
                    return
                access, _ = self.tokens()
                self.json(200, {"access_token": access, "token_type": "Bearer",
                                "expires_in": 3600})
            else:
                self.json(400, {"error": "unsupported_grant_type"})

        def do_POST(self):
            n = int(self.headers.get("Content-Length") or 0)
            if oauth and self.path.startswith("/as/"):
                self.oauthpost(self.path, self.rfile.read(n))
                return
            msg = json.loads(self.rfile.read(n))
            log({**msg, "_http": self.heads()})
            if not self.authorized():
                return
            method, rid = msg.get("method"), msg.get("id")
            sid = self.headers.get("Mcp-Session-Id")
            if method != "initialize":
                with LOCK:
                    known = sid in SESSIONS
                if not known:
                    self.reply(404 if sid else 400, b'{"error":"no session"}')
                    return
            if method is None or rid is None:
                if method == "notifications/cancelled":
                    with LOCK:
                        CANCELLED.add(msg["params"]["requestId"])
                self.reply(202)
                return
            if method == "initialize":
                sid = uuid.uuid4().hex
                with LOCK:
                    SESSIONS.add(sid)
                res = {"protocolVersion": version, "capabilities": {"tools": {}},
                       "serverInfo": {"name": "fake-http", "version": "1"},
                       "instructions": "Over HTTP."}
                self.reply(200, json.dumps({"jsonrpc": "2.0", "id": rid,
                                            "result": res}).encode(),
                           extra=[("Mcp-Session-Id", sid)])
            elif method == "tools/list":
                at = int((msg.get("params") or {}).get("cursor") or 0)
                res = {"tools": tools[at:at+PAGE]}
                if at + PAGE < len(tools):
                    res["nextCursor"] = str(at + PAGE)
                self.reply(200, json.dumps({"jsonrpc": "2.0", "id": rid,
                                            "result": res}).encode())
            elif method == "tools/call":
                self.call(rid, msg.get("params") or {})
            else:
                self.reply(200, json.dumps({"jsonrpc": "2.0", "id": rid, "error": {
                    "code": -32601, "message": "method not found"}}).encode())

        def call(self, rid, params):
            name = params.get("name")
            if name == "flaky":
                with LOCK:
                    first = rid not in FLAKED
                    FLAKED.add(rid)
                if first:
                    self.reply(503, b"busy, try again")
                    return
                r = text("steady now")
            elif name == "expire":
                with LOCK:
                    OAUTH["access"].clear()
                r = text("expired")
            elif name == "forget":
                with LOCK:
                    SESSIONS.clear()
                r = text("forgotten")
            elif name == "refuse":
                r = None
            else:
                try:
                    r = call(rid, name, params.get("arguments") or {})
                except KeyError:
                    r = None
            # A stream: a notification, a ping, then the result
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "close")
            self.end_headers()
            events = [{"jsonrpc": "2.0", "method": "notifications/progress",
                       "params": {"progressToken": 1, "progress": 1}},
                      {"jsonrpc": "2.0", "id": f"ping-{rid}", "method": "ping"}]
            if r is not None:
                events.append({"jsonrpc": "2.0", "id": rid, "result": r})
            elif name != "slow":  # a slow call is None when cancelled
                events.append({"jsonrpc": "2.0", "id": rid, "error": {
                    "code": -32000, "message": "no, thanks"}})
            try:
                for ev in events:
                    self.wfile.write(b"event: message\ndata: " + json.dumps(ev).encode() +
                                     b"\n\n")
                    self.wfile.flush()
            except OSError:
                pass
            self.close_connection = True

    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    srv.daemon_threads = True
    base["url"] = f"http://127.0.0.1:{srv.server_address[1]}"
    sys.stdout.write(f"{srv.server_address[1]}\n")
    sys.stdout.flush()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    if "--http" in sys.argv[1:]:
        serve_http()
    else:
        main()

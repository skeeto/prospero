#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""A fault-injecting fake OpenAI Chat Completions server for harness tests.

The model name picks the script: "fake:SCENARIO" or "fake:SCENARIO:NONCE",
where requests are counted per full model name, so each run of a scenario
gets a fresh count by using its own nonce. A scenario is a function of the
request number (from 0) and the parsed request, returning a generator of
actions that the handler performs in order: send the head, send chunks of
an event stream, sleep, ping, drop the connection, and so on.

Responses use chunked transfer encoding, like llama.cpp, so that a dropped
connection is a transport error rather than a short body.

Other endpoints:
  GET  /v1/models                   registered models, as a llama.cpp router
  GET  /fake/requests?model=M       requests received for M, with times
  GET  /fake/bytes?n=N              N bytes of zeros
  GET|POST /fake/stall              read the request, then never answer
  GET|POST /fake/stallbody          answer 200, send 16 bytes, then stall

Run it alone with:  uv run tests/fake_provider.py [--port N]
"""

import argparse
import json
import random
import socket
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class State:
    def __init__(self):
        self.lock = threading.Lock()
        self.counts = {}    # model -> requests so far
        self.requests = {}  # model -> [{"t": seconds, "body": request}]
        self.models = {}    # model -> listing overrides
        self.stop = threading.Event()

    def register(self, model, **listing):
        with self.lock:
            self.models[model] = listing

    def take(self, model, body):
        with self.lock:
            n = self.counts.get(model, 0)
            self.counts[model] = n + 1
            self.requests.setdefault(model, []).append(
                {"t": time.monotonic(), "body": body})
            return n

    def log(self, model):
        with self.lock:
            return list(self.requests.get(model, []))


STATE = State()


# Building blocks of a streamed response

def chunk(model, delta=None, finish=None, **extra):
    obj = {
        "id": "chatcmpl-fake",
        "object": "chat.completion.chunk",
        "created": int(time.time()),
        "model": model,
        "choices": [{"index": 0, "delta": delta or {}, "finish_reason": finish}],
    }
    obj.update(extra)
    return obj


class S:
    """Actions, as tuples, for scenario generators."""

    @staticmethod
    def head(status=200, headers=None):
        return ("head", status, headers or {})

    @staticmethod
    def data(obj):
        return ("data", obj)

    @staticmethod
    def raw(text):
        return ("raw", text)

    @staticmethod
    def sleep(seconds):
        return ("sleep", seconds)

    @staticmethod
    def ping():
        return ("raw", ": ping\n\n")

    @staticmethod
    def done():
        return ("raw", "data: [DONE]\n\n")

    @staticmethod
    def drop():
        return ("drop",)

    @staticmethod
    def stall(seconds=60):
        return ("stall", seconds)

    @staticmethod
    def error(status, obj, headers=None):
        return ("error", status, headers or {}, obj)

    @staticmethod
    def bytewise():
        return ("bytewise",)


def text(model, s, pieces=1):
    """Stream text in a few pieces."""
    step = max(1, (len(s) + pieces - 1) // pieces)
    for i in range(0, len(s), step):
        yield S.data(chunk(model, {"content": s[i:i+step]}))


def think(model, s, pieces=1):
    step = max(1, (len(s) + pieces - 1) // pieces)
    for i in range(0, len(s), step):
        yield S.data(chunk(model, {"reasoning_content": s[i:i+step]}))


def call(model, index, cid, name, args, pieces=1):
    """A tool call: the first delta names it, the rest carry arguments."""
    if not isinstance(args, str):
        args = json.dumps(args)
    yield S.data(chunk(model, {"tool_calls": [{
        "index": index, "id": cid, "type": "function",
        "function": {"name": name, "arguments": ""}}]}))
    step = max(1, (len(args) + pieces - 1) // pieces)
    for i in range(0, len(args), step):
        yield S.data(chunk(model, {"tool_calls": [{
            "index": index, "function": {"arguments": args[i:i+step]}}]}))


def finish(model, reason="stop", prompt=100, completion=10, cache_n=0):
    yield S.data(chunk(model, {}, reason))
    usage = {"prompt_tokens": prompt, "completion_tokens": completion,
             "total_tokens": prompt + completion}
    timings = {"cache_n": cache_n, "prompt_n": prompt - cache_n,
               "predicted_n": completion}
    yield S.data({"id": "chatcmpl-fake", "object": "chat.completion.chunk",
                  "model": model, "choices": [], "usage": usage,
                  "timings": timings})
    yield S.done()


def answer(model, s, pieces=3):
    yield S.head()
    yield from text(model, s, pieces)
    yield from finish(model)


def tool(model, n, name, args, pieces=2, reason="tool_calls"):
    yield S.head()
    yield from call(model, 0, f"call_{n}", name, args, pieces)
    yield from finish(model, reason)


def last_tool_result(req):
    msgs = req.get("messages", [])
    for m in reversed(msgs):
        if m.get("role") == "tool":
            return m.get("content", "")
    return ""


# Scenarios: fn(model, n, request) -> generator of actions

def sc_plain(model, n, req):
    yield from answer(model, "Hello! This is a plain answer.")


def sc_read_edit_shell(model, n, req):
    if n == 0:
        yield from tool(model, n, "read", {"path": "hello.txt"})
    elif n == 1:
        yield from tool(model, n, "edit", {"path": "hello.txt",
                                           "old_string": "world",
                                           "new_string": "there"})
    elif n == 2:
        yield from tool(model, n, "shell", {"command": "cat hello.txt"})
    else:
        yield from answer(model, "Done: hello.txt now greets there.")


def sc_bytewise(model, n, req):
    yield S.bytewise()
    if n == 0:
        yield from tool(model, n, "shell", {"command": "echo split"}, pieces=5)
    else:
        yield from answer(model, "Every byte arrived on its own.", pieces=4)


def sc_pings(model, n, req):
    # Silent prefill: pings and progress only, for longer than both the
    # idle and the progress watchdogs, then the answer.
    yield S.head()
    total = 9.0
    t0 = time.monotonic()
    k = 0
    while time.monotonic() - t0 < total:
        yield S.ping()
        if k % 2 == 0:
            done = int(1000 * (time.monotonic() - t0) / total)
            yield S.data({"choices": [], "prompt_progress": {
                "total": 1000, "cache": 0, "processed": done, "time_ms": 0}})
        k += 1
        yield S.sleep(1.0)
    yield from text(model, "Prefill finished.")
    yield from finish(model)


def sc_stall_head(model, n, req):
    if n == 0:
        yield S.stall(30)
    else:
        yield from answer(model, "Answered after a stall.")


def sc_stall_think(model, n, req):
    if n == 0:
        yield S.head()
        yield from think(model, "Let me think about this carefully.", 3)
        yield S.stall(30)
    else:
        yield S.head()
        yield from think(model, "Thinking again.")
        yield from text(model, "Recovered from a stall.")
        yield from finish(model)


def sc_stall_text(model, n, req):
    if n == 0:
        yield S.head()
        yield from text(model, "The answer is", 2)
        yield S.stall(30)
    else:
        yield from answer(model, " forty-two.")


def sc_drop_args(model, n, req):
    args = json.dumps({"command": "echo once >> count.txt"})
    if n == 0:
        yield S.head()
        yield S.data(chunk(model, {"tool_calls": [{
            "index": 0, "id": "call_a", "type": "function",
            "function": {"name": "shell", "arguments": args[:12]}}]}))
        yield S.sleep(0.2)
        yield S.drop()
    elif n == 1:
        yield from tool(model, n, "shell", args, pieces=3)
    else:
        yield from answer(model, "Counted once.")


def sc_retry_after_2(model, n, req):
    if n == 0:
        yield S.error(429, {"error": {"message": "slow down", "type": "rate_limit"}},
                      {"Retry-After": "2"})
    else:
        yield from answer(model, "Waited as asked.")


def sc_retry_after_3600(model, n, req):
    yield S.error(429, {"error": {"message": "come back later", "type": "rate_limit"}},
                  {"Retry-After": "3600"})


def sc_parse500(model, n, req):
    yield S.error(500, {"error": {
        "code": 500, "type": "server_error",
        "message": "Failed to parse tool call arguments as JSON: "
                   "[json.exception.parse_error.101] parse error"}})


def sc_503x3(model, n, req):
    if n < 3:
        yield S.error(503, {"error": {"code": 503, "message": "Loading model",
                                      "type": "unavailable_error"}})
    else:
        yield from answer(model, "Available at last.")


def sc_midstream_error(model, n, req):
    if n == 0:
        yield S.head()
        yield from think(model, "Working on it.")
        yield S.data({"error": {"code": 500, "type": "server_error",
                                "message": "Context size has been exceeded."}})
        yield S.sleep(0.2)
    else:
        yield from answer(model, "Second try worked.")


def sc_overflow(model, n, req):
    yield S.error(400, {"error": {
        "code": 400, "type": "exceed_context_size_error",
        "message": "request (40000 tokens) exceeds the available context "
                   "size (32768 tokens), try increasing it",
        "n_prompt_tokens": 40000, "n_ctx": 32768}})


def sc_malformed(model, n, req):
    if n == 0:
        yield S.head()
        yield S.raw('data: {"choices":[{"delta":{"content":"x"\n\n')
        yield S.sleep(0.2)
    else:
        yield from answer(model, "Clean this time.")


def sc_repair(model, n, req):
    if n == 0:
        # A raw newline inside a string: repairable
        yield from tool(model, n, "write",
                        '{"path": "r.txt", "content": "line1\nline2"}')
    elif n == 1:
        # Arguments double-encoded as a JSON string: repairable
        yield from tool(model, n, "write",
                        json.dumps(json.dumps({"path": "s.txt", "content": "ok"})))
    elif n == 2:
        # Truncated inside a string: never repaired
        yield from tool(model, n, "write", '{"path": "x.txt", "content": "abc')
    else:
        yield from answer(model, "Repairs checked.")


def sc_unknown_tool(model, n, req):
    if n == 0:
        yield from tool(model, n, "bash", {"command": "ls"})
    else:
        yield from answer(model, "Used the right tool.")


def sc_same_call(model, n, req):
    if n < 6:
        yield from tool(model, n, "read", {"path": "loop.txt"})
    else:
        yield from answer(model, "Stopped looping.")


def sc_cycle(model, n, req):
    if n == 0:
        yield S.head()
        yield from text(model, "Plan: " + "I will check the file now. " * 40, 60)
        yield from finish(model)
    else:
        yield from answer(model, "Checked it.")


def sc_empty(model, n, req):
    if n < 2:
        yield S.head()
        yield from finish(model, "stop", completion=1)
    else:
        yield from answer(model, "Finally something.")


def sc_length_think(model, n, req):
    if n == 0:
        yield S.head()
        yield from think(model, "Considering many options at length. " * 5, 5)
        yield from finish(model, "length")
    else:
        yield from answer(model, "Acted after the length stop.")


def sc_shell_timeout(model, n, req):
    if n == 0:
        cmd = "echo $$ > pid1; sleep 30 & echo $! > pid2; wait"
        yield from tool(model, n, "shell", {"command": cmd, "timeout": 1})
    else:
        yield from answer(model, "The command timed out.")


def sc_grandchild(model, n, req):
    if n == 0:
        cmd = "sleep 30 & echo $! > pid3; echo started"
        yield from tool(model, n, "shell", {"command": cmd})
    else:
        yield from answer(model, "The background child was cleaned up.")


def sc_outside(model, n, req):
    if n == 0:
        yield from tool(model, n, "write", {"path": "../x.txt", "content": "no"})
    elif n == 1:
        yield from tool(model, n, "write", {"path": "x.txt", "content": "yes"})
    else:
        yield from answer(model, "Wrote inside.")


def sc_crash(model, n, req):
    if n == 0:
        cmd = "echo $PPID > crashppid; echo $$ > crashpid; sleep 30"
        yield from tool(model, n, "shell", {"command": cmd})
    else:
        yield from answer(model, "Recovered after the crash.")


def sc_cancel(model, n, req):
    yield S.head()
    for i in range(100):
        yield from text(model, f"word{i} ")
        yield S.sleep(0.1)
    yield from finish(model)


def sc_cancel_tool(model, n, req):
    if n == 0:
        yield from tool(model, n, "shell", {"command": "echo $$ > cancelpid; sleep 30"})
    else:
        yield from answer(model, "Should not be reached.")


def sc_tree(model, n, req):
    # Three levels, each recording its pid, all sleeping: for cleanup.py
    if n == 0:
        cmd = ("echo $PPID > tree.ppid; echo $$ >> tree.pids; "
               "sh -c 'echo $$ >> tree.pids; "
               "sh -c \"echo \\$\\$ >> tree.pids; sleep 300\" & sleep 300' & "
               "sleep 300")
        yield from tool(model, n, "shell", {"command": cmd, "timeout": 600})
    else:
        yield from answer(model, "Tree done.")


def sc_escapee(model, n, req):
    # A child that leaves the process group (POSIX only)
    if n == 0:
        cmd = ("echo $PPID > tree.ppid; echo $$ >> tree.pids; "
               "perl -e 'use POSIX; setsid(); open(F, \">>tree.pids\"); "
               "print F \"$$\\n\"; close F; sleep 300' & sleep 300")
        yield from tool(model, n, "shell", {"command": cmd, "timeout": 600})
    else:
        yield from answer(model, "Escapee done.")


# The soak: mostly normal steps with 10% random faults

SOAK_FAULTS = ["stall_head", "stall_mid", "drop", "503", "429", "malformed",
               "error_event", "ping_wait", "stall_text", "drop_call"]


def sc_soak(model, n, req):
    rng = random.Random(hash((model, n)) ^ int(time.time() * 1000))
    if rng.random() < 0.10:
        fault = rng.choice(SOAK_FAULTS)
        if fault == "stall_head":
            yield S.stall(8)
        elif fault == "stall_mid":
            yield S.head()
            yield from think(model, "partial reasoning")
            yield S.stall(8)
        elif fault == "drop":
            yield S.head()
            yield from think(model, "about to drop")
            yield S.drop()
        elif fault == "503":
            yield S.error(503, {"error": {"message": "busy"}})
        elif fault == "429":
            yield S.error(429, {"error": {"message": "slow"}}, {"Retry-After": "1"})
        elif fault == "malformed":
            yield S.head()
            yield S.raw("data: {broken\n\n")
        elif fault == "error_event":
            yield S.head()
            yield S.data({"error": {"code": 500, "message": "boom"}})
        elif fault == "stall_text":
            # Visible text, then silence: continued, never retried
            yield S.head()
            yield from text(model, "Working on step", 2)
            yield S.stall(8)
        elif fault == "drop_call":
            # A call cut off mid-arguments: dropped, never run
            yield S.head()
            yield S.data(chunk(model, {"tool_calls": [{
                "index": 0, "id": f"call_{n}_x", "type": "function",
                "function": {"name": "shell", "arguments": '{"comm'}}]}))
            yield S.drop()
        elif fault == "ping_wait":
            yield S.head()
            for _ in range(3):
                yield S.ping()
                yield S.sleep(0.5)
            yield from text(model, "Slow but fine.")
            yield from finish(model)
        return
    # Count this turn's tool results to decide when to stop
    msgs = req.get("messages", [])
    since = 0
    for m in reversed(msgs):
        if m.get("role") == "user":
            break
        if m.get("role") == "tool":
            since += 1
    if since >= 3 or rng.random() < 0.15:
        yield from answer(model, f"Soak step {n} finished the turn.")
        return
    k = rng.randrange(4)
    if k == 0:
        yield from tool(model, n, "shell", {"command": f"echo soak {n}"})
    elif k == 1:
        yield from tool(model, n, "write", {"path": f"soak{n % 7}.txt",
                                            "content": f"step {n}\n"})
    elif k == 2:
        yield from tool(model, n, "read", {"path": f"soak{n % 7}.txt"})
    else:
        yield S.head()
        yield from think(model, "Let me look around.", 2)
        yield from call(model, 0, f"call_{n}_a", "shell", {"command": "ls"}, 2)
        yield from call(model, 1, f"call_{n}_b", "read", {"path": "."}, 2)
        yield from finish(model, "tool_calls")


SCENARIOS = {name[3:]: fn for name, fn in globals().items()
             if name.startswith("sc_")}


# The server

class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "fake-provider/1"

    def log_message(self, fmt, *args):
        pass

    def setup(self):
        super().setup()
        self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n else b""

    def send_json(self, status, obj, headers=None):
        data = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)
        self.close_connection = True

    def wait(self, seconds):
        STATE.stop.wait(seconds)

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(url.query)
        if url.path in ("/v1/models", "/models"):
            self.send_json(200, models())
        elif url.path == "/fake/requests":
            self.send_json(200, STATE.log(q.get("model", [""])[0]))
        elif url.path == "/fake/bytes":
            n = int(q.get("n", ["0"])[0])
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(n))
            self.send_header("Connection", "close")
            self.end_headers()
            block = bytes(1 << 20)
            while n > 0:
                k = min(n, len(block))
                self.wfile.write(block[:k])
                n -= k
            self.close_connection = True
        elif url.path in ("/fake/stall", "/fake/stallbody"):
            self.stall(url.path)
        else:
            self.send_json(404, {"error": {"message": "not found"}})

    def stall(self, path):
        if path == "/fake/stallbody":
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", "1000000")
            self.end_headers()
            self.wfile.write(b"0123456789abcdef")
            self.wfile.flush()
        self.wait(60)
        self.close_connection = True

    def do_POST(self):
        url = urllib.parse.urlparse(self.path)
        raw = self.body()
        if url.path in ("/fake/stall", "/fake/stallbody"):
            self.stall(url.path)
            return
        if url.path not in ("/v1/chat/completions", "/chat/completions"):
            self.send_json(404, {"error": {"message": "not found"}})
            return
        try:
            req = json.loads(raw)
        except ValueError:
            self.send_json(400, {"error": {"message": "invalid JSON body"}})
            return
        model = req.get("model", "")
        parts = model.split(":")
        fn = SCENARIOS.get(parts[1]) if len(parts) > 1 and parts[0] == "fake" else None
        if not fn:
            self.send_json(404, {"error": {"message": f"unknown model {model}"}})
            return
        n = STATE.take(model, req)
        self.perform(fn(model, n, req))

    def perform(self, actions):
        chunked = False
        bytewise = False
        try:
            for a in actions:
                kind = a[0]
                if kind == "bytewise":
                    bytewise = True
                elif kind == "head":
                    self.send_response(a[1])
                    self.send_header("Content-Type", "text/event-stream")
                    self.send_header("Cache-Control", "no-cache")
                    self.send_header("Transfer-Encoding", "chunked")
                    self.send_header("Connection", "close")
                    for k, v in a[2].items():
                        self.send_header(k, v)
                    self.end_headers()
                    self.wfile.flush()
                    chunked = True
                elif kind in ("data", "raw"):
                    s = a[1] if kind == "raw" else "data: " + json.dumps(a[1]) + "\n\n"
                    data = s.encode()
                    pieces = [data[i:i+1] for i in range(len(data))] if bytewise else [data]
                    for p in pieces:
                        self.wfile.write(b"%x\r\n%s\r\n" % (len(p), p))
                        self.wfile.flush()
                elif kind == "sleep":
                    self.wait(a[1])
                elif kind == "stall":
                    self.wait(a[1])
                    self.close_connection = True
                    return
                elif kind == "drop":
                    self.close_connection = True
                    self.connection.shutdown(socket.SHUT_RDWR)
                    return
                elif kind == "error":
                    self.send_json(a[1], a[3], a[2])
                    return
            if chunked:
                self.wfile.write(b"0\r\n\r\n")
                self.wfile.flush()
            self.close_connection = True
        except (BrokenPipeError, ConnectionResetError, OSError):
            self.close_connection = True


def models():
    data = []
    with STATE.lock:
        listing = dict(STATE.models)
    for model, over in listing.items():
        entry = {
            "id": model, "object": "model", "owned_by": "llamacpp",
            "status": {"value": "loaded",
                       "args": ["llama-server", "--ctx-size", "65536",
                                "--kv-unified", "--parallel", "4"]},
            "meta": {"n_ctx": 65536, "n_ctx_train": 131072},
        }
        entry.update(over)
        data.append(entry)
    return {"object": "list", "data": data}


class Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def start(host="127.0.0.1", port=0):
    """Start in a background thread; returns (server, base URL)."""
    srv = Server((host, port), Handler)
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    return srv, f"http://{host}:{srv.server_address[1]}"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--register", action="append", default=[],
                    help="list a model in /v1/models")
    args = ap.parse_args()
    for m in args.register:
        STATE.register(m)
    srv, url = start(args.host, args.port)
    print(url, flush=True)
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        STATE.stop.set()
        srv.shutdown()


if __name__ == "__main__":
    main()

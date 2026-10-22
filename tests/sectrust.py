#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""macOS trust evaluation of https servers (src/http_httplib.cpp).

The system (Security.framework) decides whether to trust a server's
chain, and the host name check stands. With certificates that openssl
makes here, in a temporary directory, and that are never installed
anywhere (no keychain, no trust setting), through prospero_edgetest
(src/main_edgetest.cpp):

- the evaluation alone: a chain is trusted when its root is an anchor,
  which a test-only call puts in place of the system's anchors, and not
  otherwise: without it (the system's own, which know nothing of it),
  under another root, with its intermediate missing, or for another
  name;
- the same through TLS handshakes with servers here, by name and by
  address.

With --net, real servers too: good ones answer (api.anthropic.com's
401 needs no key), and badssl.com's bad chains fail, saying why.

  uv run tests/sectrust.py --edgetest build/prospero_edgetest [--net]
"""

import argparse
import http.server
import os
import random
import shutil
import ssl
import subprocess
import sys
import tempfile
import threading

CONFIG = """\
[req]
distinguished_name = dn
prompt = no
[dn]
CN = unused
[root]
basicConstraints = critical, CA:TRUE
keyUsage = critical, keyCertSign, cRLSign
subjectKeyIdentifier = hash
[inter]
basicConstraints = critical, CA:TRUE, pathlen:0
keyUsage = critical, keyCertSign, cRLSign
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid
[leaf]
basicConstraints = critical, CA:FALSE
keyUsage = critical, digitalSignature
extendedKeyUsage = serverAuth
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid
subjectAltName = @names
[names]
"""


class Failure(Exception):
    pass


class Certs:
    """A root, an intermediate under it, and leaves under that, made by
    openssl in dir; another root stands for a stranger."""

    def __init__(self, dir):
        self.dir = dir
        self.openssl = shutil.which("openssl")
        if not self.openssl:
            raise Failure("no openssl to make certificates with")
        self.tag = f"{random.randrange(1 << 32):08x}"
        self.root = self.ca("root", None)
        self.other = self.ca("other", None)
        self.inter = self.ca("inter", "root")

    def path(self, name):
        return os.path.join(self.dir, name)

    def run(self, *args):
        r = subprocess.run([self.openssl, *args], capture_output=True, text=True)
        if r.returncode:
            raise Failure(f"openssl {args[0]}: {r.stderr.strip()[:400]}")

    def key(self, name):
        self.run("ecparam", "-name", "prime256v1", "-genkey", "-noout",
                 "-out", self.path(f"{name}.key"))

    def config(self, names=()):
        alt = "".join(f"{'IP' if n[0].isdigit() else 'DNS'}.{i} = {n}\n"
                      for i, n in enumerate(names, 1))
        path = self.path("openssl.cnf")
        with open(path, "w") as f:
            f.write(CONFIG + (alt or "DNS.1 = unused.invalid\n"))
        return path

    def sign(self, name, issuer, ext, names=()):
        """A certificate for name's key, issued by issuer, valid 30 days."""
        cnf = self.config(names)
        subj = f"/CN=Prospero test {name} {self.tag}"
        csr = self.path(f"{name}.csr")
        self.run("req", "-new", "-key", self.path(f"{name}.key"), "-subj", subj,
                 "-config", cnf, "-out", csr)
        serial = str(random.randrange(1, 1 << 62))
        self.run("x509", "-req", "-in", csr, "-CA", self.path(f"{issuer}.pem"),
                 "-CAkey", self.path(f"{issuer}.key"), "-set_serial", serial,
                 "-days", "30", "-sha256", "-extfile", cnf, "-extensions", ext,
                 "-out", self.path(f"{name}.pem"))

    def ca(self, name, issuer):
        self.key(name)
        if issuer:
            self.sign(name, issuer, "inter")
        else:
            subj = f"/CN=Prospero test {name} {self.tag}"
            self.run("req", "-new", "-x509", "-key", self.path(f"{name}.key"),
                     "-subj", subj, "-days", "30", "-sha256", "-config", self.config(),
                     "-extensions", "root", "-out", self.path(f"{name}.pem"))
        return self.path(f"{name}.pem")

    def leaf(self, name, names):
        """A leaf for names under the intermediate: its key, and its chain
        as a server sends it (leaf, intermediate) and without the latter."""
        self.key(name)
        self.sign(name, "inter", "leaf", names)
        chain = self.path(f"{name}-chain.pem")
        with open(chain, "w") as f:
            for p in (self.path(f"{name}.pem"), self.inter):
                with open(p) as g:
                    f.write(g.read())
        return self.path(f"{name}.key"), chain, self.path(f"{name}.pem")


class Quiet(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Length", "2")
        self.end_headers()
        self.wfile.write(b"ok")

    def log_message(self, *args):
        pass


def serve(key, chain):
    """An https server on a port of 127.0.0.1 with this chain."""
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(chain, key)
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Quiet)
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    srv.handle_error = lambda *args: None  # handshakes that clients refuse
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


class T:
    def __init__(self, edgetest):
        self.edgetest = edgetest
        self.failed = 0
        self.checks = 0

    def edge(self, *args):
        r = subprocess.run([self.edgetest, *args], capture_output=True, text=True,
                           timeout=120)
        if r.returncode not in (0, 1):
            raise Failure(f"prospero_edgetest {' '.join(args)}: "
                          f"exit {r.returncode}: {r.stderr.strip()}")
        return r.stdout.strip()

    def expect(self, what, out, ok, says=()):
        """out begins as success does (ok) or not, and says each of says."""
        self.checks += 1
        good = out.startswith(("trusted", "status")) == ok and \
            all(s in out for s in says)
        if not good:
            self.failed += 1
        print(f"{'ok' if good else 'FAIL':4} {what}: {out}", flush=True)


def local(t, dir):
    c = Certs(dir)
    key, chain, alone = c.leaf("leaf", ["prospero.test", "localhost", "127.0.0.1"])
    wkey, wchain, _ = c.leaf("wrong", ["elsewhere.test"])
    root, other = c.root, c.other

    # The evaluation alone
    t.expect("anchored root", t.edge("trust", "prospero.test", chain, root), True)
    t.expect("the system's anchors", t.edge("trust", "prospero.test", chain), False,
             ["not trusted"])
    t.expect("another root", t.edge("trust", "prospero.test", chain, other), False)
    t.expect("no intermediate", t.edge("trust", "prospero.test", alone, root), False)
    t.expect("another name", t.edge("trust", "other.test", chain, root), False)
    t.expect("an address", t.edge("trust", "127.0.0.1", chain, root), True)

    # Through handshakes, by name and by address
    srv = serve(key, chain)
    wsrv = serve(wkey, wchain)
    try:
        port = srv.server_address[1]
        url = f"https://localhost:{port}/x"
        t.expect("handshake, anchored root", t.edge("get", url, root), True, ["200"])
        t.expect("handshake, address", t.edge("get", f"https://127.0.0.1:{port}/", root),
                 True, ["200"])
        t.expect("handshake, the system's anchors", t.edge("get", url), False,
                 ["certificate is not trusted"])
        t.expect("handshake, another root", t.edge("get", url, other), False,
                 ["not trusted"])
        wurl = f"https://localhost:{wsrv.server_address[1]}/"
        t.expect("handshake, another name", t.edge("get", wurl, root), False,
                 ["not for localhost"])
    finally:
        srv.shutdown()
        wsrv.shutdown()


NET = [
    ("https://api.anthropic.com/v1/models", True, ["401"]),
    ("https://example.com/", True, ["200"]),
    ("https://badssl.com/", True, ["200"]),
    ("https://expired.badssl.com/", False, ["expired"]),
    ("https://self-signed.badssl.com/", False, ["not trusted"]),
    ("https://wrong.host.badssl.com/", False, ["not for wrong.host.badssl.com"]),
    ("https://untrusted-root.badssl.com/", False, ["not trusted"]),
]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--edgetest", required=True, help="prospero_edgetest")
    ap.add_argument("--net", action="store_true", help="real servers too")
    args = ap.parse_args()

    t = T(os.path.abspath(args.edgetest))
    dir = tempfile.mkdtemp(prefix="prospero-sectrust-")
    try:
        local(t, dir)
        if args.net:
            for url, ok, says in NET:
                t.expect(url, t.edge("get", url), ok, says)
    except Failure as e:
        print(f"FAIL {e}")
        return 1
    finally:
        shutil.rmtree(dir, ignore_errors=True)
    print(f"{t.checks - t.failed}/{t.checks} trust checks passed")
    return 1 if t.failed else 0


if __name__ == "__main__":
    sys.exit(main())

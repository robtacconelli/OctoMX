#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# [upd-1] A local stand-in for GitHub in the self-update end-to-end test (tests/update_e2e/e2e.sh):
#   GET /repos/robtacconelli/OctoMX/releases/latest  the releases/latest JSON of package B
#   GET /dl/<asset>       302 to /assets/<asset> (GitHub's download URLs redirect too)
#   GET /assets/<asset>   the files of B's dist folder (Range: bytes=N- answered with 206)
# --mode good | tampered (one byte of the archive flipped, SHA256SUMS + .sig unchanged) | unsigned
# (no SHA256SUMS.sig asset). Listens on 127.0.0.1 only, logs every request to --log.
import argparse
import json
import os
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ap = argparse.ArgumentParser()
ap.add_argument("--port", type=int, required=True)
ap.add_argument("--dist", required=True)
ap.add_argument("--tag", required=True)
ap.add_argument("--mode", default="good", choices=["good", "tampered", "unsigned"])
ap.add_argument("--log", required=True)
args = ap.parse_args()

dist = os.path.realpath(args.dist)
names = sorted(n for n in os.listdir(dist) if n.startswith("OctoMX-") and (n.endswith(".zip") or n.endswith(".tar.xz")))
names += ["SHA256SUMS"] + ([] if args.mode == "unsigned" else ["SHA256SUMS.sig"])
base = "http://127.0.0.1:%d" % args.port


def body_of(name):
    with open(os.path.join(dist, name), "rb") as f:
        data = f.read()
    if args.mode == "tampered" and name != "SHA256SUMS" and name != "SHA256SUMS.sig":
        b = bytearray(data)
        b[len(b) // 2] ^= 0x01
        data = bytes(b)
    return data


release = {
    "tag_name": args.tag,
    "name": "OctoMX " + args.tag,
    "draft": False,
    "prerelease": False,
    "html_url": "https://github.com/robtacconelli/OctoMX/releases/tag/" + args.tag,
    "body": "## What's new in %s\n\n* Updates that install themselves (end-to-end test release)\n* Signed with the release key\n" % args.tag,
    "assets": [{"name": n, "browser_download_url": base + "/dl/" + n, "size": os.path.getsize(os.path.join(dist, n))} for n in names],
}


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *a):
        with open(args.log, "a") as f:
            f.write("%s %s\n" % (self.command, self.path) + ("  range " + self.headers.get("Range") + "\n" if self.headers.get("Range") else ""))

    def send(self, code, body=b"", ctype="application/octet-stream", extra=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        p = self.path
        if p == "/repos/robtacconelli/OctoMX/releases/latest":
            self.send(200, json.dumps(release).encode(), "application/json")
        elif p.startswith("/dl/") and p[4:] in names:
            self.send(302, b"", extra={"Location": "/assets/" + p[4:]})
        elif p.startswith("/assets/") and p[8:] in names:
            data = body_of(p[8:])
            rng = self.headers.get("Range", "")
            if rng.startswith("bytes=") and rng.endswith("-") and rng[6:-1].isdigit() and int(rng[6:-1]) < len(data):
                start = int(rng[6:-1])
                self.send(206, data[start:], extra={"Content-Range": "bytes %d-%d/%d" % (start, len(data) - 1, len(data))})
            else:
                self.send(200, data)
        else:
            self.send(404, b"not found")


srv = ThreadingHTTPServer(("127.0.0.1", args.port), H)
print("fake release server on %s, %s, mode %s, assets %s" % (base, args.tag, args.mode, " ".join(names)), flush=True)
try:
    srv.serve_forever()
except KeyboardInterrupt:
    pass

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# [up-1] A throttled HTTP/1.1 file server with byte ranges, for the upload instance tests: a local
# source the first instance downloads from slowly, so it stays partial for a while.
#   slow_http.py <file> <port> <bytes per second>
# Serves <file> at /<file name> on 127.0.0.1 only. Prints "ready <port>" when listening.
import os
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PATH, PORT, RATE = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
NAME = '/' + os.path.basename(PATH)
SIZE = os.path.getsize(PATH)
LOCK = threading.Lock()
BUCKET = [0.0, time.monotonic()]   # tokens, last refill: one shared rate for all connections


def take(n):
    while True:
        with LOCK:
            now = time.monotonic()
            BUCKET[0] = min(RATE / 4.0, BUCKET[0] + (now - BUCKET[1]) * RATE)
            BUCKET[1] = now
            if BUCKET[0] >= n:
                BUCKET[0] -= n
                return
        time.sleep(0.02)


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def _range(self):
        r = self.headers.get('Range')
        if not r or not r.startswith('bytes='):
            return 0, SIZE, False
        a, _, b = r[6:].split(',')[0].strip().partition('-')
        if a == '':
            n = int(b)
            return max(0, SIZE - n), SIZE, True
        start = int(a)
        end = SIZE if b == '' else min(SIZE, int(b) + 1)
        return start, end, True

    def do_HEAD(self):
        self.do_GET(head=True)

    def do_GET(self, head=False):
        if self.path != NAME:
            self.send_error(404)
            return
        start, end, partial = self._range()
        if start >= end:
            self.send_response(416)
            self.send_header('Content-Range', 'bytes */%d' % SIZE)
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        self.send_response(206 if partial else 200)
        self.send_header('Content-Type', 'application/octet-stream')
        self.send_header('Accept-Ranges', 'bytes')
        self.send_header('Content-Length', str(end - start))
        if partial:
            self.send_header('Content-Range', 'bytes %d-%d/%d' % (start, end - 1, SIZE))
        self.end_headers()
        if head:
            return
        with open(PATH, 'rb') as f:
            f.seek(start)
            left = end - start
            try:
                while left > 0:
                    n = min(16384, left)
                    take(n)
                    data = f.read(n)
                    if not data:
                        break
                    self.wfile.write(data)
                    left -= len(data)
            except (BrokenPipeError, ConnectionResetError):
                pass


srv = ThreadingHTTPServer(('127.0.0.1', PORT), Handler)
srv.daemon_threads = True
print('ready %d' % PORT, flush=True)
srv.serve_forever()

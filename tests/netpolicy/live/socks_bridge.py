#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# [net-2] Live smoke run helper (not a ctest). Two roles:
#   proxy  <unix socket> <log>   a SOCKS5 proxy (CONNECT only, no login, names and addresses) that
#                                listens on a Unix socket and connects out from this network
#                                namespace; logs one line per CONNECT target (kept local)
#   bridge <port> <unix socket>  inside the program's network namespace: 127.0.0.1:<port> forwarded
#                                to the Unix socket (the only way out of that namespace)
# UDP ASSOCIATE is refused, as by many proxies.
import os
import socket
import struct
import sys
import threading


def pipe(a, b):
    try:
        while True:
            d = a.recv(65536)
            if not d:
                break
            b.sendall(d)
    except OSError:
        pass
    for s in (a, b):
        try:
            s.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass


def recv_exact(s, n):
    out = b''
    while len(out) < n:
        d = s.recv(n - len(out))
        if not d:
            raise OSError('closed')
        out += d
    return out


def serve_proxy(c, log, lock):
    try:
        ver, n = recv_exact(c, 2)
        recv_exact(c, n)
        c.sendall(b'\x05\x00')
        ver, cmd, _, atyp = recv_exact(c, 4)
        if atyp == 1:
            host = socket.inet_ntoa(recv_exact(c, 4))
        elif atyp == 4:
            host = socket.inet_ntop(socket.AF_INET6, recv_exact(c, 16))
        elif atyp == 3:
            host = recv_exact(c, recv_exact(c, 1)[0]).decode('ascii', 'replace')
        else:
            raise OSError('atyp')
        port = struct.unpack('>H', recv_exact(c, 2))[0]
        with lock:
            log.write('%s %s %d\n' % ('UDP' if cmd == 3 else 'TCP', 'name' if atyp == 3 else 'addr', port))
            log.flush()
        if cmd != 1:
            c.sendall(b'\x05\x07\x00\x01' + b'\0' * 6)
            c.close()
            return
        try:
            up = socket.create_connection((host, port), timeout=15)
            up.settimeout(None)
        except OSError:
            c.sendall(b'\x05\x04\x00\x01' + b'\0' * 6)
            c.close()
            return
        c.sendall(b'\x05\x00\x00\x01' + b'\0' * 6)
        threading.Thread(target=pipe, args=(c, up), daemon=True).start()
        pipe(up, c)
    except (OSError, ValueError):
        try:
            c.close()
        except OSError:
            pass


def main():
    role = sys.argv[1]
    if role == 'proxy':
        path, logpath = sys.argv[2], sys.argv[3]
        if os.path.exists(path):
            os.unlink(path)
        srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        srv.bind(path)
        srv.listen(256)
        log = open(logpath, 'a')
        lock = threading.Lock()
        while True:
            c, _ = srv.accept()
            threading.Thread(target=serve_proxy, args=(c, log, lock), daemon=True).start()
    elif role == 'bridge':
        port, path = int(sys.argv[2]), sys.argv[3]
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(('127.0.0.1', port))
        srv.listen(256)
        while True:
            c, _ = srv.accept()
            u = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                u.connect(path)
            except OSError:
                c.close()
                continue
            threading.Thread(target=pipe, args=(c, u), daemon=True).start()
            threading.Thread(target=pipe, args=(u, c), daemon=True).start()


if __name__ == '__main__':
    main()

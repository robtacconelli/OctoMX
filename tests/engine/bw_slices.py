#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# bw_slices.py [bw-2]: rates of a bw_slice_probe run as a share of the limit.
#   bw_slices.py RUN_DIR [--warm S] [--tail S]      (RUN_DIR holds os.csv, prog.csv, graph.csv, case.txt)
#   bw_slices.py --table RUN_DIR...                 (one Markdown row per run)
# app   = what the program really took from (or gave to) its sockets: BitTorrent from the kernel's
#         read / write pointers of the program's sockets (libtorrent reads its sockets itself, outside
#         the program's own 1 s counters), HTTP and eD2k from the program's counters, which count at
#         the moment of reading / writing; this is what a speed limit governs;
# wire  = TCP bytes the other side received (or sent, uploads), measured outside the program;
# prog  = what the program counted (limiter totals, all networks);
# graph = the Bandwidth service's 1 s samples (what the graph draws).
# Windows slide in steps of 50 ms: 100 ms, 250 ms and 1 s. Shares: mean, 95th percentile, maximum.
import csv
import os
import sys

NETS = ["bt", "http", "ed2k"]


def read(path):
    with open(path) as f:
        r = csv.DictReader(f)
        return [row for row in r]


def interp(ts, vs, grid):
    out, j = [], 0
    for t in grid:
        while j + 1 < len(ts) and ts[j + 1] <= t:
            j += 1
        if j + 1 >= len(ts):
            out.append(vs[-1])
        elif t <= ts[j]:
            out.append(vs[j])
        else:
            f = (t - ts[j]) / (ts[j + 1] - ts[j])
            out.append(vs[j] + f * (vs[j + 1] - vs[j]))
    return out


def stats(xs):
    if not xs:
        return (0, 0, 0)
    s = sorted(xs)
    return (sum(s) / len(s), s[min(len(s) - 1, int(0.95 * len(s)))], s[-1])


def analyse(run, warm=10.0, tail=2.0):
    mode, nets, down, up = open(os.path.join(run, "case.txt")).read().split()[:4]
    limit = (int(down) if mode == "down" else int(up)) * 1024
    nets = nets.split(",")
    os_rows = read(os.path.join(run, "os.csv"))
    wcol = "_sent" if mode == "down" else "_recv"
    acol = "_read" if mode == "down" else "_written"
    ts = [int(r["ms"]) for r in os_rows]
    wire = {n: [int(r[n + wcol]) for r in os_rows] for n in NETS}
    btapp = [int(r["bt" + acol]) for r in os_rows]
    prog_rows = read(os.path.join(run, "prog.csv"))
    pts = [int(r["ms"]) for r in prog_rows]
    pcol = "_down" if mode == "down" else "_up"
    pcum = {n: [int(r[n + pcol]) for r in prog_rows] for n in NETS}
    wtot = [sum(wire[n][i] for n in NETS) for i in range(len(ts))]
    first = next((ts[i] for i in range(len(ts)) if wtot[i] > 0), ts[0])
    t0, t1 = first + warm * 1000, min(ts[-1], pts[-1]) - tail * 1000
    grid = [t0 + 50 * k for k in range(int((t1 - t0) / 50) + 1)]
    span = (grid[-1] - grid[0]) / 1000.0
    res = {"run": os.path.basename(run), "mode": mode, "nets": "+".join(nets), "limit": limit // 1024, "seconds": span}
    app = {"bt": interp(ts, btapp, grid)}
    for n in ("http", "ed2k"):
        app[n] = interp(pts, pcum[n], grid)
    series = {
        "app": [sum(app[n][i] for n in NETS) for i in range(len(grid))],
        "wire": interp(ts, wtot, grid),
        "prog": interp(pts, [sum(pcum[n][i] for n in NETS) for i in range(len(pts))], grid),
    }
    for name, g in series.items():
        for label, w in (("100ms", 2), ("250ms", 5), ("1s", 20)):
            res[name + "_" + label] = stats([(g[i + w] - g[i]) / (w * 0.05) / limit for i in range(len(g) - w)])
        res[name + "_long"] = (g[-1] - g[0]) / span / limit
    for n in NETS:
        res["share_" + n] = (app[n][-1] - app[n][0]) / span / limit
    gr = read(os.path.join(run, "graph.csv"))
    gcol = "down" if mode == "down" else "up"
    gv = [int(r[gcol]) / limit for r in gr if t0 <= int(r["ms"]) <= t1]
    res["graph"] = stats(gv)
    res["graph_min"] = min(gv) if gv else 0
    xcol = "xferDown" if mode == "down" else "xferUp"
    xv = [int(r[xcol]) / limit for r in gr if xcol in r and r[xcol] not in (None, "") and t0 <= int(r["ms"]) <= t1]
    res["xfer"] = stats(xv)
    res["xfer_min"] = min(xv) if xv else 0
    return res


def pct(x):
    return "%d" % round(100 * x)


def row(r):
    o, q, w = r["app_1s"], r["app_250ms"], r["wire_1s"]
    shares = " ".join("%s %s" % (n, pct(r["share_" + n])) for n in NETS if r["share_" + n] > 0.005)
    xf = ("%s / %s" % (pct(r["xfer_min"]), pct(r["xfer"][2]))) if r["xfer"][2] > 0 else "n/a"
    return "| %s | %s %s | %d | %s / %s / %s | %s / %s / %s | %s | %s / %s | %s / %s / %s | %s | %s |" % (
        r["run"], r["nets"], r["mode"], r["limit"],
        pct(o[0]), pct(o[1]), pct(o[2]), pct(q[0]), pct(q[1]), pct(q[2]), pct(r["app_long"]),
        pct(w[1]), pct(w[2]), pct(r["graph_min"]), pct(r["graph"][1]), pct(r["graph"][2]), xf, shares)


HEAD = ("| run | networks | limit KB/s | 1 s mean / p95 / max % | 250 ms mean / p95 / max % | long-run % "
        "| wire 1 s p95 / max % | graph min / p95 / max % | Transfers min / max % | shares of the limit % |\n|---|---|---|---|---|---|---|---|---|---|")

if __name__ == "__main__":
    args = sys.argv[1:]
    warm = 10.0
    if "--warm" in args:
        i = args.index("--warm")
        warm = float(args[i + 1])
        del args[i:i + 2]
    if args and args[0] == "--table":
        print(HEAD)
        for run in args[1:]:
            try:
                print(row(analyse(run, warm)))
            except Exception as e:  # a run that did not produce data
                print("| %s | failed: %s |" % (os.path.basename(run), e))
    else:
        r = analyse(args[0], warm)
        for k, v in r.items():
            print(k, v)

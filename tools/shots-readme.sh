#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tools/shots-readme.sh: retake the README pictures (docs/screenshots/*.png) from a build, in one go.
#
#   tools/shots-readme.sh [--out <dir>] [--only <names>] [--win-build <dir>] [--xvnc <70..99>] [--keep]
#
#   --out        where the finished pictures go (default docs/screenshots)
#   --only       comma separated picture names or groups (default: all):
#                  live: networks, search, transfers, transfers-light, bandwidth (real networks, about 9 min)
#                  demo: chat, connection-test, wizard (built-in demo data, about 1 min)
#   --win-build  the Windows cross build to photograph (default build/win, must be built)
#   --xvnc       X display number for the private Xvnc (default: the first free one from 70 to 99)
#   --keep       keep the work folder (build/shots-readme/run: profiles, logs, full screen grabs)
#
# Every picture is the Windows build running under wine with the real Win32 window frame (title bar
# with the program icon and "OctoMX v5.10 ..."), cropped exactly to the window, 100 %, no scaling:
# wine in its own prefix (build/shots-readme/wineprefix, Windows 2000 standard colours, a 1280x1024
# virtual desktop) on its own Xvnc. Nothing is written to the real HOME, ~/.wine or ~/Downloads:
# HOME points into the work folder, every run has its own --profile and MX_OS_ROOT, downloads go
# into the profile, which is deleted at the end.
#
# The live pictures search the open film Big Buck Bunny (Blender Foundation, CC BY) on the real networks and
# download it for a few minutes (MX_OVERLAY_DISCOVERY=0, no UPnP / NAT-PMP, only the Internet Archive
# and LinuxTracker web indexes). They differ from run to run. Before a picture is kept, the script
# checks the file names it shows (no IP address, search rows only for the film, no site name from
# BLOCKLIST below; a picture that fails is not written and the old one stays) and covers the peer
# addresses the program shows in the sources of the opened search row and of the download with a
# grey bar (see redact). Look at every picture at full size before committing it.
#
# The title bar of every picture shows the program icon, so after a logo change run it without
# --only (or --only demo for the three pictures that do not need the networks, the wizard shows
# the logo large).
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source "$ROOT/toolchain/env.sh"

OUT=$ROOT/docs/screenshots
ONLY=all
WIN_BUILD=$ROOT/build/win
XN=
KEEP=0
while [ $# -gt 0 ]; do
    case "$1" in
        --out) OUT=$2; shift 2 ;;
        --only) ONLY=$2; shift 2 ;;
        --win-build) WIN_BUILD=$2; shift 2 ;;
        --xvnc) XN=${2#:}; shift 2 ;;
        --keep) KEEP=1; shift ;;
        -h|--help) sed -n 3,32p "$0"; exit 0 ;;
        *) echo "unknown option $1 (see --help)" >&2; exit 2 ;;
    esac
done
OUT=$(realpath -m "$OUT")
WIN_BUILD=$(realpath -m "$WIN_BUILD")
[ -f "$WIN_BUILD/bin/octomx.exe" ] || { echo "error: $WIN_BUILD/bin/octomx.exe not found (ninja -C build/win)" >&2; exit 1; }

LIVE_SHOTS="networks search transfers transfers-light bandwidth"
DEMO_SHOTS="chat connection-test wizard"
WANT=" "
for w in ${ONLY//,/ }; do
    case "$w" in
        all) WANT+="$LIVE_SHOTS $DEMO_SHOTS " ;;
        live) WANT+="$LIVE_SHOTS " ;;
        demo) WANT+="$DEMO_SHOTS " ;;
        *) [[ " $LIVE_SHOTS $DEMO_SHOTS " == *" $w "* ]] || { echo "unknown picture '$w'" >&2; exit 2; }
           WANT+="$w " ;;
    esac
done
wanted() { [[ "$WANT" == *" $1 "* ]]; }
any_wanted() { local s; for s in "$@"; do wanted "$s" && return 0; done; return 1; }

# Words that must not appear in a visible search row: names of sites that exist to share infringing
# material (and domain-like texts in general, checked separately).
BLOCKLIST='yts|yify|rarbg|eztv|ettv|1337x|piratebay|pirate bay|tpb|uindex|tfile|rutracker|kinozal|torrentgalaxy|tgx|limetorrents|glodls|nyaa|anidub|rutor|nnmclub|torrent9|cpasbien|ytsmx|megapeer|zamunda|bitru'
FILM="big buck bunny"                         # the open film searched for
EXCLUDE="-frostwire -frostclick -usenet -mecanoscrit -exploradoresp2p"             # file names that carry a site name, filtered out before anything is opened
FILTERS=("big buck bunny $EXCLUDE" "big buck bunny surround $EXCLUDE" "big buck bunny avi $EXCLUDE" "big buck bunny 1080p $EXCLUDE" "big buck bunny mp4 $EXCLUDE")   # tried in this order, first clean one wins
MERGED=${MERGED:-BT+eD2k}                     # text of the merged row's Network column to open and download
DL_ROW=${DL_ROW:-BT+HTTP}                      # text of the Network column of the row that is downloaded for the Transfers pictures
FIRST_LIMIT=${FIRST_LIMIT:-450}               # KB/s while the Transfers pictures are taken
DL_MINUTES=${DL_MINUTES:-4}                   # download time before the Transfers pictures
DOWN_LIMIT=${DOWN_LIMIT:-300}                 # KB/s, Bandwidth picture (below the usual rate, so the cap shows)
UP_LIMIT=${UP_LIMIT:-12}                      # (10 is the smallest upload limit the program accepts)

WORK=$ROOT/build/shots-readme                  # kept between runs: wine prefix, network contacts
RUN=$WORK/run                                  # this run (deleted at the end unless --keep)
rm -rf "$RUN"
mkdir -p "$RUN/raw" "$RUN/home" "$OUT"
export HOME=$RUN/home XDG_CONFIG_HOME=$RUN/home/.config XDG_DATA_HOME=$RUN/home/.local/share
export WINEPREFIX=$WORK/wineprefix WINEDEBUG=-all
export MX_OS_ROOT=$RUN/os-root MX_SOUND_OFF=1 MX_DIGIT_GROUPING=space LC_ALL=C.UTF-8
mkdir -p "$MX_OS_ROOT"
PY=$OCTOMX_VENV/bin/python                     # the toolchain's Python with Pillow
"$PY" -c 'import PIL' 2>/dev/null || { echo "error: no Pillow in $PY (tools/install-toolchain.sh)" >&2; exit 1; }
T0=$(date +%s)
log() { echo "[$(( $(date +%s) - T0 ))s] $*"; }

# ---- the Windows build as a user gets it (dll and plugin folders next to octomx.exe)
"$ROOT/tools/package-win.sh" -e octomx.exe --no-zip "$WIN_BUILD" "$RUN/pkg" > "$RUN/package.log" 2>&1 \
    || { echo "error: packaging failed, see $RUN/package.log" >&2; exit 1; }
PKG=$RUN/pkg/octomx

# ---- private wine prefix: Windows 2000 standard colours (gradient captions), no visual theme, a
# virtual desktop (Xvnc has no window manager, so wine draws the frames itself)
REG_VERSION=1
if [ "$(cat "$WINEPREFIX/.shots-readme" 2>/dev/null)" != "$REG_VERSION" ]; then
    log "preparing the wine prefix $WINEPREFIX"
    if [ ! -f "$WINEPREFIX/system.reg" ]; then
        mkdir -p "$WORK/wine-home"
        HOME=$WORK/wine-home DISPLAY= wineboot -i > "$RUN/wineboot.log" 2>&1 || true
        wineserver -w
    fi
    for f in "$WINEPREFIX"/drive_c/windows/system32/spool/drivers/color/*.ic[cm]; do
        [ -e "$f" ] && mv -f "$f" "$f.disabled-for-qt"   # Qt 6.12 + wine 10: see tools/wine-init.sh
    done
    cat > "$RUN/classic.reg" <<'EOF'
Windows Registry Editor Version 5.00

[HKEY_CURRENT_USER\Control Panel\Colors]
"ActiveBorder"="212 208 200"
"ActiveTitle"="10 36 106"
"AppWorkSpace"="128 128 128"
"Background"="58 110 165"
"ButtonAlternateFace"="181 181 181"
"ButtonDkShadow"="64 64 64"
"ButtonFace"="212 208 200"
"ButtonHilight"="255 255 255"
"ButtonLight"="212 208 200"
"ButtonShadow"="128 128 128"
"ButtonText"="0 0 0"
"GradientActiveTitle"="166 202 240"
"GradientInactiveTitle"="192 192 192"
"GrayText"="128 128 128"
"Hilight"="10 36 106"
"HilightText"="255 255 255"
"HotTrackingColor"="0 0 128"
"InactiveBorder"="212 208 200"
"InactiveTitle"="128 128 128"
"InactiveTitleText"="212 208 200"
"InfoText"="0 0 0"
"InfoWindow"="255 255 225"
"Menu"="212 208 200"
"MenuBar"="212 208 200"
"MenuHilight"="10 36 106"
"MenuText"="0 0 0"
"Scrollbar"="212 208 200"
"TitleText"="255 255 255"
"Window"="255 255 255"
"WindowFrame"="0 0 0"
"WindowText"="0 0 0"

[HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\ThemeManager]
"ThemeActive"="0"

[HKEY_CURRENT_USER\Software\Wine\Explorer]
"Desktop"="Default"

[HKEY_CURRENT_USER\Software\Wine\Explorer\Desktops]
"Default"="1280x1024"
EOF
    DISPLAY= wine regedit /S "$(winepath -w "$RUN/classic.reg" 2>/dev/null)" > /dev/null 2>&1
    wineserver -w
    echo "$REG_VERSION" > "$WINEPREFIX/.shots-readme"
fi
DESKTOP_RGB="58,110,165"

# ---- private Xvnc; everything is stopped on exit
if [ -z "$XN" ]; then
    for n in $(seq 70 99); do
        if [ ! -e "/tmp/.X$n-lock" ] && [ ! -e "/tmp/.X11-unix/X$n" ]; then XN=$n; break; fi
    done
    [ -n "$XN" ] || { echo "error: no free X display from 70 to 99" >&2; exit 1; }
fi
cleanup() {
    wineserver -k > /dev/null 2>&1 || true
    "$ROOT/tools/xvnc-stop.sh" "$XN" > /dev/null 2>&1 || true
    if [ "$KEEP" = 0 ]; then rm -rf "$RUN"; else echo "work folder kept: $RUN"; fi
}
trap cleanup EXIT
"$ROOT/tools/xvnc-start.sh" "$XN" > /dev/null
export DISPLAY=:$XN
wpath() { winepath -w "$1" 2>/dev/null; }

FAILED=()
KEPT=()

# crop <full.png> <out.png>: the window the snap is about: every pixel connected to a point inside it
# (WIN_X + 100, WIN_Y + 100) that is not the desktop colour. Wine shows its system tray as a small
# window at the top left, which stays separate (the windows are placed at WIN_X, WIN_Y below it).
WIN_X=40 WIN_Y=72
crop() {
    "$PY" - "$1" "$2" "$DESKTOP_RGB" $(( WIN_X + 100 )) $(( WIN_Y + 100 )) <<'PY'
import sys
from PIL import Image, ImageChops, ImageDraw
src, dst = sys.argv[1], sys.argv[2]
bg = tuple(int(v) for v in sys.argv[3].split(','))
seed = (int(sys.argv[4]), int(sys.argv[5]))
im = Image.open(src).convert('RGB')
mask = ImageChops.difference(im, Image.new('RGB', im.size, bg)).convert('L').point(lambda v: 255 if v else 0)
if mask.getpixel(seed) == 0:
    sys.exit('no window at %d,%d' % seed)
ImageDraw.floodfill(mask, seed, 128)
box = mask.point(lambda v: 255 if v == 128 else 0).getbbox()
im.crop(box).save(dst)
print('%dx%d at %d,%d' % (box[2] - box[0], box[3] - box[1], box[0], box[1]))
PY
}

# redact <full.png> <rows file> <column> <which>: cover peer addresses in one column of the listed
# rows with a grey bar. The app shows them and has no switch to hide them: the Network cell of an
# eD2k or Gnutella source of a search row reads "eD2k: <address>:<port>" (the part after "<network>: "
# is covered), the User cell of a peer row in Transfers is the address (covered when it has dots
# between digits). <which>: "search:<session dir>" the children of the opened merged row (found by
# comparing the list before and after expand-text), "children" rows without a file name (sources of
# a download), "all". Columns come from the header's separator lines, rows from report-rows.
redact() {
    "$PY" - "$@" <<'PY'
import sys
from PIL import Image, ImageDraw
full, rowsfile, col, which = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
def rows_of(path):
    out = []
    try:
        lines = open(path, encoding='utf-8', errors='replace').read().splitlines()
    except FileNotFoundError:
        return out
    for l in lines:
        p = l.split(' ', 6)
        if l.startswith('row ') and len(p) == 7:
            out.append((int(p[2]), int(p[3]), int(p[4]), int(p[5]), p[6]))
    return out
rows = rows_of(rowsfile)
if not rows:
    print('no rows')
    sys.exit(0)
if which.startswith('search:'):
    sdir = which[7:]
    pre = [r[4] for r in rows_of(sdir + '/rows-merge-pre')]
    post = [r[4] for r in rows_of(sdir + '/rows-merge-post')]
    k = next(i for i in range(min(len(pre), len(post))) if pre[i] != post[i])
    parent = post[k - 1]
    try:
        kids = post.index(pre[k], k) - k
    except ValueError:      # the list is longer than the view: every row after the parent is a child
        kids = len(post) - k
    pi = [r[4] for r in rows].index(parent)
    targets = rows[pi + 1:pi + 1 + kids]
elif which == 'children':
    targets = [r for r in rows if not r[4].strip()]
else:
    targets = rows
im = Image.open(full).convert('RGB')
px = im.load()
gx, w = rows[0][0], rows[0][2]
gy0 = min(r[1] for r in rows)   # the header ends right above the first visible row
dark = lambda x, y: sum(px[x, y]) < 200
seps = [x for x in range(gx, gx + w) if all(dark(x, y) for y in range(gy0 - 14, gy0 - 3))]
left = (seps[col - 1] if col > 0 else gx) + 2
right = seps[col] - 2
draw = ImageDraw.Draw(im)
covered = 0
for (x0, y0, ww, h, name) in targets:
    cells = {}
    bg = px[left, y0 + 1]
    for x in range(left, right):
        ys = [y for y in range(y0, y0 + h) if px[x, y] != bg]
        if ys:
            cells[x] = ys
    if not cells:
        continue
    base = max(max(v) for v in cells.values())
    def runs(ys):
        r, prev = [], None
        for y in ys:
            if prev is None or y != prev + 1:
                r.append([y, y])
            else:
                r[-1][1] = y
            prev = y
        return r
    def lone(x):
        return x - 1 not in cells and x + 1 not in cells
    def colon(x):    # one column, two short runs, the lower one on the baseline
        rr = runs(cells[x])
        return (lone(x) and len(rr) == 2 and all(b - a <= 1 for a, b in rr) and rr[1][1] == base
                and rr[1][0] - rr[0][1] >= 2)
    def dot(x):      # a dot between two glyphs (1.2.3.4), not one of the three dots of an elision
        return (lone(x) and len(cells[x]) <= 2 and max(cells[x]) == base and not colon(x)
                and any(x - d in cells and not lone(x - d) for d in (2, 3, 4))
                and any(x + d in cells and not lone(x + d) for d in (2, 3, 4)))
    xs = sorted(cells)
    start = None
    first = next((x for x in xs if colon(x) and x + 2 not in cells), None)   # "<network>: "
    if first is not None and any(dot(x) or colon(x) for x in xs if x > first):
        start = first + 4
    elif sum(1 for x in xs if dot(x)) >= 2:   # an address without "<network>: "
        start = xs[0]
    if start is None:
        continue
    end = max(cells)
    if end < start:
        continue
    draw.rectangle([start, y0 + 2, end + 1, y0 + h - 3], fill=(112, 112, 112))
    covered += 1
im.save(full)
print('%d of %d cells covered' % (covered, len(targets)))
PY
}

# check_rows <name> <kind> <session dir> <rows files...>: checks of the rows a picture shows. The
# program reports the File column of every visible row (report-rows): no IP address in any of them;
# search pictures: every row names the film, no site name, and the merged row is among them (found
# by comparing the list before and after expand-text: the row above the inserted children).
check_rows() {
    "$PY" - "$FILM" "$BLOCKLIST" "$@" <<'PY'
import re, sys
film, blocklist, name, kind, sdir = sys.argv[1:6]
files = sys.argv[6:]
ip = re.compile(r'(?<![\d.])\d{1,3}(\.\d{1,3}){3}(?![\d.])|\b[0-9a-fA-F]{1,4}(:[0-9a-fA-F]{0,4}){3,7}:[0-9a-fA-F]{1,4}\b')
domain = re.compile(r'\bwww\.|\b[a-z0-9-]{2,}\.(com|org|net|ru|to|se|me|io|cc|tv|info|xyz|si|eu|li|pw|ws|ch|nz|ag|cat|es|fr|de|it|uk|pl|nl|cz|br|ar|mx|tk|biz|club|top|site|online|lol|is|ph|gs|st|bz)\b', re.I)
blocked = re.compile(r'(?<![a-z])(' + blocklist + r')(?![a-z])', re.I)
def names(path):
    try:
        return [l.split(' ', 6)[6] for l in open(path, encoding='utf-8', errors='replace').read().splitlines()
                if l.startswith('row ') and len(l.split(' ', 6)) == 7]
    except FileNotFoundError:
        return None
problems, rows = [], []
for f in files:
    rows += names(f) or []   # no report: the list had no row
for text in rows:
    if ip.search(text):
        problems.append('address in row: ' + text[:80])
    if kind == 'search':
        if blocked.search(text) or domain.search(text):
            problems.append('site name in row: ' + text[:80])
        if re.sub(r'[\W_]+', '', film.lower()) not in re.sub(r'[\W_]+', '', text.lower()):
            problems.append('row not about the film: ' + text[:80])
if kind == 'search':
    pre, post = names(sdir + '/rows-merge-pre') or [], names(sdir + '/rows-merge-post') or []
    k = next((i for i in range(min(len(pre), len(post))) if pre[i] != post[i]), None)
    parent = post[k - 1] if k else None
    if not parent:
        problems.append('the merged row was not found')
    elif parent not in rows:
        problems.append('the merged row "%s" is not shown' % parent[:60])
    if len(rows) < 6:
        problems.append('only %d rows' % len(rows))
if problems:
    print('%s' % '; '.join(problems[:3]))
    sys.exit(1)
print('%d rows checked' % len(rows) + (', merged row "%s"' % parent if kind == 'search' else ''))
PY
}

# optimise <png>: lossless; a palette image when the picture has at most 256 colours (verified)
optimise() {
    "$PY" - "$1" <<'PY'
import sys
from PIL import Image
p = sys.argv[1]
im = Image.open(p).convert('RGB')
colors = im.getcolors(256)
out = im
if colors:
    pal = Image.new('P', (1, 1))
    flat = [c for _, rgb in colors for c in rgb]
    pal.putpalette(flat + [0] * (768 - len(flat)))
    q = im.quantize(palette=pal, dither=Image.Dither.NONE)
    if q.convert('RGB').tobytes() == im.tobytes():
        out = q
out.save(p, optimize=True)
PY
}

# session <tag> <demo|live> <settings json> <timeout s> <script lines...>
# "@DIR@" in a line is the session folder (wine path). A line "snap <name> [-] [<list>...]" marks a
# picture: the program reports the rows of the lists, writes a mark file and waits; this shell grabs
# the screen, checks the rows, covers the addresses, crops the window, then lets it go on.
session() {
    local tag=$1 mode=$2 settings=$3 limit=$4; shift 4
    local dir=$RUN/$tag; mkdir -p "$dir/prof/incoming"
    local wdir; wdir=$(wpath "$dir")
    printf '%s\n' "$settings" > "$dir/prof/settings.json"
    if [ "$mode" = live ] && [ -d "$WORK/contacts" ]; then cp -r "$WORK/contacts/." "$dir/prof/"; fi
    local script=$dir/session.script snaps=() line
    : > "$script"
    for line in "$@"; do
        if [[ $line == snap\ * ]]; then
            read -r _ name win lists <<< "$line"
            local l
            for l in $lists; do echo "report-rows $l $wdir\\rows-$name-$l" >> "$script"; done
            printf 'report-windows\necho-file %s\\mark-%s\nwait-file %s\\go-%s 600\n' "$wdir" "$name" "$wdir" "$name" >> "$script"
            snaps+=("$name|$win|$lists")
        else
            echo "${line//@DIR@/$wdir}" >> "$script"
        fi
    done
    echo "quit" >> "$script"
    local args=(--profile "$(wpath "$dir/prof")" --no-wizard --size 1100x690 --script "$(wpath "$script")")
    [ "$mode" = demo ] && args+=(--demo-frozen)
    log "session $tag ($mode, ${#snaps[@]} pictures)"
    ( cd "$PKG" && MX_OVERLAY_DISCOVERY=0 timeout "$limit" wine octomx.exe "${args[@]}" > "$dir/run.log" 2>&1; echo $? > "$dir/exit" ) &
    local pid=$! s
    for s in "${snaps[@]}"; do
        IFS='|' read -r name win lists <<< "$s"
        while [ ! -f "$dir/mark-$name" ]; do
            if [ -f "$dir/exit" ]; then
                log "FAILED: session $tag ended before '$name' (exit $(cat "$dir/exit"); $dir/prof/logs/octomx.log)"
                grep -E 'script: (line|error)|failed' "$dir/prof/logs/octomx.log" 2>/dev/null | tail -2
                wait "$pid" || true
                wineserver -k > /dev/null 2>&1 || true
                local rest; for rest in "${snaps[@]}"; do IFS='|' read -r n _ <<< "$rest"; [ -f "$RUN/raw/$n.png" ] || FAILED+=("$n"); done
                return 0
            fi
            sleep 0.5
        done
        sleep 1.5   # let the last repaint reach the screen
        "$ROOT/tools/x-screenshot.sh" "$XN" "$dir/full-$name.png" > /dev/null
        local ok=1 kind=rows note=
        [[ $name == search* ]] && kind=search
        if [ -n "$lists" ]; then
            local files=() l
            for l in $lists; do files+=("$dir/rows-$name-$l"); done
            check_rows "$name" "$kind" "$dir" "${files[@]}" > "$dir/check-$name.txt" || ok=0
        fi
        if [ $ok = 1 ]; then   # cover the peer addresses the lists show
            local r1= r2=
            if [ $kind = search ]; then
                r1=$(redact "$dir/full-$name.png" "${files[0]}" 5 "search:$dir" 2>&1) || ok=0
                note="search sources: $r1"
            elif [[ " $lists " == *" downloadList "* ]]; then
                r1=$(redact "$dir/full-$name.png" "$dir/rows-$name-downloadList" 1 children 2>&1) || ok=0
                r2=$(redact "$dir/full-$name.png" "$dir/rows-$name-uploadList" 1 all 2>&1) || ok=0
                note="download sources: $r1, uploads: $r2"
            fi
            [ $ok = 1 ] || echo "covering the addresses failed: $r1 $r2" > "$dir/check-$name.txt"
        fi
        local geo; geo=$(crop "$dir/full-$name.png" "$dir/crop-$name.png")
        if [ $ok = 1 ]; then
            cp "$dir/crop-$name.png" "$RUN/raw/$name.png"
            log "  $name: $geo$( [ -n "$lists" ] && echo ", $(cat "$dir/check-$name.txt")")${note:+, $note}"
        else
            log "  $name: not used: $(cat "$dir/check-$name.txt")"
        fi
        touch "$dir/go-$name"
    done
    wait "$pid" || true
    wineserver -k > /dev/null 2>&1 || true
    if [ "$mode" = live ]; then   # keep the network contacts (not the identity, not the downloads) for the next run
        rm -rf "$WORK/contacts"; mkdir -p "$WORK/contacts"
        local f
        for f in kad/nodes.dat ed2k/server.met gnutella/hosts.json rats/peers.json rats/dht_routing.json rats/dht_routing_v6.json bt/session.dat; do
            [ -f "$dir/prof/$f" ] && { mkdir -p "$WORK/contacts/$(dirname "$f")"; cp "$dir/prof/$f" "$WORK/contacts/$f"; }
        done
    fi
    rm -rf "$dir/prof/incoming" "$dir/prof/incomplete"   # the downloaded parts of the film
}

# ---- demo pictures (built-in demo data, frozen)
DEMO_SET='{"ui/settingsMigrated":1,"ui/chatWarningDisabled":true,"diag/hideAddresses":true}'
if wanted chat; then
    session chat demo "$DEMO_SET" 120 \
        "move-window mainWindow $WIN_X $WIN_Y" "page chat" "wait 300" "join OctoMX Help_49717657BABE" "wait 300" \
        "join Linux Users" "wait 500" "snap chat"
fi
if wanted connection-test; then
    session conntest demo "$DEMO_SET" 120 \
        "page networks" "wait 300" "click btnConnectionTest" "wait 1500" \
        "move-window mainWindow 2000 1200" "move-window connectionTestDialog $WIN_X $WIN_Y" "wait 500" "snap connection-test"
fi
if wanted wizard; then
    session wizard demo "$DEMO_SET" 120 \
        "page networks" "wait 300" "wizard" "wait 1500" \
        "move-window mainWindow 2000 1200" "move-window setupWizard $WIN_X $WIN_Y" "wait 500" "snap wizard"
fi

# ---- live pictures (real networks)
if any_wanted $LIVE_SHOTS; then
    W=$(wpath "$RUN/live/prof/incoming"); W=${W//\\//}
    LIVE_SET=$(cat <<JSON
{"ui/settingsMigrated":1,"ui/chatWarningDisabled":true,"identity/screenName":"Guest123_45678",
 "connection/upnp":false,"connection/natpmp":false,
 "search/adultFilter":true,"transfers/incomingDir":"$W",
 "web/providerEnabled":{"apibay":false,"knaben":false,"torrents-csv":false,"bitsearch":false,"yts":false,"nyaa":false,
                        "bt4g":false,"limetorrents":false,"internet-archive":true,"linuxtracker":true}}
JSON
)
    lines=("move-window mainWindow $WIN_X $WIN_Y" "page networks" "wait 60000" "snap networks"
           "page search" "type searchText $FILM" "click searchGo" "wait 90000"
           "type searchFilter $FILM $EXCLUDE" "wait 1000"
           "set-column searchResults 5 150" "report-rows searchResults @DIR@\\rows-merge-pre"
           "expand-text searchResults $MERGED" "wait 500" "report-rows searchResults @DIR@\\rows-merge-post")
    i=0
    for f in "${FILTERS[@]}"; do
        lines+=("type searchFilter $f" "wait 1000" "snap search-$i - searchResults")
        i=$((i + 1))
    done
    # a first limit a few seconds after the start, so that a well seeded file is still downloading
    # when the pictures are taken
    lines+=("type searchFilter $FILM $EXCLUDE" "wait 1000"
            "dblclick-text searchResults $DL_ROW" "wait 3000" "accept-dialogs" "page transfers" "wait 12000"
            "set bandwidth/lastDownLimitKBs $FIRST_LIMIT" "set bandwidth/downLimitKBs $FIRST_LIMIT"
            "wait $(( DL_MINUTES * 60000 ))" "expand-row downloadList 0" "wait 2000"
            "snap transfers - downloadList uploadList"
            "set ui/colorTheme light" "wait 2000" "snap transfers-light - downloadList uploadList"
            "set ui/colorTheme dark" "set bandwidth/lastDownLimitKBs $DOWN_LIMIT" "set bandwidth/downLimitKBs $DOWN_LIMIT"
            "set bandwidth/lastUpLimitKBs $UP_LIMIT" "set bandwidth/upLimitKBs $UP_LIMIT"
            "page bandwidth" "wait 90000" "snap bandwidth")
    session live live "$LIVE_SET" $(( DL_MINUTES * 60 + 900 )) "${lines[@]}"
    # the first filter whose rows passed the checks
    for j in $(seq 0 $(( ${#FILTERS[@]} - 1 ))); do
        if [ -f "$RUN/raw/search-$j.png" ]; then mv "$RUN/raw/search-$j.png" "$RUN/raw/search.png"; log "search: filter '${FILTERS[$j]}'"; break; fi
    done
    rm -f "$RUN"/raw/search-*.png
    FAILED=("${FAILED[@]/search-*/}")
fi

# ---- into --out: only the wanted pictures that were taken and passed the checks
for s in $LIVE_SHOTS $DEMO_SHOTS; do
    wanted "$s" || continue
    if [ -f "$RUN/raw/$s.png" ]; then
        optimise "$RUN/raw/$s.png"
        cp "$RUN/raw/$s.png" "$OUT/$s.png"
        KEPT+=("$s")
    else
        FAILED+=("$s")
    fi
done
mapfile -t FAILED < <(printf '%s\n' "${FAILED[@]}" | grep -v '^$' | sort -u)
log "written to $OUT: ${KEPT[*]:-nothing}"
[ ${#FAILED[@]} -eq 0 ] || { log "NOT taken (old picture kept): ${FAILED[*]}"; exit 1; }

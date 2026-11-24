#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# [mac-1] tools/release/fetch-mac.sh <run id> [--from arm64|x86_64] [--dist <dir>] [--keep]
#
# Fetches the macOS package that the "macOS" workflow (.github/workflows/macos.yml) built for a
# release commit, checks it here on Linux, and puts the archive into dist/ (default) so that
# tools/release.sh lists it in SHA256SUMS before signing. Needs the gh CLI (read access).
#   1. the run must be complete and green; its commit is compared with HEAD
#   2. downloads the artifact octomx-macos-package-built-on-<arch> (default: arm64, else x86_64;
#      both runners build the same universal app, each tested on its own kind of Mac)
#   3. the archive name must carry this checkout's version (OctoMX-<X.Y>-macos-universal.zip)
#   4. unpacks it (symbolic links kept), lists the structure, checks every file and link against
#      MANIFEST.txt (tools/mac/check-manifest.py), reads Info.plist back and prints the
#      signature lines that the run recorded (package-info.txt)
# Work files in build/mac-fetch/<run id>/ (deleted unless --keep).
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
RUN=${1:?usage: tools/release/fetch-mac.sh <run id> [--from arm64|x86_64] [--dist dir] [--keep]}
shift
FROM=""; DIST=$ROOT/dist; KEEP=0
while [ $# -gt 0 ]; do
  case "$1" in
    --from) FROM=$2; shift 2 ;;
    --dist) DIST=$2; shift 2 ;;
    --keep) KEEP=1; shift ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
W=$ROOT/build/mac-fetch/$RUN
rm -rf "$W"; mkdir -p "$W" "$DIST"
VFULL=$(sed -n 's/^project(OctoMX VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")
VS=$(echo "$VFULL" | cut -d. -f1-2)

# 1. the run
read -r STATUS CONCL SHA < <(gh run view "$RUN" --json status,conclusion,headSha --jq '"\(.status) \(.conclusion) \(.headSha)"')
echo "run $RUN: $STATUS / $CONCL, commit $SHA"
[ "$STATUS" = completed ] && [ "$CONCL" = success ] || { echo "the run is not complete and green" >&2; exit 1; }
HEAD=$(git -C "$ROOT" rev-parse HEAD)
[ "$SHA" = "$HEAD" ] && echo "commit = HEAD of this checkout" || echo "WARNING: the run built $SHA, this checkout is at $HEAD"

# 2. the artifact
ARTS=$(gh run view "$RUN" --json artifacts --jq '.artifacts[].name' 2>/dev/null || gh api "repos/{owner}/{repo}/actions/runs/$RUN/artifacts" --jq '.artifacts[].name')
for a in ${FROM:-arm64 x86_64}; do
  if grep -qx "octomx-macos-package-built-on-$a" <<<"$ARTS"; then FROM=$a; break; fi
done
[ -n "$FROM" ] && grep -qx "octomx-macos-package-built-on-$FROM" <<<"$ARTS" || { echo "no package artifact in run $RUN: $ARTS" >&2; exit 1; }
gh run download "$RUN" -n "octomx-macos-package-built-on-$FROM" -D "$W/art"
ZIP=$(ls "$W"/art/OctoMX-*-macos-*.zip | head -n 1)
NAME=$(basename "$ZIP" .zip)
echo "artifact octomx-macos-package-built-on-$FROM: $(basename "$ZIP"), $(du -h "$ZIP" | cut -f1), sha256 $(sha256sum "$ZIP" | cut -d' ' -f1)"

# 3. the version
case "$NAME" in OctoMX-$VS-macos-*) echo "version $VS: matches this checkout" ;; *) echo "the archive is $NAME, this checkout is version $VS" >&2; exit 1 ;; esac

# 4. structure, manifest, Info.plist, signature
python3 -I - "$ZIP" "$W/unpacked" <<'PY'
import os, stat, sys, zipfile
zf, dest = zipfile.ZipFile(sys.argv[1]), sys.argv[2]
links = files = 0
for i in zf.infolist():
    if i.filename.startswith('__MACOSX/'):
        continue
    p = os.path.join(dest, i.filename)
    if os.path.relpath(p, dest).startswith('..'):
        raise SystemExit('unsafe path ' + i.filename)
    mode = i.external_attr >> 16
    if stat.S_ISLNK(mode):
        os.makedirs(os.path.dirname(p), exist_ok=True)
        os.symlink(zf.read(i).decode(), p)
        links += 1
    elif i.is_dir():
        os.makedirs(p, exist_ok=True)
    else:
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, 'wb') as f:
            f.write(zf.read(i))
        if mode & 0o111:
            os.chmod(p, 0o755)
        files += 1
print('unpacked: %d files, %d symbolic links' % (files, links))
PY
P=$W/unpacked/$NAME
echo "top folder: $(ls "$W/unpacked" | tr '\n' ' ')"
echo "package folder: $(ls "$P" | tr '\n' ' ')"
echo "bundle: $(cd "$P/OctoMX.app/Contents" && ls | tr '\n' ' ')"
echo "frameworks: $(cd "$P/OctoMX.app/Contents/Frameworks" && ls | tr '\n' ' ')"
echo "plugins: $(cd "$P/OctoMX.app/Contents/PlugIns" && find . -type f | sed 's|^\./||' | sort | tr '\n' ' ')"
python3 -I "$ROOT/tools/mac/check-manifest.py" "$P"
python3 -I - "$P/OctoMX.app/Contents/Info.plist" <<'PY'
import plistlib, sys
d = plistlib.load(open(sys.argv[1], 'rb'))
for k in ('CFBundleIdentifier', 'CFBundleName', 'CFBundleExecutable', 'CFBundleShortVersionString', 'CFBundleVersion',
          'LSMinimumSystemVersion', 'NSHighResolutionCapable', 'NSLocalNetworkUsageDescription'):
    print('Info.plist %s = %s' % (k, d.get(k)))
print('Info.plist URL schemes = %s' % [s for t in d.get('CFBundleURLTypes', []) for s in t.get('CFBundleURLSchemes', [])])
print('Info.plist document types = %s' % [c for t in d.get('CFBundleDocumentTypes', []) for c in t.get('LSItemContentTypes', [])])
PY
[ -f "$W/art/package-info.txt" ] && sed -n '/^--- codesign --verify/,$p' "$W/art/package-info.txt" | head -n 20
cp "$ZIP" "$DIST/"
echo "copied to $DIST/$(basename "$ZIP"): tools/release.sh lists it in SHA256SUMS (same version), then signs"
[ $KEEP = 1 ] || rm -rf "$W"

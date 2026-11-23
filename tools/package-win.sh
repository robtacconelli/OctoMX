#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Make a self-contained Windows folder + zip from a cross build.
#
#   tools/package-win.sh [options] <build-dir> <out-dir>
#     -e <name.exe>   package only this exe (repeatable; default: every *.exe in
#                     <build-dir> outside CMakeFiles/ and *_autogen/)
#     -n <name>       folder/zip name (default: basename of the first exe)
#     --strip         strip debug info from the copied exe/dlls (smaller zip)
#     --no-zip        only create the folder
#     --with-tests    also package every Qt Test executable (tst_*.exe) of the build
#                     tree next to the -e exes, with their extra DLLs (Qt6Test.dll);
#                     dev packaging only. Without -e every exe is packaged anyway.
#
# Result: <out-dir>/<name>/ (exe, Qt DLLs, libc++/libunwind, plugins in
# platforms/ styles/ imageformats/ iconengines/ sqldrivers/ tls/ ...) and
# <out-dir>/<name>.zip. windeployqt is a Windows-only binary, so DLL
# dependencies are resolved here by walking PE imports (llvm-readobj).
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../toolchain/env.sh"

EXES=(); NAME=""; STRIP=0; ZIP=1; WITH_TESTS=0
while [ $# -gt 0 ]; do
  case "$1" in
    -e) EXES+=("$2"); shift 2 ;;
    -n) NAME=$2; shift 2 ;;
    --strip) STRIP=1; shift ;;
    --no-zip) ZIP=0; shift ;;
    --with-tests) WITH_TESTS=1; shift ;;
    -h|--help) sed -n 2,18p "$0"; exit 0 ;;
    *) break ;;
  esac
done
[ $# -eq 2 ] || { echo "usage: $0 [-e x.exe] [-n name] [--strip] [--no-zip] [--with-tests] <build-dir> <out-dir>" >&2; exit 2; }
BD=$(realpath "$1"); OUT=$(realpath -m "$2")

# --- which executables ---------------------------------------------------------
EXE_PATHS=()
if [ ${#EXES[@]} -gt 0 ]; then
  for e in "${EXES[@]}"; do
    f=$(find "$BD" -name "$e" -not -path '*/CMakeFiles/*' | head -n1)
    [ -n "$f" ] || { echo "error: $e not found in $BD" >&2; exit 1; }
    EXE_PATHS+=("$f")
  done
else
  while IFS= read -r f; do EXE_PATHS+=("$f"); done < <(find "$BD" -name '*.exe' \
      -not -path '*/CMakeFiles/*' -not -path '*_autogen/*' | sort)
fi
if [ $WITH_TESTS = 1 ] && [ ${#EXES[@]} -gt 0 ]; then
  # test executables: their imports (Qt6Test.dll, ...) are walked like the app's below
  while IFS= read -r f; do EXE_PATHS+=("$f"); done < <(find "$BD" -name 'tst_*.exe' \
      -not -path '*/CMakeFiles/*' -not -path '*_autogen/*' | sort)
fi
[ ${#EXE_PATHS[@]} -gt 0 ] || { echo "error: no .exe in $BD" >&2; exit 1; }
[ -n "$NAME" ] || NAME=$(basename "${EXE_PATHS[0]}" .exe)

DEST=$OUT/$NAME
rm -rf "$DEST"; mkdir -p "$DEST"

# --- DLL search path (target DLLs only) ------------------------------------------
SEARCH=("$QT_WIN_DIR/bin" "$LLVM_MINGW_ROOT/x86_64-w64-mingw32/bin" "$TP_WIN/bin" "$BD")
declare -A DLLMAP   # lowercase name -> path
for d in "${SEARCH[@]}"; do
  [ -d "$d" ] || continue
  while IFS= read -r f; do
    k=$(basename "$f" | tr 'A-Z' 'a-z')
    [ -n "${DLLMAP[$k]:-}" ] || DLLMAP[$k]=$f
  done < <(find "$d" -maxdepth 1 -iname '*.dll')
done
# DLLs built by the project itself (anywhere in the build tree) win over nothing else
while IFS= read -r f; do
  k=$(basename "$f" | tr 'A-Z' 'a-z'); [ -n "${DLLMAP[$k]:-}" ] || DLLMAP[$k]=$f
done < <(find "$BD" -iname '*.dll' -not -path '*/CMakeFiles/*')

imports_of() { # PE file -> imported dll names (normal + delay-load)
  llvm-readobj --coff-imports "$1" | sed -n 's/^ *Name: \(.*\.[dD][lL][lL]\)$/\1/p'
}

declare -A DONE     # lowercase dll name -> 1
declare -A MISSING
QUEUE=()
copy_dll() { # lowercase name
  local k=$1
  [ -z "${DONE[$k]:-}" ] || return 0
  DONE[$k]=1
  if [ -n "${DLLMAP[$k]:-}" ]; then
    cp -f "${DLLMAP[$k]}" "$DEST/"
    QUEUE+=("$DEST/$(basename "${DLLMAP[$k]}")")
  else
    case "$k" in
      api-ms-win-*|ext-ms-*) ;;               # UCRT / API sets: part of Windows 10+
      *) MISSING[$k]=1 ;;                     # assumed system DLL, reported below
    esac
  fi
}

for e in "${EXE_PATHS[@]}"; do cp -f "$e" "$DEST/"; QUEUE+=("$DEST/$(basename "$e")"); done

# --- Qt plugins ----------------------------------------------------------------------
QP=$QT_WIN_DIR/plugins
add_plugin() { # subdir file
  if [ -f "$QP/$1/$2" ]; then mkdir -p "$DEST/$1"; cp -f "$QP/$1/$2" "$DEST/$1/"; QUEUE+=("$DEST/$1/$2"); fi
}
ALL_IMPORTS=$(for e in "${EXE_PATHS[@]}"; do imports_of "$e"; done | tr 'A-Z' 'a-z' | sort -u)
uses() { grep -qx "$1" <<<"$ALL_IMPORTS"; }

if uses qt6gui.dll; then
  add_plugin platforms qwindows.dll
  for f in qgif.dll qico.dll qjpeg.dll qsvg.dll; do add_plugin imageformats $f; done
  add_plugin iconengines qsvgicon.dll
fi
uses qt6widgets.dll && add_plugin styles qmodernwindowsstyle.dll
uses qt6sql.dll && add_plugin sqldrivers qsqlite.dll
if uses qt6network.dll; then
  # schannel = native Windows TLS (no OpenSSL DLLs needed); certonly = fallback
  add_plugin tls qschannelbackend.dll
  add_plugin tls qcertonlybackend.dll
  add_plugin networkinformation qnetworklistmanager.dll
fi
if uses qt6multimedia.dll; then
  add_plugin multimedia ffmpegmediaplugin.dll
  add_plugin multimedia windowsmediaplugin.dll
fi

# --- walk imports recursively -----------------------------------------------------------
i=0
while [ $i -lt ${#QUEUE[@]} ]; do
  f=${QUEUE[$i]}; i=$((i+1))
  while IFS= read -r dll; do
    [ -n "$dll" ] && copy_dll "$(tr 'A-Z' 'a-z' <<<"$dll")"
  done < <(imports_of "$f")
done

cat > "$DEST/qt.conf" <<'EOF'
[Paths]
Prefix = .
Plugins = .
EOF

if [ $STRIP = 1 ]; then
  find "$DEST" \( -iname '*.exe' -o -iname '*.dll' \) -exec llvm-strip --strip-debug {} \;
fi

echo "packaged: $DEST"
echo "  executables: $(for e in "${EXE_PATHS[@]}"; do basename "$e"; done | tr '\n' ' ')"
echo "  dlls copied: $(find "$DEST" -maxdepth 1 -iname '*.dll' | wc -l), plugins: $(find "$DEST" -mindepth 2 -iname '*.dll' | wc -l)"
echo "  system dlls (not copied): $(printf '%s\n' "${!MISSING[@]}" | sort | tr '\n' ' ')"

if [ $ZIP = 1 ]; then
  rm -f "$OUT/$NAME.zip"
  (cd "$OUT" && 7z a -tzip -mx=7 "$NAME.zip" "$NAME" >/dev/null)
  echo "  zip: $OUT/$NAME.zip ($(du -h "$OUT/$NAME.zip" | cut -f1))"
fi

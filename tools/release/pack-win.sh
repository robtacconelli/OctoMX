#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tools/release/pack-win.sh <build-dir> <dest-dir> <symbols-dir>
# Minimal Windows release folder for octomx.exe (called by tools/release.sh).
#
# Unlike tools/package-win.sh (dev packaging: every plugin family "just in case"), this ships only
# what the app actually loads (checked with WINEDEBUG=+loaddll by verify-win.sh):
#   platforms/qwindows.dll        the Windows QPA
#   tls/qschannelbackend.dll      Qt TLS via the OS (no OpenSSL DLLs; OpenSSL is static in libtorrent)
#   sqldrivers/qsqlite.dll        the profile database
#   imageformats/qico.dll         .ico read/write (--export-icons, icon conversions)
# Not shipped: styles/ (the app uses the built-in "Windows" style under its ClassicStyle proxy),
# iconengines/qsvg (no SVG), networkinformation/, multimedia/, opengl32sw.dll, d3dcompiler, translations.
# DLLs are found by walking PE imports (llvm-readobj) from the exe and plugins; every import must be
# either shipped or a known Windows system DLL, otherwise this script fails.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../../toolchain/env.sh"
BD=$(realpath "$1"); DEST=$(realpath -m "$2"); SYM=$(realpath -m "$3")
EXE=$BD/bin/octomx.exe
[ -f "$EXE" ] || { echo "no $EXE" >&2; exit 1; }
rm -rf "$DEST"; mkdir -p "$DEST" "$SYM"

# the exe: split debug info into symbols/, ship a fully stripped copy with a debuglink
llvm-objcopy --only-keep-debug "$EXE" "$SYM/octomx.exe.debug"
llvm-strip --strip-all -o "$DEST/octomx.exe" "$EXE"
(cd "$SYM" && llvm-objcopy --add-gnu-debuglink=octomx.exe.debug "$DEST/octomx.exe")

QP=$QT_WIN_DIR/plugins
PLUGINS=(platforms/qwindows.dll tls/qschannelbackend.dll sqldrivers/qsqlite.dll imageformats/qico.dll)
for p in "${PLUGINS[@]}"; do
  mkdir -p "$DEST/$(dirname "$p")"; cp "$QP/$p" "$DEST/$p"
done

SEARCH=("$QT_WIN_DIR/bin" "$LLVM_MINGW_ROOT/x86_64-w64-mingw32/bin" "$TP_WIN/bin")
# Windows 10/11 system DLLs (plus the UCRT API sets): never shipped
SYSTEM_RE='^(api-ms-win-.*|ext-ms-.*|advapi32|authz|bcrypt|comctl32|comdlg32|crypt32|d3d11|d3d12|d3d9|dcomp|dnsapi|dwmapi|dwrite|dxgi|gdi32|imm32|iphlpapi|kernel32|mpr|msvcrt|mswsock|ncrypt|netapi32|ntdll|ole32|oleaut32|opengl32|secur32|setupapi|shcore|shell32|shlwapi|user32|userenv|usp10|uxtheme|version|winhttp|winmm|winspool\.drv|wintrust|wtsapi32|ws2_32|dbghelp|mscms|d2d1|msimg32|oleacc|uiautomationcore|dxcore|bcryptprimitives|wevtapi|hid|xinput1_4|cfgmgr32|powrprof|propsys|rpcrt4|sspicli|wldap32|normaliz|runtimeobject|dxva2|mf|mfplat|mfreadwrite|wininet|urlmon)(\.dll)?$'
imports_of() { llvm-readobj --coff-imports "$1" | sed -n 's/^ *Name: \(.*\.[dD][lL][lL]\)$/\1/p'; }
find_dll() { local d; for d in "${SEARCH[@]}"; do
  local f; f=$(find "$d" -maxdepth 1 -iname "$1" | head -n1); [ -n "$f" ] && { echo "$f"; return 0; }; done; return 1; }

declare -A SEEN SYSTEM
QUEUE=("$DEST/octomx.exe")
for p in "${PLUGINS[@]}"; do QUEUE+=("$DEST/$p"); done
i=0; FAILED=0
: > "$SYM/../win-imports.txt"
while [ $i -lt ${#QUEUE[@]} ]; do
  f=${QUEUE[$i]}; i=$((i+1))
  echo "${f#$DEST/}: $(imports_of "$f" | tr '\n' ' ')" >> "$SYM/../win-imports.txt"
  while IFS= read -r dll; do
    k=$(tr 'A-Z' 'a-z' <<<"$dll")
    [ -z "${SEEN[$k]:-}" ] || continue
    SEEN[$k]=1
    if grep -qiE "$SYSTEM_RE" <<<"$k"; then SYSTEM[$k]=1; continue; fi
    # not on the list: a DLL that Windows itself provides (wine's system32 mirrors it)?
    if [ -e "$WINEPREFIX/drive_c/windows/system32/$k" ] && ! find_dll "$dll" >/dev/null; then
      SYSTEM[$k]=1; echo "note: $dll treated as a Windows system DLL (present in system32, not in Qt/llvm-mingw)"; continue
    fi
    if src=$(find_dll "$dll"); then
      cp "$src" "$DEST/"; QUEUE+=("$DEST/$(basename "$src")")
    else
      echo "ERROR: $dll (imported by ${f#$DEST/}) is neither shipped nor a known system DLL"; FAILED=1
    fi
  done < <(imports_of "$f")
done
[ $FAILED = 0 ] || exit 1

# third-party DLLs: drop debug sections (libc++/libunwind carry DWARF); our exe is already stripped
find "$DEST" -iname '*.dll' -exec llvm-strip --strip-debug {} \;

cat > "$DEST/qt.conf" <<'EOF'
[Paths]
Prefix = .
Plugins = .
EOF
# fonts: the UI font is compiled into octomx.exe as a Qt resource; the same files ship separately
# so the LGPL-2.1 font can be inspected and replaced (see licenses/)
mkdir -p "$DEST/fonts"
cp "$OCTOMX_ROOT/src/ui/res/fonts/tahoma.ttf" "$OCTOMX_ROOT/src/ui/res/fonts/tahomabd.ttf" "$DEST/fonts/"

echo "shipped: octomx.exe + $(find "$DEST" -maxdepth 1 -iname '*.dll' | wc -l) DLLs: $(cd "$DEST" && ls *.dll | tr '\n' ' ')"
echo "plugins: ${PLUGINS[*]}"
echo "system DLLs imported (not shipped): $(printf '%s\n' "${!SYSTEM[@]}" | sort | tr '\n' ' ')"
echo "symbols: $SYM/octomx.exe.debug ($(du -h "$SYM/octomx.exe.debug" | cut -f1)); exe $(du -h "$DEST/octomx.exe" | cut -f1)"

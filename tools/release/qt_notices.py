#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write the Qt part of THIRD-PARTY-NOTICES from Qt's own SBOM (called by tools/release/notices.sh).

    qt_notices.py <qtbase .spdx.json> <qt licence texts dir> <out notices file> <out licences dir>

Keeps the third-party components compiled into the Qt libraries and plugins we ship (Core, Gui,
Widgets, Network, Sql, DBus, the xcb / windows QPA, the SQLite driver, bundled PCRE2 / HarfBuzz /
libpng / FreeType / zlib); drops build-time-only parts (Bootstrap, Test, Wayland, CMake helpers,
JPEG, qjpeg is not shipped) and system libraries Qt merely links to (Wrap*, X11, ICU ... are
listed separately by notices.sh). Every licence ID referenced is copied as <id>.txt.
"""
import json, re, shutil, sys, os

sbom, textdir, out, licdir = sys.argv[1:5]
d = json.load(open(sbom, encoding="utf-8"))
extracted = {x["licenseId"]: x.get("extractedText", "") for x in d.get("hasExtractedLicensingInfos", [])}

KEEP = re.compile(r"^(Core|Gui|Widgets|Network|Sql|DBus|XcbQpaPrivate|XcbQpa|QXcbIntegrationPlugin|"
                  r"QWindowsIntegrationPlugin|QSQLiteDriverPlugin|BundledPcre2|BundledHarfbuzz|HarfbuzzPrivate|"
                  r"BundledLibpng|PngPrivate|BundledFreetype|FreetypePrivate|BundledZLIB|ZlibPrivate)(_|$)")
DROP = re.compile(r"^(Bootstrap|Test|TestInternals|Wayland|QWayland|Platform_Attribution|Wrap|BundledLibjpeg|JpegPrivate)")

rows, seen = [], set()
for p in d["packages"]:
    n = p.get("name", "")
    if DROP.match(n) or not KEEP.match(n):
        continue
    if n.endswith("Private") and n.replace("Private", "") in seen:
        continue
    m = re.search(r"\n\s*Name: (.+)", p.get("comment", ""))
    title = m.group(1).strip() if m else n
    key = (title, p.get("versionInfo", ""))
    if key in seen:
        continue
    seen.add(key); seen.add(n)
    rows.append((title, n, p.get("versionInfo", ""), p.get("licenseConcluded", "NOASSERTION"),
                 p.get("copyrightText", "").strip(), p.get("downloadLocation", "NOASSERTION")))

ids = set()
with open(out, "w", encoding="utf-8") as f:
    for title, n, ver, lic, cr, url in sorted(rows, key=lambda r: r[0].lower()):
        f.write(f"* {title}" + (f" {ver}" if ver and ver != "unknown" else "") + f"   [in Qt target {n.split('_')[0]}]\n")
        f.write(f"  Licence: {lic}\n")
        if url and url != "NOASSERTION":
            f.write(f"  Source: {url}\n")
        if cr and cr != "NOASSERTION":
            f.write("  " + cr.replace("\n", "\n  ") + "\n")
        f.write("\n")
        # Qt itself is used under LGPL-3.0-only (the commercial / GPL alternatives are not copied)
        ids.update(t for t in re.split(r"[\s()]+", lic)
                   if t and t not in ("AND", "OR", "WITH", "NOASSERTION", "LicenseRef-Qt-Commercial"))

os.makedirs(licdir, exist_ok=True)
missing = []
for i in sorted(ids):
    src = os.path.join(textdir, i + ".txt")
    if os.path.exists(src):
        shutil.copy(src, os.path.join(licdir, i + ".txt"))
    elif i in extracted:
        open(os.path.join(licdir, i + ".txt"), "w", encoding="utf-8").write(extracted[i])
    else:
        missing.append(i)
print(f"qt components: {len(rows)}, licence ids: {' '.join(sorted(ids))}")
if missing:
    print("MISSING licence texts: " + " ".join(missing))
    sys.exit(1)

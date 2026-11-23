#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Snapshot the source tree (not toolchains/builds) into .snapshots/<timestamp>[-label].tar.zst
# Usage: tools/snapshot.sh [label]
set -e
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
mkdir -p "$ROOT/.snapshots"
name="$(date +%Y%m%d-%H%M%S)${1:+-$1}"
cd "$ROOT"
tar --exclude='./toolchain' --exclude='./third_party' --exclude='./build' --exclude='./dist' \
    --exclude='./.snapshots' --exclude='./reference' \
    -cf - . | gzip -1 > ".snapshots/$name.tar.gz"
ls -la ".snapshots/$name.tar.gz"
# keep the 40 most recent
ls -1t .snapshots/*.tar.gz | tail -n +41 | xargs -r rm -f

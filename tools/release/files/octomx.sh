#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# OctoMX launcher for the Linux bundle. bin/octomx also runs directly (it finds ../lib through its
# $ORIGIN runpath and ../plugins through bin/qt.conf); this script only picks a platform plugin the
# bundle has: it ships xcb (X11 / XWayland) and offscreen, not the native Wayland plugin.
HERE=$(dirname "$(readlink -f "$0")")
case "${QT_QPA_PLATFORM:-}" in
  xcb*|offscreen*|minimal*) ;;
  *) QT_QPA_PLATFORM=xcb; export QT_QPA_PLATFORM ;;
esac
exec "$HERE/bin/octomx" "$@"

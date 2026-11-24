#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# [net-2] Runs tst_netpolicy_netns inside an unprivileged network namespace (no root needed:
# `unshare -rn` makes the user root of a namespace of its own). Two dummy devices: vpn0 (10.77.0.2,
# the default route: the "VPN") and lan0 (10.88.0.2, the other line). Nothing reaches the real
# network from inside. Without unshare / user namespaces (CI, macOS, Windows) the test is skipped.
set -u
bin="$1"
if ! command -v unshare >/dev/null 2>&1 || ! unshare -rn true 2>/dev/null; then
    echo "SKIP: no unprivileged network namespaces here"
    exec "$bin"
fi
exec unshare -rn bash -c '
    set -e
    ip link set lo up
    ip link add vpn0 type dummy; ip addr add 10.77.0.2/24 dev vpn0; ip link set vpn0 up
    ip link add lan0 type dummy; ip addr add 10.88.0.2/24 dev lan0; ip link set lan0 up
    ip route add default dev vpn0
    MX_NETNS=1 exec "$0"
' "$bin"

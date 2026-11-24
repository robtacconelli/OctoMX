#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# [net-2] Live smoke run: the real program behind a SOCKS5 proxy, with a capture that shows every
# packet it tries to send other than to the proxy. Not a ctest (needs the internet).
#
#   tests/netpolicy/live/proxy_smoke.sh <build dir> <work dir> [seconds]
#
# * The proxy (socks_bridge.py proxy) runs in this namespace and reaches the internet.
# * join_probe (the real core with every network, no GUI) runs in a new user network namespace whose
#   only way out is 127.0.0.1:1080, a bridge to the proxy's Unix socket. Its default route points at
#   a dummy device (blackhole), so a direct connection attempt or datagram, DNS included, becomes a
#   packet there; tcpdump records that device and loopback (all but the proxy port).
# * It searches "big buck bunny", downloads a merged row for a while, then cancels and deletes it.
# Only counts are printed (no addresses).
set -u
build="$1"; work="$2"; seconds="${3:-150}"
here="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$work"
rm -f "$work"/*.pcap "$work"/proxy-targets.log
python3 "$here/socks_bridge.py" proxy "$work/socks.sock" "$work/proxy-targets.log" > "$work/proxy.out" 2>&1 &
proxy=$!
for i in $(seq 50); do [ -S "$work/socks.sock" ] && break; sleep 0.1; done
unshare -rn bash -c '
    set -u
    work="$1"; build="$2"; here="$3"; seconds="$4"
    ip link set lo up
    ip link add wan0 type dummy; ip addr add 10.99.0.2/24 dev wan0; ip link set wan0 up
    ip route add default via 10.99.0.1
    python3 "$here/socks_bridge.py" bridge 1080 "$work/socks.sock" > "$work/bridge.out" 2>&1 &
    bridge=$!
    tcpdump -Z root -U -n -i wan0 -w "$work/wan0.pcap" > "$work/tcpdump-wan0.out" 2>&1 &
    td1=$!
    tcpdump -Z root -U -n -i lo -w "$work/lo.pcap" "not port 1080" > "$work/tcpdump-lo.out" 2>&1 &
    td2=$!
    sleep 1
    MX_OVERLAY_DISCOVERY=0 MX_PORT_MAPPING=0 "$build/bin/join_probe" --profile "$work/profile" --seconds "$seconds" \
        --set connection/proxyType=socks5 --set connection/proxyHost=127.0.0.1 --set connection/proxyPort=1080 \
        --set connection/proxyHostnames=true --out "$work/probe.out" > "$work/probe.log" 2>&1
    echo "probe exit $?" >> "$work/probe.log"
    sleep 1
    kill $td1 $td2 $bridge 2>/dev/null
    wait 2>/dev/null
' bash "$work" "$build" "$here" "$seconds"
kill $proxy 2>/dev/null
wait $proxy 2>/dev/null
rm -f "$work/socks.sock"
echo "packets on the blackhole route (direct attempts): $(tcpdump -r "$work/wan0.pcap" -n 2>/dev/null | wc -l)"
echo "packets on loopback other than the proxy port:   $(tcpdump -r "$work/lo.pcap" -n 2>/dev/null | wc -l)"
echo "proxy CONNECT requests: $(grep -c '^TCP' "$work/proxy-targets.log" 2>/dev/null), by name: $(grep -c '^TCP name' "$work/proxy-targets.log" 2>/dev/null), UDP associate asked: $(grep -c '^UDP' "$work/proxy-targets.log" 2>/dev/null)"

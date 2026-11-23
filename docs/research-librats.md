# Research: librats / Rats Search for OctoMX

Task `research-librats`, 2026-11-20. Sources read: `third_party/src/rats-search` (v2.3.6, 826b2bd),
`third_party/src/librats` (HEAD 9eefd0a, 2026-11-18) and `third_party/src/librats-pinned`
(364169f, 2026-10-27: the commit rats-search v2.3.6 pins as its `src/librats` submodule).
PoC: `tests/research-librats/` (one binary, `rats_poc`, with three modes). Raw logs:
`build/research-librats/logs/`.

**Legend.** **[ran]** = I executed it and the evidence is quoted. **[code]** = taken from reading the
source. **[inferred]** = my judgement.

---

## 0. TL;DR

| Use | Recommendation | Main reason |
|---|---|---|
| 1. Keyword search over other Rats Search clients | **Embed librats as-is** (pinned commit, library only, `RATS_SEARCH_FEATURES=OFF`) and speak the rats-search JSON messages ourselves | **[ran]** It works today. Cold start: first peer at about 10 s and first result 100 ms after that. 30+ peers within 30 s. Warm start (persisted peers): 19 peers in 5 s. Hundreds of results per keyword. Reimplementing Noise-XX + the QUIC-like UDP transport + identify + DHT discovery to be wire-compatible would take months. |
| 2. Our own OctoMX overlay (rooms, PMs, browse, hash registry) | **Embed librats as-is**, using the same library as for use 1 but a separate `Node` with its own protocol string, behind our own `Overlay` interface | **[ran]** GossipSub across 2 hops, direct JSON PMs, mDNS LAN discovery in 0.1 s and Noise encryption all work. Cross-compiles with llvm-mingw unchanged. MIT. No dependencies. Adds about 1.5-2.2 MB. Risk: one maintainer and very fast churn, so pin and wrap it. |
| 3. Local DHT crawling / indexing | **Do not use librats for this. Reimplement on libtorrent-rasterbar** (which we already ship) with BEP 51 + passive announce alerts + BEP 9. Store in **SQLite FTS5**, not Manticore. | librats' crawler needs `RATS_SEARCH_FEATURES`, i.e. a second full BitTorrent stack (10.5 k lines), and it uses node-id spoofing plus skipping the announce token. Manticore is an external `searchd` daemon reached through the QMYSQL driver. |

---

## 1. What librats is (facts)

* **Size [code]**: 69.5 k lines of C/C++ under `src/` (pinned). By area: transport 14.6 k, bittorrent 10.6 k
  (optional), subsystems 8.6 k, core 6.5 k, crypto 5.6 k (curve25519-donna, chacha20-poly1305,
  sha2, blake2, hkdf: all bundled), util 5.1 k, dht 4.8 k, node 3.3 k, nat 3.0 k, storage 2.1 k (optional).
* **Dependencies [ran]**: none except the C++17 standard library and threads. On Windows it adds `ws2_32 iphlpapi bcrypt advapi32`.
  No OpenSSL, no Boost, no asio. Network I/O runs on its own reactor (epoll / kqueue / IOCP).
* **Licence [code]**: MIT. Bundled parts are curve25519-donna (BSD-3), poly1305-donna (MIT/PD), noise-c (MIT) and
  ifaddrs-android (BSD, Android only). See `third_party/src/librats/THIRD_PARTY_NOTICES.md`. rats-search is MIT.
* **Maturity [ran: `gh api`]**: the repo was created 2025-07-11 and has **955 commits, 937 of them by one developer (DEgITx)**.
  It has 170 stars. Releases 3.0.2→3.0.6 came out between 2026-10-20 and 2026-11-10, i.e. every 3-7 days.
  In those weeks the public API changed (`DhtDiscovery` gained multi-key and rendezvous support, mDNS loopback was fixed). The
  wire changed too: rats-search v2.3.6 (2026-11-02) says *"Updated to latest librats with new protocol"*, and
  its protocol string is `rats-search/4`.
  rats-search has 2,086 stars. v2.3.6 has 2,697 downloads in 18 days and v2.3.5 had 1,480.
  The code itself is careful: it has extensive comments and about 107 gtest files.
* **Build [ran]**:
  * Linux, g++ 15, Release, library + reference client: **16 s wall** at -j8. `librats.a` is 6.6 MB (static, PIC).
    2 warnings in the pinned tree. HEAD has a few `-Wpedantic`/`-Wmissing-field-initializers` warnings.
  * Windows, llvm-mingw 20260602 (clang 22, UCRT), with our `cmake/toolchain-win64.cmake`: **builds unchanged, 21 s,
    2 warnings** (`-Wunused-lambda-capture`). `librats.a` is 7.1 MB. The PoC .exe imports only
    KERNEL32, WS2_32, IPHLPAPI, ADVAPI32 and the UCRT api-sets.
  * **The Windows .exe works on the live network under wine [ran]**: 32 peers, 794 unique results in 180 s
    (`logs/search-win-1.log`).
  * Binary cost [ran]: the stripped PoC is 2.2 MB on Linux and 2.3 MB on Windows. A static hello-world with
    libc++ is 0.79 MB, so **librats adds roughly 1.5 MB to a Windows exe**.
  * Winsock/POSIX [code + ran]: every socket call goes through `core/socket.*` with `#ifdef _WIN32` branches
    (WSAGetLastError, closesocket, `char*` setsockopt casts). The poller is IOCP on Windows and the network monitor uses
    iphlpapi. Nothing POSIX-only reaches the Windows build. It ships `version.rc.in` for MSVC/mingw.
* **Qt clash [code]**: `librats::EventBus` has a method called `emit()`. rats-search wraps every librats include
  in `#pragma push_macro("emit")/("slots")/("signals")` + `#undef`. We must do the same, or build that TU
  with `QT_NO_KEYWORDS`.

### Public API shape (what we would use)

```cpp
#include <librats/node/node.h>                 // Node, NodeConfig, Peer, PeerId
#include <librats/subsystems/dht_discovery.h>  // mainline-DHT rendezvous
#include <librats/subsystems/mdns_discovery.h> // LAN discovery
#include <librats/subsystems/message_json.h>   // on(type,cb) / send(peer,type,json) / send(type,json)=broadcast
#include <librats/subsystems/pubsub.h>         // GossipSub: subscribe/publish/set_validator
#include <librats/subsystems/reconnection.h>   // persisted peer book (peers.json), re-dial with backoff
#include <librats/subsystems/peer_exchange.h>  // PEX
#include <librats/subsystems/hole_punch.h>     // UDP hole punching (DCUtR-like)
#include <librats/subsystems/relay.h>          // relayed circuits (client; serving is opt-in)
#include <librats/subsystems/file_transfer.h>  // offer/accept file streaming with resume + SHA-256
#include <librats/subsystems/port_mapping_service.h> // UPnP-IGD + NAT-PMP

librats::NodeConfig cfg;              // listen_port, protocol, security=Noise, data_dir, max_peers, ...
librats::Node node(cfg);
auto* dht  = node.add_subsystem(std::make_unique<librats::DhtDiscovery>(dhtCfg)); // BEFORE start()
auto* json = node.add_subsystem(std::make_unique<librats::MessageJson>());
node.on_peer_connected([](const librats::Peer&){});                 // BEFORE start(), reactor thread
json->on("type", [](const librats::PeerId&, const librats::Json&){}); // reactor thread
node.start();  ...  json->send(peerId, "type", j);  ...  node.stop();
```

The full composition is in `tests/research-librats/main.cpp:run_search()`, which mirrors
`rats-search/src/net/p2p_transport.cpp`.

---

## 2. Use 1: keyword search against running Rats Search clients

### 2.1 How the rats network works [code]

* **Transport identity**: `NodeConfig::protocol = "rats-search/4"`, `security = Noise`
  (`rats-search/src/net/p2p_transport.cpp:309`). The protocol string goes into the Noise-XX prologue, so
  a node with a different string *cannot complete a handshake*. The string also names the DHT rendezvous key.
* **Peer id**: SHA-256 of the node's Curve25519 public key. The key is stored as 32 raw bytes in `<data_dir>/identity.key`.
* **Rendezvous on the mainline BitTorrent DHT**: `info_hash = SHA1("rats-search/4") =
  278903a7e96d10ceec7ddefb8b9bcdd36305346c` (`DhtDiscovery::hash_for_key`). Each node `announce_peer`s
  that hash every 10 min and `get_peers` it every 30 s, then dials every address it gets back.
  Over IPv4 the port it announces is the UPnP-mapped one if there is one, otherwise the STUN-reflexive port of the shared UDP socket
  (STUN server: `stun.l.google.com:19302` and others), otherwise the listen port.
* **DHT bootstrap nodes** (`dht/node.cpp: default_bootstrap_nodes`): `router.bittorrent.com:6881`,
  `dht.transmissionbt.com:6881`, `router.utorrent.com:6881`, `dht.libtorrent.org:25401`,
  `dht.aelitis.com:6881`. The routing table is persisted to `<data_dir>/dht_routing.json` and `dht_routing_v6.json`.
* **Other discovery**: mDNS (`_librats._tcp.local.`), PEX (peers swap peer lists), and
  `ReconnectionService` (re-dials everyone remembered in `<data_dir>/peers.json`).
* **Ports**: rats-search defaults are `p2pPort 4444` (TCP+UDP, librats) and `dhtPort 6881` (UDP). One UDP socket carries
  all peer traffic (a QUIC-like transport, `transport/udp_connection.*`). TCP is raced after 1.2 s. Any port
  works. Peers just dial what was announced.
* **Framing**: `[u32 len][Noise-sealed inner]`, inner = `[u8 type][u8 flags][u16 channel][payload]`.
  `MessageType::Typed (7)` = MessageJson: payload `[u16 type_len][type bytes][compact JSON text]`.
  The sender is the authenticated PeerId, never a field in the payload.

### 2.2 The search conversation (rats-search `peer/peer_api.cpp`, `services/peer_registry.cpp`, `ui/mainwindow.cpp`) [code, confirmed on the wire]

Every message is MessageJson `type` + a JSON object. These are the exact field names the 2.3.6 peers sent and accepted in our runs.

| Direction | type | payload |
|---|---|---|
| both, on connect | `client_info` | `{clientVersion:str, torrents:int64, files:int64, totalSize:int64, peersConnected:int, connectedAt:int64 ms, databaseSharing:bool}` (legacy readers also accept `torrentsCount`/`filesCount`) |
| us → peer (rats-search *broadcasts* to all connected peers) | `searchTorrent` | `{text:str, query:str (same), limit:int (GUI sends 50; default 10), index:int offset, orderBy:str ("" / "seeders" / "added" / ...), orderDesc:bool, safeSearch:bool, type?:str content type, size?:{min?,max?} bytes, files?:{min?,max?}}`. Legacy form wraps the options in `navigation:{...}`. **The query must be >2 chars or it is ignored.** |
| us → peer | `searchFiles` | same as `searchTorrent`, but matches file paths inside torrents |
| peer → us, **one message per hit** | `searchTorrent_response` | `{hash:str40 lowercase hex, name:str, size:int64, files:int, pieceLength:int, added:int64 ms, contentType:str ("video","audio","books","pictures","software","games","archive","bad"), contentCategory:str ("movie","series",...,"xxx"), seeders:int, leechers:int, completed:int, trackersChecked:int64 ms, good:int, bad:int}`. **No file list and no echo of the query.** |
| peer → us | `searchFiles_response` | as above + `path:[str]` (matching paths, legacy key), plus `matchingPaths`/`fileMatch` |
| us → peer | `torrent` | `{hash:str40, options:{files:true}}` (the GUI sends this when a remote hit is opened) |
| peer → us | `torrent_response` | Torrent JSON as above + `files_list:[{path:str, size:int64}]` (+ `info:{...}` if the peer has it). No reply at all on a miss. |
| peer → us on connect | `feed` `{localSize, localFeedDate}` → expects `feed_response {feed:[torrent...], feedDate, size}` | |
| peer → us on connect | `randomTorrents {limit:5}` → expects `randomTorrents_response {torrents:[torrent+files_list]}` | replication. We may answer or ignore it. |
| other | `topTorrents`/`_response`, `torrentAnnounce` (unsolicited new torrent), `databaseRequest`/`_response`/`databaseProgress`/`databaseCancel` (bulk DB dump via FileTransfer) | |

Because replies do not echo the query, a client that has several searches in flight has to correlate by name match or
by issuing one query at a time per peer. rats-search shows every hit for the "current" query. The PoC matches
the keyword tokens against `name` and tags `searchFiles` hits as `kw='?'` (they match inside paths).

### 2.3 What happened on the live network [ran]

All runs were from one host behind a home NAT (public IP shown as the documentation address 203.0.113.7; librats reported
`nat=endpoint-independent`). UPnP was not enabled.

| run | binary | duration | first peer | first hit | peers ≤15 s / ≤30 s / ≤60 s | peers total | unique results |
|---|---|---|---|---|---|---|---|
| `search-1` (cold, empty data dir) | Linux, pinned librats | 600 s | 9.8 s | 9.9 s (106 ms after query) | 8 / 30 / 32 | 34 | 855 (ubuntu 467, sintel 33, big buck bunny 15, file-path matches 340) |
| `search-win-1` (cold) | **Windows .exe under wine** | 180 s | 10.1 s | 10.2 s | 11 / 30 / 32 | 32 | 794 |
| `search-head-1` (cold) | Linux, **librats HEAD** | 150 s | 15.1 s | 15.2 s | 0 / 31 / 31 | 32 | 781 → HEAD still talks to the 2.3.6 network |
| `search-warm-1` (reused data dir: identity + peers.json + dht tables) | Linux, pinned | 75 s | **0.32 s** | **0.42 s** | 29 at ≤15 s (19 at ≤5 s) | 31 | - |

Excerpt (`logs/search-1.log`):

```
+   9.79s PEER+ e3cc9e34 89.179.74.229:4444 via tcp (dir out) peers=1
+   9.89s INFO  e3cc9e34 clientVersion=2.3.6.1 torrents=2245897 files=101847889 totalSize=10153.9TB peersConnected=86 databaseSharing=yes
+   9.90s HIT   [torrent] kw='ubuntu' 90758bf8...3f47 4.6GB files=1 S/L=2/0 ubuntu_pack-22.04-amd64.iso  <- e3cc9e34 106ms
+  10.22s HIT   [torrent] kw='ubuntu' c8295ce6...4862a 5.3GB files=1 S/L=55/1 ubuntu-25.10-desktop-amd64.iso  <- e3cc9e34 423ms
+  13.48s HIT   [torrent] kw='sintel' 6a9759bf...f3c35d 123.3MB files=1 S/L=19/8 sintel.mp4  <- e3cc9e34 3683ms
+  18.88s HIT   [torrent] kw='sintel' 08ada5a7a6183aae1e09d831df6748d566095a10 123.3MB files=11 S/L=48/3 Sintel  <- 0491bb00 5009ms
```
(`logs/search-win-1.log`): `HIT [torrent] kw='big buck bunny' dd8255ecdc7ca55fb0bbf81323d87062db1f6d1c 263.6MB files=3 S/L=148/11 Big Buck Bunny`.
(`logs/search-warm-1.log`):
```
+   0.42s RAW   [torrent] {"added":1742510689000,"bad":0,"completed":1,"contentCategory":"","contentType":"video","files":1,"good":0,"hash":"2f24d03eab998ca672b8c1ef567a184609236c02","leechers":0,"name":"Big.Buck.Bunny.2008.x264.BDRip.1080p.mkv","pieceLength":1048576,"seeders":1,"size":648602823,"trackersChecked":1742750058000}
+   2.70s DETAIL 5df30f25 2f24d03e... files_list=1 keys=[added,bad,completed,contentCategory,contentType,files,files_list,good,hash,leechers,name,pieceLength,seeders,size,trackersChecked,] 2275ms
```

* **Latency**: the first result from a peer usually arrives 50-1,300 ms after the query (most peers 100-700 ms).
  Each peer returns up to `limit` hits per query and per type (about 45-50 for "ubuntu" with limit 50). `torrent` detail requests took 2.3 s and 8.2 s.
* **Peers we talked to (search-1)**: 34 connected. 31 reported `clientVersion` 2.3.6.x, 1 reported 2.0.0.1 and 2 sent no client_info.
  30 of them advertise a non-empty index, **0.05 M to 10.6 M torrents each, 73.6 M summed** (the indexes overlap heavily).
  32 of 34 links used UDP, 2 used TCP. 25 were inbound (they found our announce), 9 outbound.
  ~600 dial failures in 10 min: these are stale or NAT-ed DHT entries, plus junk returned by DHT nodes. They are harmless.
* **The peer count plateaued at about 32.** That is our own cap (`max_peers=30` inbound, `PeerExchange.peer_target=30`), not a property of the network.
* **Population [ran, `rats_poc dhtprobe`]**: a standalone DHT client running `get_peers` for 280 s collected
  **390 endpoints / 119 distinct IPs announced under SHA1("rats-search/4")**. In comparison, `rats-search/3` had 5,
  `rats-search/2` had 1 and `librats/1.0` had 5 (`logs/dhtprobe-1.log`, `logs/dhtprobe-1-rs4-endpoints.txt`). 67 of the endpoints use the default
  port 4444. Endpoints are inflated by NAT port changes. Distinct IPs are a lower bound for "nodes online in the last
  ~30 min", so **roughly 100-150 reachable Rats Search nodes right now** [inferred]. The old protocol versions are dead,
  so the network moves to each new protocol string within days.

**Conclusion for use 1**: a fresh OctoMX gets torrent results from minute one, usually within about 10-15 s of start,
and in under 1 s on later starts if `data_dir` is persisted.

### 2.4 Recommendation for use 1: embed librats as-is

Why not reimplement the protocol: wire compatibility means Noise_XX with librats' exact prologue and
identity derivation, the identify control message, the `UdpConnection` QUIC-like packet format with its streams,
recovery and masking, TCP block framing, MessageRouter types, and the rendezvous logic. That is about 25 k lines that change
from week to week. Embedding the library costs about 1.5 MB and a 20 s build.

What to embed: `third_party/src/librats-pinned` as an `add_subdirectory` with BitTorrent/storage/bindings/client/tests off.
**Pin to the exact commit the current rats-search release pins.** HEAD also worked today, but nothing guarantees that.

### 2.5 Integration recipe (use 1)

**CMake** (same as `tests/research-librats/CMakeLists.txt`):
```cmake
set(RATS_BUILD_CLIENT OFF CACHE BOOL "" FORCE)
set(RATS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(RATS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(RATS_BINDINGS OFF CACHE BOOL "" FORCE)
set(RATS_SEARCH_FEATURES OFF CACHE BOOL "" FORCE)   # we use libtorrent, not librats' BitTorrent
set(RATS_STORAGE OFF CACHE BOOL "" FORCE)
add_subdirectory(${OCTOMX_ROOT}/third_party/src/librats-pinned ${CMAKE_BINARY_DIR}/librats EXCLUDE_FROM_ALL)
target_link_libraries(mxnet_overlay PRIVATE rats)   # brings Threads + ws2_32/iphlpapi/bcrypt/advapi32 on Windows
```

**Node composition** (copy of `run_search()`, and of rats-search's `P2PTransport::start()`):
1. `NodeConfig{listen_port = settings (default 4444 is fine; 0 = random), protocol = "rats-search/4",
   security = Noise, data_dir = <profile>/rats, max_peers = 30}`.
2. Before `start()`: `DhtDiscovery{dht_port = settings, data_dir = same}`, `MdnsDiscovery`, `MessageJson`,
   `ReconnectionService{store_path = data_dir + "/peers.json"}`, `HolePunch{enable_relay = true}`,
   `Relay{serve = false}`, `PeerExchange{peer_target = 30}`, and optionally `PortMappingService` (user setting "UPnP").
3. Register `on_peer_connected`, `on_peer_disconnected` and `on_peer_rerouted`, plus MessageJson handlers for `client_info`,
   `searchTorrent_response`, `searchFiles_response`, `torrent_response`, and `feed`/`randomTorrents` (ignore or answer).
4. `node.start()`.
5. On peer connected, send our `client_info`. If a search is active, re-send it to the newcomer (peers trickle in
   over 30 s, and a broadcast reaches only the peers connected at that moment). Do the same on `on_peer_rerouted`, because messages sent
   over a link that lost a duplicate-connection race are lost.
6. Search = `json->send("searchTorrent", q)` and `json->send("searchFiles", q)` (the 2-argument overload broadcasts to all), plus a
   per-peer send to late joiners until the search is closed. Deduplicate hits by `hash`.
7. When a hit is opened, send `torrent {hash, options:{files:true}}` to the peer it came from (the `peer` you
   recorded) to get `files_list`. Then build a magnet `magnet:?xt=urn:btih:<hash>&dn=<name>` and give it to libtorrent.
8. Shutdown: `node.stop()` takes about 1 s (observed). It persists peers.json and the DHT tables. Call it from the network thread
   or on exit.

**Threading → Qt**: every librats callback runs on a librats reactor or worker thread. Convert in the
callback (copy strings, `Json::dump()` → `QJsonDocument::fromJson`, as rats-search does) and hop
with `QMetaObject::invokeMethod(qobj, [=]{ ... }, Qt::QueuedConnection)`. `send`, `broadcast` and `connect` are
thread-safe and non-blocking, so they can be called from the GUI thread. Never block inside a callback. `librats::Json::get<T>()` throws
`JsonError` on type mismatch, so guard every field (see `jint`/`jstr` in the PoC). A peer can send anything.

**Persistence**: keep `<profile>/rats/` (identity.key, peers.json, dht_routing*.json) across restarts. That is the difference
between 10 s and 0.3 s to the first result.

**Etiquette [inferred]**: answer `client_info` (we do), and preferably answer `searchTorrent` from our own index (use 3)
so we are not a pure leech. Do not advertise `databaseSharing`. Honour `safeSearch` from user settings (the network
has a lot of adult content, so default to safeSearch=true for end users). Seeders/leechers in replies come from
the remote peer's last tracker scrape (`trackersChecked`, often months old). Treat them as hints and refresh
them via libtorrent when the user opens the hit.

---

## 3. Use 2: our own OctoMX overlay

### 3.1 Fit [code + ran]

| Need | librats piece | Verdict |
|---|---|---|
| Find other OctoMX users on the internet | `DhtDiscovery` with our own key (our own protocol string, `kOverlayProtocol` in `src/net/overlay/overlay_plugin_impl.h`, kept byte for byte → SHA1 rendezvous on mainline) + `ReconnectionService` + `PeerExchange` | Works for rats-search at ~120 nodes. See 3.2 for the same-host caveat. |
| LAN without internet | `MdnsDiscovery` | **[ran] 0.10 s with librats HEAD.** The pinned commit has `IP_MULTICAST_LOOP=0`, so two instances on **one host** never meet; this was fixed on HEAD 2026-11. Separate LAN hosts work on both (the pinned build received other LAN hosts' mDNS). |
| Chat rooms | `PubSub` (GossipSub: mesh D=6, IHAVE/IWANT, seen-cache, per-topic `set_validator`) | **[ran]** A→B→C chain with PEX off: C's messages reach A over 2 hops (`logs/overlay-chain-A.log`). Note: a publisher also receives its own message (`from == local_id`), so filter it. |
| Private messages, browse requests/replies | `MessageJson` direct `send(peer, type, json)` (or raw `node.send(peer, channel, bytes)`) | **[ran]** PM delivered on connect in every scenario. |
| Browse = file list transfer | `MessageJson` for the list (chunked), or `FileTransfer` (offer/accept, resume, SHA-256, backpressure) | [code] FileTransfer is what rats-search uses for multi-GB DB dumps. |
| Hash registry (link the same file across BT/ed2k/gnutella) | `PubSub` topic for announcements + MessageJson query/response. (A DHT `put`/`get` for arbitrary values does not exist; librats only does announce/get_peers. `StorageManager` is an LWW-replicated KV store, but it is flood-replicated to every peer and not suited to large registries.) | [inferred] Build the registry as an application protocol on top: gossip `{btih, ed2k, sha1, tth, size}` tuples plus a request/response lookup. Optionally share the rats network's search for torrents. |
| NAT traversal | STUN, UPnP/NAT-PMP, hole punch, relay, UDP-first transport | [ran] reached 25 inbound peers through our NAT without UPnP |
| Encryption / identity | Noise_XX, PeerId = SHA-256(pubkey) | Usable directly as the OctoMX user identity key (stable per profile). |

### 3.2 Overlay PoC results [ran] (`tests/research-librats/run_overlay.sh`, logs `logs/overlay-*`)

* **mDNS, two processes on one host, librats HEAD**: both connected at **0.10 s**. PM received by both.
  GossipSub messages every 5 s on `winmx/room/lobby` (the PoC's test topic). 45 s: `gossip_rx=17 pm_rx=1` on each side.
* **mDNS, same test, pinned librats**: never connected (multicast loopback disabled; see above).
* **Chain A←B←C, direct dials over 127.0.0.1, PEX disabled**: A got `C says hi #1` at 7.0 s via B
  (2 hops). Each node got its PMs.
* **DHT-only, two processes on one host, unique key**: **no connection** within 150 s (HEAD, `logs/overlay-dht-*`)
  or 300 s (pinned, `--debug`, `logs/dhtdbg2-*.log`). The debug run shows how far it got:
  * both nodes announced their STUN-reflexive endpoints (`203.0.113.7:38419` and `:34893`);
  * **after 3 min 13 s** each node's `get_peers` returned the other
    (`find_peers 07d739f1 → 3 peer(s): 203.0.113.7:34893, 203.0.113.7:38419, ...`);
  * both then dialed and punched every 10 s, which failed. The target is our *own public IP*, and this router does not
    do NAT hairpinning, so two nodes behind the same NAT can never meet over the DHT.
  The nodes also dialed random unrelated IPs (`45.176.99.77:34709`, `94.120.191.113:46065`, ...) that DHT nodes returned for a
  key nobody else uses. Several of these carry *our own DHT port*, which points to fabricated get_peers replies
  (poisoning or honeypot nodes). They cost a failed handshake each and are otherwise harmless.
  **[inferred]** For a brand-new, 2-member key, DHT rendezvous takes about 3 min. For a populated key (use 1) it took about 10 s cold.
  Same-NAT users must be found by mDNS (same LAN), PEX, or a direct dial to the LAN address.
* **No internet**: mDNS-only nodes, direct dials and GossipSub/MessageJson need no internet at all
  (only `DhtDiscovery` uses STUN and the bootstrap routers).

### 3.3 Recommendation for use 2: embed librats as-is, behind our own interface

* Run **a second `Node`** with our own protocol string (its own port, its own `data_dir`) alongside the rats-search node.
  Two nodes cost two more DHT sockets (v4+v6 each) and a couple of threads. Do not try to share one Node
  between the two protocols: the protocol string is part of the handshake.
* Wrap it in our own `OverlayTransport` (QObject: `start/stop`, `sendTo(user, type, QJsonObject)`,
  `publish(room, QByteArray)`, signals `peerUp/peerDown/message/roomMessage`). Then a librats API break, or a later
  replacement, stays local. Model it on rats-search's `P2PTransport` (≈740 lines, MIT), which already
  solves the Qt marshalling, `emit` macro and handler registration problems.
* Use HEAD (or the first release after 2026-11-17) for the overlay node if mDNS on one host matters, but
  **one librats copy serves both nodes**, so pick the newest commit that still interoperates with the rats network.
  It did on 2026-11-20 (search-head-1). Re-run `rats_poc search` before every bump.
* **GossipSub messages are not signed.** [code: `pubsub.h` wire `PUBLISH [0][origin:32][seqno:u64][topic_len][topic][data]`;
  no signature anywhere in `pubsub.cpp`] The handler's `from` is the *origin* field. In the chain test A saw C's id
  `b0ab814f` although only B was connected. But that field is a claim that any relaying peer can forge. Only direct
  MessageJson/channel messages carry a handshake-authenticated sender. For chat authorship, add an Ed25519/X25519-derived
  signature in our payload (or accept the risk), and use `PubSub::set_validator` to enforce size and rate limits and signature checks
  per room topic.

---

## 4. Use 3: local DHT crawling / indexing

### 4.1 How rats-search fills its database [code]

* `net/crawler.cpp` turns on librats `Bittorrent::set_spider_mode(true)` plus a timer `spider_walk()` (every 100 ms,
  `spider.walkInterval`) and toggles `set_spider_ignore()` for rate limiting.
* `dht/node.cpp` in spider mode:
  * **Passive announce sniffing**: every incoming `announce_peer` is reported (`spider_announce_(info_hash, peer)`),
    **with token verification skipped** (`skip_token = spider`).
  * **Sybil-style walk**: `find_node` toward random targets, *posing as a neighbour* of the queried node
    (`neighbor_id(target.id)`), and answering crawled IPs with neighbour ids so they insert us into many buckets.
  * **No BEP 51 `sample_infohashes`** anywhere in librats (`grep -ri sample_infohash` finds nothing).
* Metadata: for each new hash, `Bittorrent::get_torrent_metadata_from_peer(hash, ip, port)` (BEP 9/10 ut_metadata
  from the announcing peer). It falls back to `get_torrent_metadata(hash)` (DHT get_peers + BEP 9). The queue is capped by
  `MAX_CONCURRENT_METADATA_FETCHES`.
* Storage: **Manticore Search** (`data/manticore.cpp`). rats-search spawns its bundled `searchd` (submodule
  `imports` = DEgITx/manticore-binary), writes a `sphinx.conf` with RT indexes `torrents` (`nameIndex` full-text,
  `min_prefix_len=3`, `expand_keywords=1`, attrs hash/name/size/files/seeders/.../info json), `files` (path full-text),
  `feed` and `store`, and talks SphinxQL through Qt's **QMYSQL** driver.

### 4.2 Recommendation for use 3: reimplement on libtorrent + SQLite FTS5

* We already ship libtorrent-rasterbar **2.0.15** (`third_party/linux/include/libtorrent/version.hpp`), which has
  everything we need:
  * `session::dht_sample_infohashes(endpoint, target)` → `dht_sample_infohashes_alert` (BEP 51: well-behaved,
    returns up to ~20 hashes plus the node's total count and interval, and the nodes to walk next).
  * `dht_announce_alert` (info_hash, ip, port of incoming announces) and `dht_get_peers_alert`: the same passive
    feed rats-search's spider gets, without spoofing node ids.
  * Metadata: `add_torrent` with a magnet + `torrent_flags::upload_mode` (or a per-torrent `max_connections`
    cap), wait for `metadata_received_alert`, read `torrent_file()`, remove. Optionally add the announcing peer
    with `torrent_handle::connect_peer(ip:port)`, which is the same fast path rats-search uses.
* Do **not** enable librats `RATS_SEARCH_FEATURES` for this. It pulls in a second BitTorrent engine (10.6 k lines, its own uTP and MSE),
  and the node-id spoofing makes us a bad DHT citizen.
* **Index store: SQLite FTS5 works fine as a replacement for Manticore [inferred]**:
  `CREATE VIRTUAL TABLE torrent_fts USING fts5(name, paths, content='torrents', content_rowid='id',
  tokenize='unicode61 remove_diacritics 2', prefix='3 4');` (mirrors `min_prefix_len=3`). Use `bm25()` for ranking
  and an external-content table for attributes (hash UNIQUE, size, files, seeders, leechers, added, content_type).
  Optionally add a second `tokenize='trigram'` table over paths for substring search. Manticore is a separate daemon process
  needing QMYSQL (not in standard Qt builds), so it is not suitable here. SQLite is in QtSql already.
* **Seed the local index from the rats network too**: `randomTorrents {limit:5..10}` on connect (peers have
  `p2pReplicationServer=true` by default and answer with full torrents + `files_list`), plus `feed`, plus every
  `torrent_response`. rats-search itself does this, and it grows our local DB with no crawling at all.

---

## 5. Risks and unknowns

1. **Protocol churn (high).** `rats-search/3` → `/4` happened with v2.3.6 on 2026-11-02, and the old network is empty now (5 IPs).
   Each bump strands old clients. Mitigation: keep the protocol string in a setting (or a remotely updated config),
   ship librats updates promptly, and keep the `rats_poc search` smoke test in CI. You could run two rats Nodes
   (old + new string) during a transition.
2. **Single maintainer, weekly releases (high).** API changes even between patch releases (DhtDiscovery config,
   mDNS). Pin commits, wrap behind our interface, budget an upgrade check per release.
3. **Pinned commit vs HEAD.** Today both interoperate [ran]. The pinned commit has the mDNS loopback bug.
4. **Content.** The network returns plenty of adult and copyrighted material. Filtering (`safeSearch`, `contentType "bad"`,
   category `xxx`) is the remote peer's job and only advisory. Apply our own filter too.
5. **Trust.** Results are unauthenticated claims by peers. A hostile peer can return fake names for a hash. Show
   the source peer count per hash (several peers agreeing), and verify via BEP 9 metadata before download.
6. **Peers.json, identity.key and DHT tables are written by librats**, as plain files in `data_dir`. Fine for us; just point
   it at the profile. identity.key is an unencrypted private key.
7. **Firewall prompts on Windows**: librats listens on TCP+UDP (and 5353 for mDNS). The installer should add a rule.
8. **Not tested**: UPnP/NAT-PMP (deliberately not enabled, to avoid touching the router), Relay serving, FileTransfer,
   behaviour under real Windows (only wine), IPv6 (host has none: "Network is unreachable" for v6 bootstrap,
   harmless), long-running stability (>10 min), memory/CPU use.
9. **Unknown**: how rats-search peers rate-limit or ban clients that send many searches. No limit was hit in our runs
   (one search per peer per run).

---

## 6. Rebuild / rerun the PoCs

```bash
source toolchain/env.sh
cd $OCTOMX_ROOT
# sources (already present):
#   git clone --depth 1 https://github.com/librats/librats.git third_party/src/librats
#   git -C third_party/src/librats fetch --depth 1 origin 364169f06d9ed38abdf95c6b000855849302eb78
#   git -C third_party/src/librats worktree add ../librats-pinned 364169f06d9ed38abdf95c6b000855849302eb78
#   git clone --depth 1 https://github.com/librats/rats-search.git third_party/src/rats-search

# Linux, against the commit rats-search v2.3.6 pins (default LIBRATS_DIR)
cmake -S tests/research-librats -B build/research-librats/poc-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build/research-librats/poc-linux -j8
# Linux, against librats HEAD
cmake -S tests/research-librats -B build/research-librats/poc-linux-head -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DLIBRATS_DIR=$OCTOMX_ROOT/third_party/src/librats
ninja -C build/research-librats/poc-linux-head -j8
# Windows (llvm-mingw)
cmake -S tests/research-librats -B build/research-librats/poc-win64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=$OCTOMX_TOOLCHAIN_FILE
ninja -C build/research-librats/poc-win64 -j8

cd build/research-librats
# Use 1: live search (state persists in --data; reuse it for a warm start)
./poc-linux/rats_poc search --data run/search1 --port 44445 --dht-port 46881 --seconds 600 ubuntu "big buck bunny" sintel | tee logs/search.log
WINEPREFIX=$PWD/wineprefix wine poc-win64/rats_poc.exe search --data run/search-win --seconds 180 ubuntu
# population estimate
./poc-linux/rats_poc dhtprobe --data run/probe --seconds 280 rats-search/4 rats-search/3
# Use 2: overlay scenarios (mdns | dht | chain | all) [seconds]; use HEAD for same-host mDNS
BIN=$PWD/poc-linux-head/rats_poc $OCTOMX_ROOT/tests/research-librats/run_overlay.sh all 60
# add --verbose / --debug to any mode for librats logs
```

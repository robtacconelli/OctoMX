# OctoMX: architecture (authoritative; owner: coordinator)

One process, C++17 + Qt 6. The UI is a faithful WinMX 3.53 look-alike; underneath, several
networks are searched at once and a single download can pull bytes from several of them.

```
              ┌────────────── src/ui (mxui) ───────────────┐      Qt Widgets, classic WinMX look.
              │ MainWindow, pages, dialogs, models, style  │      Knows ONLY mx/services.h
              └───────────────────┬────────────────────────┘
                                  │ mx::Core facade  (src/api/mx/services.h)
        ┌─────────────────────────┴───────────────────────────┐
        │ src/mock (mxmock): canned data      src/core (mxcore): the real thing
        │   --demo, UI screenshots            Settings · Database · Library+Hasher
        └─────────────────────────────────    SearchManager · DownloadEngine · UploadTracker
                                              NetworkManager · Chat/User/Bandwidth services
                                                    │ mx::NetworkPlugin / RangeSource /
                                                    │ TorrentBackend / ChatBackend (mx/plugin.h)
     ┌───────────┬───────────┬───────────┬─────────┴──┬────────────┬───────────┐
     net/bt      net/ed2k    net/gnutella net/http     net/overlay  net/web
     libtorrent  eD2k + Kad  Gnutella 0.6 HTTP ranges  our peers:   torrent index
     (BT + DHT)  (2 plugins) leaf         web seeds    chat, browse search (rats
                                                       hash links   network, web APIs,
                                                                    local DHT index)
```

## Modules, targets, owners

| dir | target | links | purpose |
|---|---|---|---|
| `src/api` | `mxapi` | Qt6::Core | contract: `mx/types.h`, `mx/services.h`, `mx/plugin.h` (coordinator-owned, additive only) |
| `src/core` | `mxcore` | mxapi, Qt Core/Network/Sql/Concurrent | real `mx::Core` (`createRealCore`) |
| `src/mock` | `mxmock` | mxapi | `createMockCore`, deterministic demo data resembling the reference screenshots |
| `src/net/bt` | `mxnet_bt` | mxapi, libtorrent | `mx::createBitTorrentPlugin()`, NetworkPlugin + TorrentBackend |
| `src/net/ed2k` | `mxnet_ed2k` | mxapi, Qt Network | `mx::createEd2kPlugins()` → {ED2K plugin, Kad plugin} |
| `src/net/gnutella` | `mxnet_gnutella` | mxapi, Qt Network | `mx::createGnutellaPlugin()` |
| `src/net/http` | `mxnet_http` | mxapi, Qt Network | `mx::createHttpPlugin()`, RangeSource for http(s) URLs |
| `src/net/overlay` | `mxnet_overlay` | mxapi, ... | `mx::createOverlayPlugin()`, NetworkPlugin + ChatBackend |
| `src/net/web` | `mxnet_web` | mxapi, Qt Network | `mx::createWebSearchPlugin()`, search-only |
| `src/ui` | `mxui` | mxapi, Qt Widgets | everything visible |
| `src/app` | `octomx` (exe) | all of the above | `main.cpp`: args, single instance, composition |

Each plugin module exposes exactly one small public header `src/net/<name>/<name>_plugin.h` with
its factory function; everything else is private to the module. The top-level `CMakeLists.txt`
auto-adds any module directory that has a `CMakeLists.txt`; `src/app` checks `if(TARGET mxnet_x)`
and defines `MX_HAVE_X` so the app builds with whatever subset exists.

Tests: `tests/<module>/CMakeLists.txt` (Qt Test, registered with `add_test`). Build dir of your own.

## Command line (src/app)

```
octomx [--profile <dir>] [--demo] [--page <networks|shared|chat|search|hotlist|transfers|bandwidth>]
       [--size WxH] [--screenshot <out.png>] [--screenshot-delay <ms>] [--script <file>]
       [--minimized] [<magnet:|ed2k://|http(s)://|file.torrent> ...]
```
* `--profile`: where all state lives (default: `QStandardPaths::AppDataLocation`, i.e.
  `%APPDATA%/OctoMX` / `~/.local/share/OctoMX`; if a `portable.txt` sits next to the executable,
  use `<exe dir>/profile`). Tests always pass `--profile`. When `.../OctoMX` does not exist and an
  old `%APPDATA%/WinMX` / `~/.local/share/WinMX` with a `settings.json` does (the legacy profile
  folder of copies installed before 5.0), that old folder is used as it is, every start; it is
  never moved or copied.
* `--demo`: use the mock core (no network at all).
* `--screenshot`: show the window, wait for the delay (default 800 ms), `grab()` the main
  window (and any open dialog: save as `<name>.<dialog>.png`), exit 0.
* `--script`: simple UI automation for tests/screenshots (one command per line; see the UI
  module's README for the verbs: `page search`, `type ...`, `click <objectName>`, `wait 500`,
  `shot out.png`, `menu ...`, `quit`).
* URIs given on the command line (or forwarded from a second instance, or via OS file/URL
  association) start downloads.

## Profile directory layout

```
<profile>/settings.json        all settings (see src/core settings schema)
<profile>/octomx.db            SQLite: transfers, sources, library index + hashes, hash links,
                               hotlist, ignore list, search history, chat favourites, stats
                               (an existing winmx.db, the legacy name, keeps being used under
                               that name)
<profile>/bt/                  libtorrent session state, DHT nodes, .fastresume + .torrent per torrent
<profile>/ed2k/                server.met-like list, nodes.dat (Kad contacts), user hash, known files
<profile>/gnutella/            host cache
<profile>/overlay/             identity keypair, known peers
<profile>/web/                 local torrent index (FTS)
<profile>/incomplete/          part files of non-torrent downloads (+ .part.met-like state in db)
<profile>/logs/                rotating log (logs/octomx.log, octomx.1.log ...)
```
Default incoming directory: `<Downloads>/OctoMX` (setting `transfers/incomingDir`); for a user who
already has the legacy default `<Downloads>/WinMX` (copies installed before 5.0), that folder
stays the default.
Everything a user would expect to "remember" must survive a restart and a crash (write state
incrementally; fast-resume data at least every 60 s and on shutdown).

## Search

`SearchManager` (core) fans a `SearchQuery` out to every enabled plugin with `CanSearch`, merges
the hits, and emits `hitsChanged`. Merge rule (`SearchMerger`, src/core/search_merge.h), two hits are
the same file when:
1. `a.hashes.matches(b.hashes)` (some hash kind equal, none conflicting); two hits of the same whole
   torrent are one row even when indexes disagree about it being a folder (the known file count or
   list decides), or
2. [merge-1] no shared hash kind and `SearchMerger::sameFile()`: exactly the same byte size
   (>= `search/minCandidateSize`, 1 MiB), the same non-empty extension and `sameName()` (lower-case words
   of letters and digits, two characters or more, the extension word not counted: at least 60 % of the
   shorter name's words occur in the other; names without such words must be equal) → *candidate*
   (kept as merged sources flagged `SearchSource::extra["candidate"]=true` with their own hashes in
   `extra["hashes"]`; checked block by block when downloading). This holds for every pair of networks
   in both directions (eD2k, Kad, Gnutella, the overlay, HTTP, torrents). A torrent counts as one file
   when its list has one real entry (pad files ignored), when an index reported one file, or when
   nothing says it has several and its name has an extension, also as a trailing word
   (`Sintel 2010 720p mkv`: indexes lose the dots). A later hit whose hash matches a candidate's (the
   same torrent from an index with a rounded size) joins that row as a candidate.
3. [merge-1] inner files: a folder torrent with a known file list whose inner file is `sameFile()` as a
   plain-file row becomes a source of that row (derived, `extra["innerOf"]`, `["insideBundle"]`,
   `["bundlePath"]`, `["btFileIndex"]`, magnet `so=`); the folder row stays and lists those files in
   `SearchHit::innerMatches` (with that row's sources, so downloading the folder uses them for that
   file). A folder without a list whose name and size say it may hold a plain row's file gets one
   file-list request (at most 30 per search).
Hits never merge when `conflicts()` is true. A torrent whose file list or count later shows several
files is split out of a row again (`setBundleFiles`, rows rebuilt from the hits they were made of,
keys kept). The merged hit keeps the best metadata (longest name wins ties, any known
duration/bitrate). Peers of a row = the best BitTorrent swarm figure + the sum of the other
networks' sources (the Peers column sorts by it). `search/mergeByNameAndSize = false` restores the
stricter rule (equal normalised names, plain files only, a bundle never meets a non-bundle).
`SearchHit::key` = first available of `ed2k:` / `sha1:` / `btih:<hash>[:idx]` / `size+name`.
Local flags (`matchesIncompleteLocal`, ...) are set from the DB before emitting.

## Downloads: the multi-network engine (core/DownloadEngine)

A **download** is one file, or one torrent *bundle* (multi-file). It has a `FileHashes` identity,
a store, and any number of sources from any network.

Two stores behind one internal interface:
* **TorrentStore**: when a BitTorrent identity is known (`hashes.btih`). libtorrent owns the
  files, picks pieces from its own peers, and verifies every piece. Extra networks help through
  `TorrentBackend::reservePieces()` + `injectPiece()`: the engine reserves a batch of pieces that
  libtorrent is not currently downloading (prefer pieces fully inside the linked file, rarest /
  furthest from libtorrent's own progress), fetches the corresponding byte ranges from
  RangeSources, assembles whole pieces, pre-checks SHA-1 against `pieceHash()` (so a bad source
  is blamed precisely), and injects them. Reservations time out and are returned.
* **PartFileStore**: no torrent known. A sparse file `<incomplete>/<id>.part` with a persisted
  range map. Verification units, in order of preference: eD2k part hashes (9 728 000-byte parts,
  via `hashSetLearned("ed2k-parts")`), AICH / TTH leaves, else whole-file hash on completion
  (SHA-1/ed2k recomputed incrementally). A failed unit is re-downloaded and the contributing
  sources are penalised (3 strikes → banned for this download).
  If a torrent identity is learned later (hash link), the part file is handed to libtorrent
  (move into place, `addTorrent`, `recheck`) and the download continues as a TorrentStore.

**Range scheduling**: missing ranges are split into chunks of `source->preferredRequestSize()`
and handed to RangeSources in state Ready; rarest-first across known `available()` maps, never
the same chunk to two sources unless endgame (< 2 % left). Slow sources (< 2 kB/s for 30 s while
others are fast) lose their chunk. Data arrives via `RangeSource::data()`.

**Source discovery**: on start (and every few minutes while incomplete) the engine calls
`findSources()` on every plugin with `CanFindSources` whose hash kind is known, and
`lookupHashLinks()` on plugins with `CanHashLinks` to learn more hash kinds, which unlocks more
networks. Candidate (size+name) links are tried too: the first verified unit confirms the link
(then it is published with `publishHashLinks`); a mismatch discards it, costing one unit.

**Copies matched only by name and size** [merge-1]: one identity is the *judge*, the one that
verifies the smallest units and is known completely: BitTorrent piece hashes once the torrent's
metadata is there, else eD2k part hashes (AICH is not used for verification), else the TTH tree, else
the whole-file hash of the identity the download started with. Blocks from a candidate source are
accepted only where the judge can verify their unit (never mixed in unverified); the first verified
unit confirms that network for this download (the source row says `(confirmed)`, its hash becomes part
of the identity, except for a folder's inner file); a failed unit bans that source, and after
`Tuning::candidateNetFailures` (3) different sources of that network failed, or when every candidate of
it failed, the network is dropped from the download (`The eD2k copy is a different file: dropped`; eD2k
and Kad count as one copy). Waste cap: an unconfirmed network has at most
`Tuning::candidateUnitsInFlight` (2) units of the judge in flight or unverified at once (two pieces of a
torrent, two 9.28 MB eD2k parts), so a wrong match costs those units. With only a whole-file judge (no
part hashes, no torrent) candidates are used only provisionally ([engine-4]) and the whole file is the
unit. At completion every known hash kind is compared: a kind only a confirmed candidate brought that
disagrees is reported in the status (`Complete, but the eD2k copy was not the same file ...`), dropped
from the identity and never published; a disagreement of the judge's own kinds fails the file as before.
Started from the plain-file side with a candidate torrent: the part file runs with eD2k / TTH / whole-file
judging while the torrent's metadata is fetched (wanted files none); once a piece the part file holds
matches the torrent's piece hash the download is promoted into the torrent and the judge becomes the
piece hashes (a mismatch rejects that torrent, the next candidate torrent is tried, then BitTorrent is
dropped). Started from the torrent side: candidates wait for the metadata (no unit to judge before).
A torrent an index called a folder whose metadata shows one file becomes a plain-file download (that
file's name and size). The background *Search by name and size* runs for the file, or for each large
file of a folder (up to 4, the largest first), on every connected searchable network (eD2k / Kad only
without a trusted ed2k hash, Rats only without a torrent), accepts `sameFile()` hits (plain files,
single-file torrents, inner files of folder torrents, whose file lists it asks for), and repeats with
back-off (2, 5, 10, 20, 30, 60 min) while the download has fewer than 20 sources or more than 15 min to go.

**Completion**: files are moved to the incoming dir, all hashes are computed (the Library hasher
does btih-independent ones: ed2k + part hashes, AICH, SHA-1, TTH, SHA-256), stored in the DB,
published as hash links, and the file is shared like any library file (if `transfers/shareCompleted`).

Limits: `transfers/maxDownloads` active at once (others are `Queued` = "Locally Queued");
pause/resume/cancel/priority per download; everything is restored after restart.

## Uploads / sharing (Library)

`Library` scans the shared folders, hashes files in a background thread pool (results cached in
the DB by path+size+mtime), and pushes the list to every plugin (`setSharedFiles`). Plugins with
`CanServe` serve data through `LibraryAccess::read()`. Uploads are reported with
`uploadChanged` and shown in the lower Transfers pane. BitTorrent: completed torrents keep
seeding; library files can additionally be seeded as single-file torrents (`seedLocalFile`,
setting `bt/seedLibrary`).

## Bandwidth

`RateLimiter` (core) holds the global up/down limits. Non-libtorrent plugins call
`acquire()`/`account()`. Once per second the core gives libtorrent what is left of the global
limit (never less than 25 %). `BandwidthService` samples the totals once per second.
[bw-1] One shared budget: wire bytes of transfers that did not pass `acquire()` (libtorrent within
its budget, protocol messages of transfer connections) are charged to the same buckets ([bw-2]:
search and discovery traffic is not, see below); libtorrent's budget yields a
fifth of the limit per second while the others are refused tokens, never below half of it (the 25 %
above is now 50 %; [bw-2] replaced by the fair shares below). LAN peers are exempt unless `bandwidth/limitLan` (`exempt()` / `accountExempt()`).
The limits in force (schedule, "your limits win until the window ends", upload minimum 10 KB/s) come
from `mx/bandwidth_limits.h`, the same code the UI uses.
[bw-2] Second by second: the shared bucket holds 100 ms of the limit and refills smoothly; every
100 ms each network's rate is measured and the limit is shared max-min fairly between the networks
that want more (`acquireFor(dir, net, bytes)` refused them, or libtorrent reports bytes waiting for
its limit with `reportWaiting()`); while another network wants more, a network gets no more than its
share, otherwise all that is left. libtorrent's budget moves halfway to its share every 100 ms (the
plugin polls it every 200 ms, accounts its bytes every 100 ms, which the bucket reserves ahead of the
report). Under a limit libtorrent hands out quota every 100 ms and keeps in flight, over all peers it
downloads from, an in-flight time of its budget (0.1 to 0.5 s, starting at 0.12 s, adapted once a
second to the peers' round trip: longer while it stays under its budget with nothing waiting for
quota, shorter while much waits), split among the peers, at least 2 blocks each; a libtorrent peer
plugin (`src/net/bt/bt_pacing.*`) sets it on every connection, as the setting itself is replaced by
each peer's "reqq". Without a limit libtorrent's defaults. Measured with `tests/engine/bw_slice_probe`
(see `docs/status/bw-2.md`).

**What the limits cover** [bw-2, owner's rule]: the download and upload limits are for file
transfers only. Search and discovery traffic is never slowed down and never charged to the budget
(so a search never takes allowance away from a transfer and a limit never makes a search slow); it is
still counted, so the Bandwidth graph shows it.
- Counted against the limits: BitTorrent peer connections (payload and peer protocol), HTTP / HTTPS
  sources, eD2k peer connections (file data, hello, hash sets, source exchange, queue), Gnutella
  HTTP downloads and uploads (with their request headers), overlay file data (the data channel).
- Exempt (counted for the graph only): BitTorrent DHT and trackers; eD2k server connection (login,
  searches, source lists) and all eD2k / Kad UDP; Kad; Gnutella ultrapeer connections (queries, hits,
  pings), Gnutella UDP and the first bytes of an incoming connection; web index queries; Rats
  searches and file lists; overlay control channel, pub-sub, presence, chat and search; LAN peers
  unless `bandwidth/limitLan`. Plugins report exempt bytes with `accountExempt()`; the core treats
  Kad, Web and Rats as exempt whatever a plugin calls.

## Overlay (the OctoMX network)

Our own peer network gives the classic WinMX social features: chat rooms, private messages, browse a
user's files, whois, hotlist presence, the hash-link registry and direct file transfer between
OctoMX clients (as a RangeSource). Design to be finalised from `docs/research-librats.md`.

## Conventions

* Namespace `mx`. Classes `CamelCase`, files `snake_case.{h,cpp}`. No exceptions across module
  boundaries. No `using namespace` in headers. `QStringLiteral`/`tr()` for UI strings.
* Logging: `qCDebug`/`qCInfo` with a category per module (`mx.core`, `mx.bt`, `mx.ed2k`, ...);
  the app installs a handler writing `<profile>/logs/octomx.log`.
* Long operations are asynchronous; services batch UI updates (≤ ~2 Hz per row).
* Shutdown must finish within 5 s and never lose state.

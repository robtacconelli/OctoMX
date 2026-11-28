# The OctoMX API

OctoMX can be used by another program, on this computer or on a TV or PC in your home network, to
**search every network, pick a result and get a link that a video player opens**. The player gets
plain HTTP with Range requests; OctoMX fetches the bytes the player reads, when it reads them, from
every network that has the file (BitTorrent, eD2k / Kad, Gnutella, HTTP mirrors, the OctoMX
network), and deletes them again afterwards.

The same API also **downloads files of any type** (from a search result or a magnet, ed2k or http
link) into the incoming folder, like the Search page does, and shows and controls every download of
the Transfers window: state, progress, speed, sources and peers, pause, resume, change, remove from
the list with or without its files. A program that only downloads needs no part about streams: start
with [Downloads](#downloads).

The API is **off by default**. Nothing listens until you switch it on.

## Contents

* [Switching it on](#switching-it-on)
* [Safety](#safety)
* [The calls](#the-calls) (status, searches, streams)
* [Walk-through: search, pick, play](#walk-through-search-pick-play)
* [How the bytes are fetched](#how-the-bytes-are-fetched)
* [What it does not do](#what-it-does-not-do)
* [Downloads](#downloads): [the calls](#the-download-calls), [state words](#state-words),
  [errors](#errors-of-the-download-calls), [searching for downloads](#searching-for-downloads),
  [streams and downloads together](#streams-and-downloads-together), [limits and
  warnings](#limits-and-warnings), [walk-through: search, pick, download, watch,
  remove](#walk-through-search-pick-download-watch-remove)
* [What changed since 5.9: only additions](#what-changed-since-59-only-additions)

## Switching it on

Settings, Other Networks, **Local API** (called Streaming API in 5.9):

| Setting | Default | What it does |
|---|---|---|
| Switch on the local API | off | Listens on 127.0.0.1 and ::1 (this computer only). Applies at once. |
| Port | 17580 | The port it listens on. A port another program holds is said on the page and in the log; no other port is taken by itself. |
| Allow other devices on my network | off | Also listens on this computer's private addresses (10.x, 172.16 to 31.x, 192.168.x, fc00::/7), never on a public one. Every device there that knows the token can then search, stream, download and remove downloads. |
| Programs may delete downloaded files | off | A program may delete the files of a download it removes (`DELETE /api/v1/downloads/<id>?files=delete` or `files=trash`). Off: `403`, nothing is touched; taking a download off the list (its files stay) is always allowed. Applies at once. |
| Access token | made when first switched on | Every API call must send it. **Copy** puts it in the clipboard, **New** makes a new one (the old token and every old stream link stop working). |
| Address | | The base address to give the program, for example `http://127.0.0.1:17580/api/v1/`. |
| All stream caches at most | 4096 MB | When all stream caches together hold more, streams nobody reads are ended and their caches deleted, the longest unread first. A stream that is being played is never cut: its cache can grow to the size of its file. The status calls show each cache and the total against the limit. |
| Delete unread streams after | 240 minutes (4 hours) | A stream nobody reads (no player, or a paused one) for this long ends and its cache is deleted. |
| Keep what was streamed as a download | off | When a stream ends, it becomes a normal download that completes into the incoming folder, instead of being deleted. |

## Safety

The API starts transfers, pauses and removes downloads and, when allowed, deletes their files, so it
is treated as a door:

* **Access token** on every call: `Authorization: Bearer <token>`. Without it, or with a wrong one, the
  answer is `401` and nothing else. The comparison takes the same time whatever the token is.
* **Stream links** carry a key made from the token for that one stream (players cannot send headers),
  never the token itself. A link opens only its own stream.
* **Only this computer** unless "Allow other devices on my network" is checked, and then only private
  addresses.
* **No use from web pages**: no CORS headers, a call with an `Origin` header is refused (`403`), the
  `Host` header must be one of the API's own addresses (against DNS rebinding), POST bodies must be
  JSON (`415` otherwise).
* Only `GET`, `HEAD`, `POST` and `DELETE` (`405` otherwise). Request head at most 8 KiB (`431`), body
  at most 64 KiB (`413`), no chunked bodies (`411`), at most 32 connections and 16 from one address
  (`503`), a request head must arrive within 10 s, an idle connection is closed after 30 s, a client
  that reads nothing of a stream for 5 minutes is closed.
* The API reads and writes nothing but its own streams and the downloads of the Transfers list: it
  cannot read or change settings, reach the shared folders or any other file. A download goes where a
  download of the window goes (the incoming folder, under the name its network gives): no request
  names a folder, a file name or a path, a link is a magnet:, ed2k:// or http(s):// link (never a file
  of this computer), ids from the path are looked up in the list (never used as a path). Deleting
  removes only what the download created (see [DELETE](#delete-apiv1downloadsid)).
* The full form of a download (`path`) and the answer of a removal name the folders on this computer
  where the files are: they show a folder name of the user (often the user's name) to whoever holds
  the token.

## The calls

In the examples the token is in the shell variable `T` and the API listens on port 17580:

    T=0123456789abcdef0123456789abcdef0123456789abcdef
    A=http://127.0.0.1:17580/api/v1

Every answer is JSON. An error has a code and a sentence a person can read:

    {"error": "search_empty", "message": "The search has no results yet: it is still running."}

| Call | What it does |
|---|---|
| `GET /api/v1/status` | Version, API on, the networks and whether they are up, open streams and searches |
| `POST /api/v1/searches` | Start a search over all networks (or the ones named) |
| `GET /api/v1/searches` | The open searches |
| `GET /api/v1/searches/<id>` | The results so far and whether the search still runs |
| `GET /api/v1/searches/<id>/results/<key>` | One result in full (a bundle: its file list, asked for when not known yet) |
| `DELETE /api/v1/searches/<id>` | Close a search |
| `POST /api/v1/streams` | Select a result (and for a bundle a file): a stream id and the stream link |
| `GET /api/v1/streams` | All streams (ended ones stay readable for 10 minutes) |
| `GET /api/v1/streams/<id>` | A stream's status |
| `DELETE /api/v1/streams/<id>` | Stop a stream: its cache is deleted (or kept as a download, see the setting) |
| `GET`, `HEAD /stream/<id>/<key>/<file name>` | The stream link a player opens |
| `/api/v1/downloads...` | Downloads: see [Downloads](#downloads) |

### GET /api/v1/status

    curl -s -H "Authorization: Bearer $T" $A/status

    {
        "api": "on",
        "networks": [
            {"name": "BitTorrent", "peers": 61, "state": "connected"},
            {"name": "eD2k", "peers": 1, "state": "connected"},
            {"name": "Kad", "peers": 240, "state": "connected"},
            {"name": "Gnutella", "peers": 4, "state": "connected"},
            {"name": "HTTP", "peers": 0, "state": "connected"},
            {"name": "OctoMX", "peers": 0, "state": "connected"}
        ],
        "searches": 0,
        "streams": 0,
        "version": "5.10"
    }

After 5.9 the status also has `downloads` (how many downloads the list holds, per [state
word](#state-words), and their total speeds), `types` (every type code a search takes, with what it
matches) and `deleteFilesAllowed` (the setting "Programs may delete downloaded files"); `cache` (the
stream caches) is there since 5.7. See [the status for downloads](#get-apiv1status-for-downloads).

### POST /api/v1/searches

The same search as the Search page, with the same settings (the adult and fake filters, the
maximum results). Fields: `text` (keywords, required unless `link` is given), `text2` (a second
keyword line), `type` (a file type code of the Search page: `any`, `video`, `audio`, `mp3`,
`documents`, ..., every one in [searching for downloads](#searching-for-downloads)), `types` (after
5.9: several codes at once; a result matches when it matches one of them), `extension`, `minSize`,
`maxSize` (bytes), `networks` (names such as `"BitTorrent"`, `"eD2k"`; empty or left out = every
network). `link` searches by a magnet / ed2k link or a hash. After 5.9 the answer also says which
`networks` are asked, and a network named in `networks` that cannot be asked now (switched off, not
running, or one that cannot search, such as HTTP) is listed in `skipped` instead of being left out
silently.

    curl -s -H "Authorization: Bearer $T" -H "Content-Type: application/json" \
         -d '{"text": "big buck bunny", "type": "video"}' $A/searches

    {"id": 1, "running": true, "url": "/api/v1/searches/1"}

At most 16 searches are open at once; then `429` "The API is busy" until one is deleted.

### GET /api/v1/searches/1

`offset` and `limit` (at most 500, default 100) page through the results, most sources first.

    curl -s -H "Authorization: Bearer $T" "$A/searches/1?limit=2"

    {
        "count": 41,
        "id": 1,
        "offset": 0,
        "results": [
            {
                "bundle": true,
                "files": [
                    {"index": 0, "path": "Big Buck Bunny/Big Buck Bunny.en.srt", "size": 140, "streamable": false},
                    {"index": 1, "path": "Big Buck Bunny/Big Buck Bunny.mp4", "size": 276134947, "streamable": true},
                    {"index": 2, "path": "Big Buck Bunny/poster.jpg", "size": 310380, "streamable": false}
                ],
                "key": "6c1f0e2d9a8b7c4d",
                "name": "Big Buck Bunny",
                "networks": ["BitTorrent", "Web"],
                "peers": 412,
                "seeds": 380,
                "size": 276445467,
                "sources": 3,
                "streamable": true,
                "type": "video"
            },
            {
                "bundle": false,
                "key": "a03b5e7f19c2d4e6",
                "name": "big_buck_bunny_480p_surround-fix.avi",
                "networks": ["Kad", "eD2k"],
                "peers": 25,
                "seeds": 12,
                "size": 220514438,
                "sources": 2,
                "streamable": true,
                "type": "video"
            }
        ],
        "running": true,
        "text": "big buck bunny"
    }

After 5.9 every result also says `downloadable` (`false` with `notDownloadable`, the reason, when no
network named a place to fetch it from and no hash of it is known).

`key` is stable for a result while the search runs. Two results can merge into one while networks
answer (a later answer proves they are the same file); the old key is then gone (`404`): read the
results again. A bundle whose file list is not known yet has `"files": null`; reading that one
result asks the networks for it:

    curl -s -H "Authorization: Bearer $T" $A/searches/1/results/6c1f0e2d9a8b7c4d

### POST /api/v1/streams

Fields: `search` and `result` (the key), `file` (for a bundle: the file's `index`; not needed when it
holds only one audio or video file), `download` (true: download it normally, see below).

    curl -s -H "Authorization: Bearer $T" -H "Content-Type: application/json" \
         -d '{"search": 1, "result": "6c1f0e2d9a8b7c4d", "file": 1}' $A/streams

    {
        "contentType": "video/mp4",
        "download": "d3f0a9c41b2e6d7a8",
        "id": "9b2c41d0e7f3",
        "name": "Big Buck Bunny.mp4",
        "ownCache": true,
        "state": "waiting for a player",
        "url": "http://127.0.0.1:17580/stream/9b2c41d0e7f3/5f0c3a9e1d2b4c6a8e7f0a1b2c3d4e5f/Big%20Buck%20Bunny.mp4",
        ...
    }

`url` is the link for the player. The same selection again gives the same stream (`200`). A stream
shows in the Transfers window as a download with the status "Streaming: ..." and can be paused or
cancelled there.

With `"download": true` the result is downloaded normally (a download row like any other, it stays
when the stream stops) and the link still works: the bytes the player reads come first. A file that
is not audio or video cannot be streamed (`415`), but `"download": true` downloads it (no link).

Errors: `404` no such search / result, `409 search_empty` (no results yet), `409 files_unknown` (the
bundle's file list was asked for: select again in a few seconds), `400 choose_file` (a bundle with
several audio or video files and no `file`), `415 not_streamable`, `409 no_sources`, `409
not_started` (with the engine's reason), `429 busy` (8 streams open).

### GET /api/v1/streams/9b2c41d0e7f3

    curl -s -H "Authorization: Bearer $T" $A/streams/9b2c41d0e7f3

    {
        "bitrateBps": 351000,
        "cachedBytes": 58720256,
        "contentType": "video/mp4",
        "download": "d3f0a9c41b2e6d7a8",
        "failedChecks": 0,
        "fetchedByNetwork": {"BitTorrent": 58982400},
        "id": "9b2c41d0e7f3",
        "limits": {
            "connections": "the proxy and network adapter settings apply as for every download",
            "downloadsAtOnce": "not counted: a stream always runs",
            "speed": "no download limit"
        },
        "longestWaitMs": 0,
        "name": "Big Buck Bunny.mp4",
        "networks": ["BitTorrent"],
        "players": [
            {
                "bufferTarget": 10530000,
                "bufferedAhead": 10747904,
                "bufferedAheadSec": 30.6,
                "end": 276134947,
                "firstByteMs": 1820,
                "from": 0,
                "id": 2,
                "longestWaitMs": 0,
                "position": 48234496,
                "rateBps": 351000,
                "sent": 48234496
            }
        ],
        "ranges": [[0, 58982400], [275775488, 276134947]],
        "served": 48593955,
        "size": 276134947,
        "sizeKnown": true,
        "sources": 64,
        "sourcesActive": 9,
        "speedBps": 0,
        "state": "playing",
        "transferStatus": "Streaming: 1 player, 10.2 MB buffered ahead",
        "unverifiedServed": 0,
        "verification": "Every BitTorrent piece is checked before it is served."
    }

* `state`: `waiting for a player`, `playing`, `waiting for data` (a player waits: `players[].waitingFor`
  says for which byte and how long), `paused` (a player took nothing for 3 s), `idle` (no player),
  `paused in the Transfers window`, `error`, `ended` (`ended` says why).
* `players`: every open read: where it started, where it is, how much is buffered ahead of it (bytes,
  and seconds at the measured bit rate), the buffer it aims at, its time to the first byte and the
  longest it waited after that.
* `cacheDropInSec`: when nobody reads, the cache is deleted in this many seconds.
* `ranges`: what is on disk and can be served (file offsets, at most 64 ranges).
* `unverifiedServed`: bytes sent before any check could prove them (see "Only right data" below),
  `failedChecks`: of those, bytes whose check failed later (fetched again).
* `fetchedByNetwork`: what each network delivered for this stream, `served`: what players got.

### DELETE /api/v1/streams/9b2c41d0e7f3

    curl -s -X DELETE -H "Authorization: Bearer $T" $A/streams/9b2c41d0e7f3

    {"message": "stopped through the API", "state": "ended", "stopped": true}

Every player connection of it ends; the download row goes; its cache is deleted (not moved to the
trash). With "Keep what was streamed as a download" on, it becomes a normal download instead. A link
of an ended stream answers `410` with the reason.

### The stream link

`GET` and `HEAD`, a single `Range` (`bytes=a-b`, `bytes=a-`, `bytes=-n`): `206` with `Content-Range`,
`Accept-Ranges: bytes`, `Content-Length` and a `Content-Type` from the extension; a range beyond the
end is `416`; several ranges at once get the whole file (`200`). The size is known before the first
byte. A torrent added by magnet first needs its file list from a peer: the answer waits for it (the
status call says so in `waitingFor`), at most 5 minutes (then `504` with the reason in the status); a
single file that other networks have too goes on from them after 10 s without that list. Several connections of one player, a player that closes and reopens, and a
second player on the same link all work.

## Walk-through: search, pick, play

    T=$(...the token from Settings...)
    A=http://127.0.0.1:17580/api/v1
    # 1. search
    curl -s -H "Authorization: Bearer $T" -H "Content-Type: application/json" \
         -d '{"text": "big buck bunny", "type": "video"}' $A/searches
    # 2. a few seconds later, the results
    curl -s -H "Authorization: Bearer $T" "$A/searches/1?limit=20"
    # 3. pick one (here the bundle, its file 1)
    curl -s -H "Authorization: Bearer $T" -H "Content-Type: application/json" \
         -d '{"search": 1, "result": "6c1f0e2d9a8b7c4d", "file": 1}' $A/streams
    # 4. play the "url" of the answer
    mpv "http://127.0.0.1:17580/stream/9b2c41d0e7f3/5f0c.../Big%20Buck%20Bunny.mp4"
    #    or: ffplay, vlc, ffprobe, ffmpeg -ss 300 -i <url> -t 5 -f null -
    # 5. see what it does
    curl -s -H "Authorization: Bearer $T" $A/streams/9b2c41d0e7f3
    # 6. done: stop it, the cache goes
    curl -s -X DELETE -H "Authorization: Bearer $T" $A/streams/9b2c41d0e7f3

## How the bytes are fetched

* **On demand, in the order the player reads.** Every open read position has a window: the part right
  after it (about 4 s of playing) first, then the **buffer ahead**: 30 s of playing at the measured
  bit rate, at least 4 MiB, at most 64 MiB. Nothing else of the file is fetched. The beginning, the
  end (mp4 and mkv players read their index there first), sequential reading and any place after a
  skip are the same mechanism. A seek moves the window at once; requests nobody waits for any more
  are cancelled unless less than 256 KiB of them is left.
* **Buffering ahead.** While a player reads sequentially, the bytes ahead of it are fetched before it
  asks; with sources faster than the bit rate it never waits after the start.
* **Player paused** (connection open, nothing read): fetching stops once the buffer is full. **Player
  gone**: nothing new is asked at once; after 20 s without any player the sources are let go (a torrent
  pauses). A player that comes back within the idle time reads what is cached; nothing is fetched twice.
* **Every network that has the file takes part**: BitTorrent through libtorrent's piece deadlines and
  priorities (the pieces of the windows with deadlines at the top priority, every other piece at 0), eD2k / Kad, Gnutella, HTTP
  mirrors and the OctoMX network through the engine's range choice (each source is asked first for the
  byte nearest after a read position it has). A source that does not have that range gives what it has.
* **Only right data reaches the player** where a block can be checked in reasonable time: torrent
  pieces, tiger tree leaves. Where the only check is a large unit (an eD2k part of 9.28 MB, also the
  eD2k part set of an OctoMX source) or the whole file (HTTP mirrors without other hashes), the bytes
  are served as they arrive: waiting for a whole 9.28 MB part before the first byte would cost seconds
  to minutes of start time. The status says so (`unverifiedServed`); a part that fails its check later
  is fetched again and counted in `failedChecks`. A torrent piece is checked in memory and written to
  its file a moment later (on a slow disk a long moment): the player gets it only after OctoMX has
  compared the file with the checked piece.
* **Where the bytes live**: in a cache below the incomplete folder (`Streams/<download id>`), a sparse
  file (or a torrent's folder) never allocated in full. It is deleted when the stream stops, when
  nobody reads it for the idle time, by the cache limit (only streams nobody reads), and at the next
  start of OctoMX (a cache is never kept across a quit or a crash). It is never in the database.
  Nothing behind a read position is freed while the stream lives: a stream played to the end holds
  the whole file until it ends. `GET /api/v1/streams` and `GET /api/v1/status` show `cache`: the
  total, the limit and each stream's bytes; every stream shows its own `cachedBytes`.
* **Uploading**: what a stream holds is shared like a partial download (verified parts only; BitTorrent
  peers get the pieces it has).
* **Limits**: the download speed limit applies to streams too (the status says it when one is set);
  the proxy and network adapter settings apply as to every transfer; a stream always runs and does not
  count as one of the "downloads at once".

## What it does not do

* Formats are not converted: the player must know the file's format (any player knows mp4 / mkv /
  avi / webm).
* eD2k / Kad sources often keep a requester in their upload queue for minutes: a file only eD2k has
  can take long to start, and the bytes are served unverified until their 9.28 MB part is complete.
* A torrent added by magnet needs its file list from a peer before the first byte (the request waits; a
  single file other networks have goes on from them after 10 s).
* The cache limit does not cut a stream that is being played: its cache can grow to the size of its file.

## Downloads

Every download of the Transfers window is a resource of the API: the ones a program starts here and
the ones the user starts in the window (each says where it came from: `"origin": "api"` or `"user"`).
A program starts a download of **any file type** from a search result or a link, reads its state,
progress, speed, sources and peers, pauses it, resumes it, changes it and removes it from the list,
with or without its files. What the user changes in the window shows in the next answer, and what a
program changes shows in the window at once.

A download goes where a download of the window goes: the incoming folder, under the name its network
gives, with the user's own limits (downloads at once, speed limits). No call names a folder or a file.

In the examples, as above:

    T=0123456789abcdef0123456789abcdef0123456789abcdef
    A=http://127.0.0.1:17580/api/v1

The answers below come from a run on the real networks (a search for an Ubuntu disc image), shortened
where an answer is long (`...`); the profile folder of that run is shown as
`/home/user/Downloads/OctoMX`.

### The download calls

| Call | What it does |
|---|---|
| `POST /api/v1/downloads` | Start a download: `{"search": id, "result": key}` (a search result, any file type; for a bundle optionally `"files": [indexes]`) or `{"link": "magnet:... / ed2k://... / http(s)://..."}`. Optional `"paused": true`, `"priority": -1 / 0 / 1`. `201` with the download; the same file in the list already: `200` with that download and `"existing": true` |
| `GET /api/v1/downloads` | Every download of the list in a short form; `offset`, `limit` (at most 500, default 100), `state` (one or more [state words](#state-words), comma separated); `totals` (count per state, speeds) |
| `GET /api/v1/downloads/<id>` | One download in full; `?sources=1` adds its sources |
| `POST /api/v1/downloads/<id>` | Change it: `priority`, `files` (the wanted files of a bundle), `sequential`, `firstLastFirst` |
| `POST /api/v1/downloads/<id>/pause` | Pause it (the new status; pausing a paused one changes nothing) |
| `POST /api/v1/downloads/<id>/resume` | Continue it (a download that failed is tried again, as with the window's Resume) |
| `POST /api/v1/downloads/<id>/sources` | Ask every network for more sources now ("Find More Sources") |
| `DELETE /api/v1/downloads/<id>` | Take it off the list. `?files=keep` (the default): its files stay; `?files=delete`: its files are deleted for good; `?files=trash`: they go to the trash / Recycle Bin. `delete` and `trash` need "Programs may delete downloaded files" |
| `POST /api/v1/downloads/pause`, `/resume` | Pause or continue every download of the list |
| `DELETE /api/v1/downloads?state=completed` | Clear the finished downloads off the list (their files stay), like the window's Clear Completed |
| `GET /api/v1/status` | Also counts the downloads per state and their total speeds |

The action calls (`pause`, `resume`, `sources`, and `pause` / `resume` of all) take an empty body,
with or without a `Content-Type`; a body that is there must be JSON. Every other POST needs a JSON
body as before.

### POST /api/v1/downloads

From a search result (the `key` of a result of `GET /api/v1/searches/<id>`):

    curl -s -H "Authorization: Bearer $T" -H "Content-Type: application/json" \
         -d '{"search": 1, "result": "f54c559160d35ed5"}' $A/downloads

    HTTP 201
    {
        "added": "2026-11-27T22:10:30Z",
        "bundle": false,
        "completed": null,
        "diagnosis": null,
        "done": 0,
        "error": null,
        "etaSec": null,
        "existing": false,
        "firstLastFirst": false,
        "hashes": {
            "btih": "2aa4f5a7e209e54b32803d43670971c4c8caaa05"
        },
        "id": "dc6083110ca84e240",
        "limits": {
            "downloadLimitKBs": 0,
            "downloadsAtOnce": 40,
            "uploadLimitKBs": 0
        },
        "magnet": "magnet:?xt=urn:btih:2aa4f5a7e209e54b32803d43670971c4c8caaa05&xl=6114656256&dn=ubuntu-24.04-desktop-amd64.iso",
        "name": "ubuntu-24.04-desktop-amd64.iso",
        "networks": [
            "BitTorrent",
            "Gnutella"
        ],
        "origin": "api",
        "path": null,
        "paused": false,
        "peers": 0,
        "peersTotal": null,
        "priority": 0,
        "progress": 0,
        "seeds": 0,
        "seedsTotal": null,
        "sequential": false,
        "size": 6114656256,
        "sizeKnown": true,
        "sources": 1,
        "sourcesActive": 0,
        "speedBps": 0,
        "state": "finding_sources",
        "statusText": "Fetching torrent from BitSearch...",
        "stream": null,
        "uploadBps": 0,
        "url": "/api/v1/downloads/dc6083110ca84e240"
    }

Fields:

* `search` and `result`: a search result. A result that cannot be downloaded (`"downloadable": false`
  in the results) answers `409 no_sources`.
* `files` (a bundle, a torrent of several files): the indexes of the files to fetch (the `index` of the
  result's `files`); left out = every file. A bundle whose file list is not known yet (`"files": null`)
  answers `409 files_unknown` and the list is asked for: start again in a few seconds, or leave `files`
  out. An index that is not in the list: `400 file`.
* `link` instead of `search` and `result`: a `magnet:?` link, an `ed2k://` link or an `http://` /
  `https://` link (a web server, or a `.torrent` file there). Nothing else: no local file, no `file:`,
  no `ftp:` (`400 link`). `files` cannot go with a link (the file list is not known yet): change the
  wanted files later with `POST /api/v1/downloads/<id>`.
* `paused`: `true` = it is added paused (nothing is fetched until `resume`).
* `priority`: `-1` low, `0` normal (default), `1` high.
* A field that names a place (`saveDir`, `path`, `folder`, `dir`, `directory`, `target`, `name`,
  `fileName`) is refused (`400 not_allowed`): the API does not choose folders or file names.

The answer is the download in full (as `GET /api/v1/downloads/<id>`) and:

* `existing`: `true` when the same file is in the list already (`200`). That download is not changed
  (not resumed, not re-prioritised); a finished one says `completed`.
* `continued`: `true` when the file was removed from the list earlier with its data kept: the
  download continues from that data, as in the window.
* `warning`: when the window would warn: a program or script, or a file that can hide one (exe, scr,
  com, bat, pif, vbs, cmd, msi, zip, rar, js, ps1, lnk, jar, sh), and when the result looks like a fake
  (the reason). The API downloads it all the same (a program asked for exactly this file) and never
  opens a file. With "Suppress dangerous filetype warning" in the settings the first part is left out.
* `fromStream`: the file was being streamed with its own cache: that cache became this download (see
  [streams and downloads together](#streams-and-downloads-together)).

A link:

    curl -s -H "Authorization: Bearer $T" -H "Content-Type: application/json" \
         -d '{"link": "magnet:?xt=urn:btih:2aa4f5a7e209e54b32803d43670971c4c8caaa05&dn=ubuntu-24.04-desktop-amd64.iso"}' $A/downloads

    HTTP 201
    {
        "existing": false,
        "hashes": {
            "aich": "SKUIHMRGR3WINGE55OG575HKIEXEDWBK",
            "btih": "2aa4f5a7e209e54b32803d43670971c4c8caaa05",
            "sha1": "BCXPCM2XZ2CQQL7IFEVSFZWRRB6UN2RX",
            "tth": "EET5OE6Z3ZDMNR4BMXXF557AEWAXC6DQVSW7XIQ"
        },
        "id": "d5570e59e86fca8c7",
        "name": "ubuntu-24.04-desktop-amd64.iso",
        "origin": "api",
        "size": 0,
        "sizeKnown": false,
        "state": "finding_sources",
        "statusText": "Finding sources... looking for peers (DHT, trackers)",
        ...
    }

### GET /api/v1/downloads

    curl -s -H "Authorization: Bearer $T" "$A/downloads?limit=20"

    {
        "count": 1,
        "downloads": [
            {
                "added": "2026-11-27T22:10:30Z",
                "bundle": false,
                "completed": null,
                "done": 703856640,
                "error": null,
                "etaSec": 448,
                "id": "dc6083110ca84e240",
                "name": "ubuntu-24.04-desktop-amd64.iso",
                "networks": [
                    "BitTorrent",
                    "eD2k",
                    "Kad",
                    "Gnutella"
                ],
                "origin": "api",
                "paused": false,
                "peers": 31,
                "peersTotal": 49,
                "priority": 0,
                "progress": 0.1151,
                "seeds": 46,
                "seedsTotal": 29,
                "size": 6114656256,
                "sizeKnown": true,
                "sources": 84,
                "sourcesActive": 78,
                "speedBps": 12071285,
                "state": "transferring",
                "statusText": "Transferring",
                "stream": null,
                "uploadBps": 148170,
                "url": "/api/v1/downloads/dc6083110ca84e240"
            }
        ],
        "limit": 100,
        "offset": 0,
        "totals": {
            "byState": {
                "cancelled": 0,
                "completed": 0,
                "connecting": 0,
                "error": 0,
                "finding_sources": 0,
                "paused": 0,
                "queued": 0,
                "remotely_queued": 0,
                "seeding": 0,
                "timed_out": 0,
                "transferring": 1,
                "verifying": 0
            },
            "count": 1,
            "speedBps": 12071285,
            "uploadBps": 148170
        }
    }

* `count`: the downloads that match `state` (all without it); `offset`, `limit`: as given.
* `downloads`: the short form of each (no `path`, `hashes`, `diagnosis`, `limits`; at most 20 files of a
  bundle, `filesNotShown` says how many more).
* `totals`: of the whole list, whatever `state` asks: `count`, `byState` (every [state
  word](#state-words), also the ones at 0), `speedBps` and `uploadBps` (the sums of the downloads).
* `?state=paused`, `?state=transferring,remotely_queued`: only those (an unknown word: `400 state`).
* A stream's own cache (a stream without `"download": true`) is not a download of this list: it is in
  `GET /api/v1/streams`.

### GET /api/v1/downloads/&lt;id&gt;

    curl -s -H "Authorization: Bearer $T" $A/downloads/dc6083110ca84e240

    {
        "added": "2026-11-27T22:10:30Z",
        "bundle": false,
        "completed": null,
        "diagnosis": null,
        "done": 703856640,
        "error": null,
        "etaSec": 448,
        "firstLastFirst": false,
        "hashes": {
            "aich": "SKUIHMRGR3WINGE55OG575HKIEXEDWBK",
            "btih": "2aa4f5a7e209e54b32803d43670971c4c8caaa05",
            "sha1": "BCXPCM2XZ2CQQL7IFEVSFZWRRB6UN2RX",
            "tth": "EET5OE6Z3ZDMNR4BMXXF557AEWAXC6DQVSW7XIQ"
        },
        "id": "dc6083110ca84e240",
        "limits": {
            "downloadLimitKBs": 0,
            "downloadsAtOnce": 40,
            "uploadLimitKBs": 0
        },
        "magnet": "magnet:?xt=urn:btih:2aa4f5a7e209e54b32803d43670971c4c8caaa05&xt=urn:bitprint:BCXPCM2XZ2CQQL7IFEVSFZWRRB6UN2RX.EET5OE6Z3ZDMNR4BMXXF557AEWAXC6DQVSW7XIQ&xt=urn:aich:SKUIHMRGR3WINGE55OG575HKIEXEDWBK&xl=6114656256&dn=ubuntu-24.04-desktop-amd64.iso",
        "name": "ubuntu-24.04-desktop-amd64.iso",
        "networks": [
            "BitTorrent",
            "eD2k",
            "Kad",
            "Gnutella"
        ],
        "origin": "api",
        "path": "/home/user/Downloads/OctoMX/incoming/ubuntu-24.04-desktop-amd64.iso",
        "paused": false,
        "peers": 31,
        "peersTotal": 49,
        "priority": 0,
        "progress": 0.1151,
        "seeds": 46,
        "seedsTotal": 29,
        "sequential": false,
        "size": 6114656256,
        "sizeKnown": true,
        "sources": 84,
        "sourcesActive": 78,
        "speedBps": 12071285,
        "state": "transferring",
        "statusText": "Transferring",
        "stream": null,
        "uploadBps": 148170,
        "url": "/api/v1/downloads/dc6083110ca84e240"
    }

| Field | What it is |
|---|---|
| `id`, `url` | The download's id (as in the window, stable across restarts) and this call's path |
| `name` | The file or bundle name |
| `state` | One of the [state words](#state-words) |
| `statusText` | What the Transfers window shows ("Transferring", "Finding sources...", "Paused") |
| `error` | Why it failed, or `null` |
| `size`, `sizeKnown` | Bytes (of the wanted files of a bundle); `sizeKnown` is `false` while a torrent's file list has not arrived (`size` 0) |
| `done`, `progress` | Bytes received and verified; `progress` 0 to 1 (1 when finished) |
| `speedBps`, `uploadBps` | Bytes per second now, down and up (a torrent uploads while it downloads) |
| `etaSec` | Seconds left at the speed now, or `null` |
| `sources`, `sourcesActive` | Sources known, sources sending now |
| `peers`, `seeds` | BitTorrent: peers and seeds connected now; `peersTotal`, `seedsTotal`: the swarm as trackers or the DHT report it. `null` where a network does not tell them (eD2k, Gnutella, HTTP: see `sources`) |
| `networks` | The networks it is fetched from now / has sources on |
| `priority` | -1 low, 0 normal, 1 high |
| `paused` | Paused by the user or a program |
| `bundle`, `files` | A torrent of several files: `true` once its file list is known; `files` with `index`, `path`, `size`, `done` and `wanted` (`null` while the list is not known) |
| `origin` | `api` (started through the API) or `user` (in the window) |
| `stream` | The id of a stream playing it, or `null` |
| `added`, `completed` | ISO 8601 times in UTC, or `null` |
| `path` | Where it is on the disk: the part file in the incomplete folder while it runs, the finished file or folder in the incoming folder. It shows a folder name of the user to whoever holds the token |
| `hashes`, `magnet` | The hashes known, as the links write them: `btih`, `ed2k`, `sha256`, `md5` in hex, `sha1`, `tth`, `aich` in base32; a magnet link of the file |
| `diagnosis` | The engine's sentence on why it is slow or stuck (as in the window's tool tip), or `null` |
| `sequential`, `firstLastFirst` | The download order (Transfers context menu) |
| `limits` | The user's limits that apply: `downloadsAtOnce`, `downloadLimitKBs`, `uploadLimitKBs` (0 = none) |
| `sourceList` | With `?sources=1`: each source with `network`, `status`, `bytes`, `speedBps`, `client`. No addresses and no user names of other people |

With `?sources=1`:

    curl -s -H "Authorization: Bearer $T" "$A/downloads/dc6083110ca84e240?sources=1"

    {
        "id": "dc6083110ca84e240",
        "name": "ubuntu-24.04-desktop-amd64.iso",
        "peers": 31,
        "seeds": 46,
        "sourceList": [
            {
                "bytes": 1705007,
                "client": "gtk-gnutella/1.2.3-103-gffaf0-dirty (2024-03-03; GTK2; Linux x86_64)",
                "network": "Gnutella",
                "speedBps": 23933,
                "status": "Transferring (confirmed)"
            },
            {
                "bytes": 1079296,
                "client": "aMule v2.3.3",
                "network": "Kad",
                "speedBps": 25604,
                "status": "Transferring (confirmed)"
            },
            {
                "bytes": 9699328,
                "client": "eMule v0.60d",
                "network": "eD2k",
                "speedBps": 0,
                "status": "Remotely Queued 36 (confirmed)"
            },
            ...
        ],
        "sources": 84,
        "sourcesActive": 78,
        "state": "transferring",
        ...
    }

### POST /api/v1/downloads/&lt;id&gt;/pause, /resume, /sources

    curl -s -X POST -H "Authorization: Bearer $T" $A/downloads/dc6083110ca84e240/pause

    {
        "done": 703856640,
        "id": "dc6083110ca84e240",
        "name": "ubuntu-24.04-desktop-amd64.iso",
        "paused": true,
        "sourcesActive": 76,
        "speedBps": 0,
        "state": "paused",
        "statusText": "Paused",
        ...
    }

Pause stops fetching at once (what was on its way may still arrive); resume goes on from what is
there, nothing is fetched twice. Both answer with the new status and change nothing when the
download is in that state already (a finished download cannot be paused). `sources`:

    curl -s -X POST -H "Authorization: Bearer $T" $A/downloads/dc6083110ca84e240/sources

    {
        "id": "dc6083110ca84e240",
        "message": "Every network is asked for more sources now; new ones show in a few seconds.",
        "name": "ubuntu-24.04-desktop-amd64.iso",
        "sources": 85,
        "state": "transferring",
        ...
    }

### POST /api/v1/downloads/&lt;id&gt;

    curl -s -H "Authorization: Bearer $T" -H "Content-Type: application/json" \
         -d '{"priority": 1}' $A/downloads/dc6083110ca84e240

    {
        "id": "dc6083110ca84e240",
        "name": "ubuntu-24.04-desktop-amd64.iso",
        "priority": 1,
        "state": "transferring",
        ...
    }

Fields (any of them; at least one): `priority` (-1, 0, 1), `files` (the wanted files of a bundle,
indexes; `409 files_unknown` while its file list is not known; the files no longer wanted stop, what
they hold stays), `sequential` and `firstLastFirst` (`true` / `false`). Nothing else is changed here
(no folder, no name).

### DELETE /api/v1/downloads/&lt;id&gt;

"Stop" in the owner's words: the download leaves the list. What happens to its files:

| `?files=` | Its files | Needs |
|---|---|---|
| `keep` (default) | Stay. A finished file stays in the incoming folder; partial data stays in the incomplete folder and the download continues from it when the same file is downloaded again (`"continued": true`), as in the window | nothing |
| `delete` | Deleted for good (a program on the other end has nobody who empties a trash) | "Programs may delete downloaded files" |
| `trash` | Moved to the trash / Recycle Bin; what cannot go there stays and is named in `failed` | "Programs may delete downloaded files" |

Only what the download created goes: the part file or the finished file, a torrent's files and the
folder it made (when nothing else is in it), its resume data. A file that was on the disk before the
download (a torrent that found one of its files there) stays in every case (`kept`). A download the
user resumed in the window from a file of his own ("Manually Resume") has made that file its data: it
is deleted with the rest, and the answer names it (`inPlaceFile`). Deleting works for a running, a
paused and a finished download.

    curl -s -X DELETE -H "Authorization: Bearer $T" "$A/downloads/dc6083110ca84e240?files=delete"

    {
        "deleted": [
            "/home/user/Downloads/OctoMX/incoming/ubuntu-24.04-desktop-amd64.iso"
        ],
        "download": {
            "done": 827211776,
            "id": "dc6083110ca84e240",
            "name": "ubuntu-24.04-desktop-amd64.iso",
            "origin": "api",
            "size": 6114656256,
            "state": "paused",
            ...
        },
        "errors": [],
        "failed": [],
        "files": "delete",
        "id": "dc6083110ca84e240",
        "kept": [],
        "message": "Removed from the list; its files are deleted.",
        "name": "ubuntu-24.04-desktop-amd64.iso",
        "pending": false,
        "removed": true,
        "state": "removed",
        "trashed": []
    }

The answer comes when the files are gone (a torrent's files can be deleted only after libtorrent has
closed them), at most after 15 s; when that takes longer it is `202` with `"pending": true`, and the
outcome can be read with `GET /api/v1/downloads/<id>`. A removed download stays readable for 10
minutes: `410` with that record:

    curl -s -H "Authorization: Bearer $T" $A/downloads/dc6083110ca84e240

    HTTP 410
    {
        "deleted": [
            "/home/user/Downloads/OctoMX/incoming/ubuntu-24.04-desktop-amd64.iso"
        ],
        "download": {
            "done": 827211776,
            "id": "dc6083110ca84e240",
            "name": "ubuntu-24.04-desktop-amd64.iso",
            "origin": "api",
            "size": 6114656256,
            "state": "paused",
            ...
        },
        "error": "removed",
        "errors": [],
        "failed": [],
        "files": "delete",
        "id": "dc6083110ca84e240",
        "kept": [],
        "message": "Removed from the list; its files are deleted.",
        "name": "ubuntu-24.04-desktop-amd64.iso",
        "pending": false,
        "removed": true,
        "state": "removed",
        "trashed": []
    }

With the switch off, `files=delete` and `files=trash` are refused and nothing is touched:

    HTTP 403
    {
        "error": "delete_not_allowed",
        "message": "Programs may not delete downloaded files here: the user has not allowed it (Settings, Local API, \"Programs may delete downloaded files\"). Without ?files the download leaves the list and its files stay."
    }

A stream that plays the download ends first (its link answers `410`); the answer names it
(`streamEnded`).

### POST /api/v1/downloads/pause, /resume

    curl -s -X POST -H "Authorization: Bearer $T" $A/downloads/pause

    {
        "paused": 1,
        "totals": {
            "byState": {
                "cancelled": 0,
                "completed": 0,
                "connecting": 0,
                "error": 0,
                "finding_sources": 0,
                "paused": 1,
                "queued": 0,
                "remotely_queued": 0,
                "seeding": 0,
                "timed_out": 0,
                "transferring": 0,
                "verifying": 0
            },
            "count": 1,
            "speedBps": 0,
            "uploadBps": 282920
        }
    }

`paused` / `resumed`: how many changed. Finished downloads are left as they are.

### DELETE /api/v1/downloads?state=completed

    curl -s -X DELETE -H "Authorization: Bearer $T" "$A/downloads?state=completed"

    {
        "cleared": 0,
        "ids": []
    }

The finished downloads (completed, seeding, cancelled) leave the list; their files stay. Only
`state=completed` is taken here (`400` otherwise); the upload list is not touched.

### GET /api/v1/status for downloads

    curl -s -H "Authorization: Bearer $T" $A/status

    {
        "api": "on",
        "deleteFilesAllowed": false,
        "downloads": {
            "byState": {
                "cancelled": 0,
                "completed": 0,
                "connecting": 0,
                "error": 0,
                "finding_sources": 0,
                "paused": 0,
                "queued": 0,
                "remotely_queued": 0,
                "seeding": 0,
                "timed_out": 0,
                "transferring": 1,
                "verifying": 0
            },
            "count": 1,
            "speedBps": 548897,
            "uploadBps": 282920
        },
        "searches": 2,
        "streams": 1,
        "types": [
            {
                "code": "any",
                "matches": "every file"
            },
            {
                "code": "audio",
                "extensions": [
                    "mp3",
                    "flac",
                    "m4a",
                    "ogg",
                    "aac",
                    "opus",
                    ...
                ],
                "matches": "audio files; audio:min:<kbps>, audio:eq:<kbps>, audio:max:<kbps> also limit the bit rate where a network tells it"
            },
            {
                "code": "video",
                "extensions": [
                    "mkv",
                    "mp4",
                    "avi",
                    "mov",
                    "webm",
                    "m4v",
                    ...
                ],
                "matches": "video files"
            },
            ...
        ],
        "version": "5.10",
        ...
    }

### State words

One word per state of a download (`state`, the `state` filter, `byState`):

| Word | The window says | What it means |
|---|---|---|
| `queued` | Locally Queued | Waits for a free place ("downloads at once") |
| `finding_sources` | Finding sources... | Looks for sources, or for a torrent's file list |
| `connecting` | Connecting... | Connects to its sources |
| `remotely_queued` | Remotely Queued | Every source has it in its upload queue |
| `transferring` | Transferring | Bytes arrive |
| `paused` | Paused | Paused by the user or a program |
| `verifying` | Verifying | Its data is checked |
| `completed` | Complete | Finished: the file is in the incoming folder |
| `seeding` | Seeding | Finished and still uploading (BitTorrent) |
| `error` | Error: ... | Failed (`error` says why); `resume` tries again |
| `cancelled` | Cancelled | Stopped for good |
| `timed_out` | Timed Out | Gave up waiting; `resume` tries again |

A removed download's record (`410`) says `"state": "removed"`.

### Errors of the download calls

| Status | `error` | When |
|---|---|---|
| 400 | `what` | Neither or both of `link` and `search` + `result` |
| 400 | `link` | A link that is not magnet:, ed2k:// or http(s):// |
| 400 | `not_allowed` | A field that names a folder, a file name or a path |
| 400 | `files`, `file` | `files` is not a list of indexes, the result is one file, or an index the bundle does not have |
| 400 | `paused`, `priority`, `sequential`, `firstLastFirst` | Not `true` / `false`, or a priority other than -1, 0, 1 |
| 400 | `nothing` | A change with nothing to change |
| 400 | `state` | An unknown state word, or `DELETE /downloads` without `state=completed` |
| 400 | `files` | `?files=` other than keep, trash, delete |
| 400 | `json` | A body that is not a JSON object |
| 403 | `delete_not_allowed` | `files=delete` or `files=trash` while "Programs may delete downloaded files" is off |
| 404 | `not_found` | No such download, search or result (a stream's own cache: the answer has `stream`) |
| 405 | `method` | Another method (the `Allow` header lists the ones it takes) |
| 409 | `search_empty` | The search has no results yet |
| 409 | `no_sources` | The result names no source and no hash |
| 409 | `files_unknown` | `files` given and the bundle's file list is not known yet (it was asked for) |
| 409 | `not_started` | The engine did not start it; `message` is its reason ("Invalid link", ...) |
| 410 | `removed` | Removed through the API (the record of what happened) |
| 415 | `content_type` | A body that is not sent as JSON (the action calls take an empty body) |

The errors of the whole API (`401`, `403 host`, `403 web_page`, `413`, `429`, `431`, `503`) are as in
[Safety](#safety).

### Searching for downloads

`POST /api/v1/searches` takes the Search page's type codes in `type`, or several in `types`. The status
call lists them (`types`) with their extensions:

| Code | Matches |
|---|---|
| `any` | Every file (the default) |
| `audio` | Audio files; `audio:min:<kbps>`, `audio:eq:<kbps>`, `audio:max:<kbps>` also limit the bit rate where a network tells it |
| `video` | Video files |
| `pictures` | Pictures |
| `documents` | Documents: e-books, comics, office files, web pages, subtitles |
| `software` | Programs, installers, packages and disc images |
| `archives` | Archives (zip, rar, 7z, ...) |
| `discimages` | CD / DVD images (iso, img, bin, cue, nrg, mdf, ...); every network is asked for every type (an image can hold anything), the extension decides |
| `torrents` | .torrent files |
| `mp3` | MP3 audio only; `mp3:min:<kbps>`, `mp3:eq:<kbps>`, `mp3:max:<kbps>` |
| `lossless` | Lossless audio (flac, wav, ape, ...) |
| `servers` | Server lists (.met, .wsx) |
| `clr` | Colour schemes (.clr) |
| `custom:<ext>,<ext>` | Files with one of these extensions |

`"types": ["video", "audio"]` finds results of either type (the networks are asked for the type they
share, else for every type, and each result is checked against each code). `extension` decides alone
when it is given, as on the Search page.

`networks` left out or empty: every network that is switched on and can search. A network named that
cannot be asked now is said:

    curl -s -H "Authorization: Bearer $T" -H "Content-Type: application/json" \
         -d '{"text": "debian netinst", "type": "discimages", "networks": ["eD2k", "Gnutella"]}' $A/searches

    {
        "id": 2,
        "networks": ["eD2k"],
        "running": true,
        "skipped": [
            {"network": "Gnutella", "reason": "switched off", "state": "disabled"}
        ],
        "url": "/api/v1/searches/2"
    }

`reason`: `switched off` (Networks page), `not running`, or `cannot search` (HTTP only fetches links).
When no network named can be asked, `note` says the search finds nothing. Every result says
`downloadable` (and `notDownloadable` with the reason when it is `false`).

### Streams and downloads together

* A stream of a result (`POST /api/v1/streams` without `"download"`) has its own cache: it is a stream,
  not a download of this list (`GET /api/v1/streams` shows it); `GET /api/v1/downloads/<its download
  id>` is `404` and names the stream.
* `POST /api/v1/streams` with `"download": true` starts a normal download: its `download` id is a
  download of this list (`"origin": "api"`, `"stream"`: the stream's id).
* A download started here can be streamed later with `POST /api/v1/streams` of the same result: the
  stream joins the download (`"ownCache": false`); stopping the stream leaves the download.
* `POST /api/v1/downloads` of a result that is streamed with its own cache turns that cache into the
  download (`fromStream`): the stream goes on, and the download completes into the incoming folder
  when the stream stops.
* Deleting a download that a stream plays ends the stream first (`streamEnded` in the answer).

### Limits and warnings

* The user's limits apply as in the window: downloads at once (a download beyond them is `queued`),
  the speed limits, the proxy and network adapter settings; `limits` in the full form shows them.
* A program or script, or a file that can hide one, is downloaded with a `warning` in the answer (see
  POST); the API never opens a file.
* At most 32 connections and 16 from one address, as for every call. There is no limit of downloads a
  program can start other than the user's own.

### Walk-through: search, pick, download, watch, remove

    T=$(...the token from Settings...)
    A=http://127.0.0.1:17580/api/v1
    # 1. search: disc images only, on every network
    curl -s -H "Authorization: Bearer $T" -H "Content-Type: application/json" \
         -d '{"text": "ubuntu 24.04", "type": "discimages"}' $A/searches
    # 2. a few seconds later, the results: pick one with "downloadable": true and many seeds
    curl -s -H "Authorization: Bearer $T" "$A/searches/1?limit=20"
    # 3. download it
    curl -s -H "Authorization: Bearer $T" -H "Content-Type: application/json" \
         -d '{"search": 1, "result": "f54c559160d35ed5"}' $A/downloads
    # 4. watch it: state, done, progress, speedBps, sources, peers, seeds, etaSec
    curl -s -H "Authorization: Bearer $T" $A/downloads/dc6083110ca84e240
    #    or every download with the totals
    curl -s -H "Authorization: Bearer $T" $A/downloads
    # 5. pause, resume
    curl -s -X POST -H "Authorization: Bearer $T" $A/downloads/dc6083110ca84e240/pause
    curl -s -X POST -H "Authorization: Bearer $T" $A/downloads/dc6083110ca84e240/resume
    # 6a. finished: the file is at "path"; take it off the list, the file stays
    curl -s -X DELETE -H "Authorization: Bearer $T" $A/downloads/dc6083110ca84e240
    # 6b. or remove it with its files (needs "Programs may delete downloaded files")
    curl -s -X DELETE -H "Authorization: Bearer $T" "$A/downloads/dc6083110ca84e240?files=delete"

What step 4 showed every 10 s in the run behind these examples (an Ubuntu disc image over BitTorrent):

| Time | state | done | speedBps | sources | sourcesActive | peers | seeds |
|---|---|---|---|---|---|---|---|
| 0 s | transferring | 47448064 | 3287424 | 84 | 51 | 30 | 24 |
| 10 s | transferring | 122683392 | 7159229 | 84 | 67 | 34 | 33 |
| 20 s | transferring | 234881024 | 9723427 | 84 | 72 | 34 | 36 |
| 30 s | transferring | 351272960 | 11668570 | 84 | 75 | 34 | 40 |
| 40 s | transferring | 454557696 | 11154059 | 84 | 80 | 35 | 42 |
| 50 s | transferring | 581173248 | 11849227 | 86 | 83 | 33 | 45 |
| resume+0 s | connecting | 714866688 | 9459 | 84 | 0 | 0 | 0 |
| resume+10 s | transferring | 716439552 | 177621 | 85 | 23 | 12 | 12 |

## What changed since 5.9: only additions

A program written for the 5.7 to 5.9 API works unchanged: no path, method, field name, field type,
status code or error code changed its meaning; answers only gained fields. The test
`tests/stream/tst_api_compat.cpp` plays the walk-through of 5.7 call by call and checks every field
this document shows for it.

* New: the downloads calls (`/api/v1/downloads...`) and the setting "Programs may delete downloaded
  files" (off).
* `GET /api/v1/status`: gained `downloads`, `types`, `deleteFilesAllowed`.
* `POST /api/v1/searches`: gained `types`; the answer gained `networks`, `skipped` and `note`. `networks`
  left out or empty now means every network as this document always said (before, the Advanced setting
  "Networks to search" applied to API searches too).
* Search results gained `downloadable` and `notDownloadable`.
* The action calls of downloads take an empty body; every call that took a body before still needs JSON.
* `POST /api/v1/streams` with `"download": true` works as before; its download is now visible in
  `/api/v1/downloads`. Its first answer says `"state": "waiting for a player"` as this document shows
  (5.9 said `waiting` in the first second). Re-selecting a stream with its own cache with `"download":
  true` keeps the download when the stream stops later (5.9 could drop it then).
* The Settings page is called Local API (was Streaming API); its settings keys are the same.

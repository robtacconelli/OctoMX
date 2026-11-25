# The OctoMX streaming API

OctoMX can be used by another program, on this computer or on a TV or PC in your home network, to
**search every network, pick a result and get a link that a video player opens**. The player gets
plain HTTP with Range requests; OctoMX fetches the bytes the player reads, when it reads them, from
every network that has the file (BitTorrent, eD2k / Kad, Gnutella, HTTP mirrors, the OctoMX
network), and deletes them again afterwards.

The API is **off by default**. Nothing listens until you switch it on.

## Switching it on

Settings, Other Networks, **Streaming API**:

| Setting | Default | What it does |
|---|---|---|
| Switch on the streaming API | off | Listens on 127.0.0.1 and ::1 (this computer only). Applies at once. |
| Port | 17580 | The port it listens on. A port another program holds is said on the page and in the log; no other port is taken by itself. |
| Allow other devices on my network | off | Also listens on this computer's private addresses (10.x, 172.16 to 31.x, 192.168.x, fc00::/7), never on a public one. Every device there that knows the token can then search and start transfers. |
| Access token | made when first switched on | Every API call must send it. **Copy** puts it in the clipboard, **New** makes a new one (the old token and every old stream link stop working). |
| Address | | The base address to give the program, for example `http://127.0.0.1:17580/api/v1/`. |
| All stream caches at most | 4096 MB | When all stream caches together hold more, streams nobody reads are ended and their caches deleted, the longest unread first. A stream that is being played is never cut: its cache can grow to the size of its file. The status calls show each cache and the total against the limit. |
| Delete unread streams after | 240 minutes (4 hours) | A stream nobody reads (no player, or a paused one) for this long ends and its cache is deleted. |
| Keep what was streamed as a download | off | When a stream ends, it becomes a normal download that completes into the incoming folder, instead of being deleted. |

## Safety

The API can start transfers, so it is treated as a door:

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
* The API reads and writes nothing but its own streams: it cannot read or change settings, reach the
  shared folders or any other file.

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
        "version": "5.7"
    }

### POST /api/v1/searches

The same search as the Search page, with the same settings (the adult and fake filters, the
maximum results). Fields: `text` (keywords, required unless `link` is given), `text2` (a second
keyword line), `type` (a file type code of the Search page: `any`, `video`, `audio`, `mp3`,
`documents`, ...), `extension`, `minSize`, `maxSize` (bytes), `networks` (names such as
`"BitTorrent"`, `"eD2k"`; empty = every network). `link` searches by a magnet / ed2k link or a hash.

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

# OctoMX overlay protocol, wire version 1

Owner: overlay task. Implementation: `overlay_*.cpp` in this directory. Status: implemented and
tested (`tests/overlay`). Every field described here is checked on input. Unknown fields are
ignored and unknown message types are dropped, so later versions can add things without breaking
version-1 peers.

**4.3 additions (worker ovl-2, still wire version 1, all capability-flagged):** capabilities in
hello / presence (`caps`, section 3), the presence interval `pi` and per-user timeouts (section 4),
delivery receipts and refusal reasons for private messages (`pm_ack`, `pm_err`, section 5.2), relayed
control messages on `mx/relay` (`pm_ack`, `pm_err`, `reach`, `ping`, `pong`), connect-back and relayed
circuits for browse / whois / transfers (section 6.1), the `hotlist` refusal reason for browse. The
port probe of diag-1 is documented in section 11, compatibility with 4.2 and older in section 12.

## 1. Transport (librats)

* One librats `Node` with `protocol = P`, the protocol string `P = "winmx-ng/1"`, and Noise_XX
  encryption. The protocol string is bound into the handshake, so only our nodes can connect. `P`
  (and every string derived from it below) is a protocol identifier kept for compatibility; it
  stays byte for byte, so the nodes of every version are one network. The **node id** is the
  SHA-256 of the node's Noise static key (`<dataDir>/identity.key`, managed by librats), written as
  64 hex characters. It authenticates every *direct* message.
* Discovery: mainline-DHT rendezvous on `SHA1(P)` (announce every 10 min, `get_peers`
  every 30 s), mDNS `_librats._tcp.local.` on the LAN, peer exchange, the librats peer book
  `<dataDir>/peers.json` (re-dialled at start), plus static bootstrap peers (`overlay/bootstrapPeers`,
  `"host:port"`). The listen port is `connection/tcpPort` (default 6699; TCP and UDP share it). If it
  is busy, librats tries port+1...+4 and then an ephemeral port. NAT handling (STUN, hole punching,
  relayed circuits, optional UPnP/NAT-PMP via `connection/upnp`) is whatever librats provides.
  Relay serving follows `overlay/relay`.
* Three kinds of traffic:
  * `mx`: **control channel** (raw librats channel). Payload is `'J' <JSON>` or
    `'Z' <u32 BE inflated length> <zlib deflate of JSON>`. Payloads larger than 2 KiB are
    compressed. The inflated size limit is 4 MiB (64 KiB before hello).
  * `mxd`: **data channel** for file bytes (section 7).
  * **pub-sub topics** (librats GossipSub). Every payload is a *signed envelope* (section 2).

Every control message is a JSON object with `t` (type) and `v` (wire version, 1). A receiver that
sees `v > 255` ignores the message.

## 2. Identity and signed envelopes

* **User identity**: an Ed25519 key pair in `<dataDir>/user_ed25519.key` (32 raw private bytes,
  mode 0600). **User id = the hex public key (64 chars)**. It is used in
  `mx::UserRef{net = Overlay, id = <user id>, name = <display name>}`.
* **Box key**: an X25519 key pair in `<dataDir>/user_x25519.key`. It is used to receive
  end-to-end encrypted relayed private messages.
* **Display name** = `cleanName(identity/screenName)`, with any trailing `_ddddd` removed, then
  `"_" + idSuffix`. `idSuffix` = `SHA1(user id hex)[0..4]` read as a big-endian u32, mod 100000,
  as 5 digits. Two users can pick the same screen name. The suffix (and, in the end, the id)
  tells them apart, the way the original WinMX's `Name_12345` did.
* **Signed envelope** (all pub-sub traffic):
  ```
  'S' | 0x01 | ed25519_pub[32] | sig[64] | body (compact JSON)
  sig = Ed25519( P + "\n" + topic + "\n" + body )
  ```
  The signature covers the topic, so a message cannot be replayed into another room or topic.
  The body always contains `t`, `v`, `id` (random 16 hex, used to deduplicate), `ts` (ms since
  epoch, accepted when within ±10 min of the receiver's clock) and `name` (the sender's display
  name). The **sender is the envelope key**. Nothing in the body can claim another identity.
  Envelopes are checked inside the librats validator, before delivery *and before relaying*:
  structure, size limit, signature, freshness. A forged message is therefore never forwarded.
  Size limits: room topics 8 KiB, `mx/relay` 16 KiB, all other topics 32 KiB.

## 3. Hello (binding a node to a user)

On every new connection each side sends:
```
{t:"hello", v:1, user:<ed25519 pub hex>, node:<sender node id>, x:<x25519 pub hex>, name, ts,
 client:"OctoMX", ver, files:<shared count>, conn:<connection label>, since:<online since ms>,
 addrs:["ip:port",...], caps:["pm_ack","reach","ping","part"], sig}
sig = Ed25519("hello\n" + node + "\n" + x + "\n" + name + "\n" + ts)
```
`caps` (4.3, not signed, at most 16 strings of at most 16 characters) says which optional parts of
this document the sender implements: `pm_ack` (it answers private messages with receipts and wants
receipts for its own, section 5.2), `reach` (it honours connect-back requests, section 6.1), `ping` (it
answers liveness pings through the mesh, section 4), `part` (5.3: it serves and fetches files that
are still downloading, section 7). A node without `caps` is treated as a 4.2 node.
Messages queued for a node that is being dialled are sent once its hello has been verified (before
4.3: as soon as the link came up, so a link that dropped at once lost them).
The receiver checks that `node` equals the authenticated librats sender id and verifies `sig`
against `user`. On any failure it disconnects. A hello carrying our own user id is also
disconnected (it is a second instance or a loop). Control messages that arrive before the hello
are buffered (at most 64) and processed after it.

## 4. Pub-sub topics

| topic | body `t` | fields | who |
|---|---|---|---|
| `mx/presence` | `presence` | `node, x, addrs[], files, conn, client, ver, since, pi?, caps?` | everyone, every `pi` s (4.3: 60 s; 4.2: 120 s) and when something changes |
| `mx/presence` | `bye` | - | on stop (best effort) |
| `mx/rooms` | `room` | `room` (normalised id), `title`, `topic`, `users` | for each joined room, every 60 s, sent only by the member with the smallest user id |
| `mx/room/<hex20>` | `join`, `part`, `hb`, `msg`, `topic`, `motd` | see section 5 | room members |
| `mx/search` | `q`, `src_q`, `hl_q` | see sections 6 and 8 (all carry `qid, node, addrs[]`) | askers |
| `mx/relay` | `pm` | `to` (user id), `box` (base64) | section 5.2 |
| `mx/relay` | `pm_ack`, `pm_err` | `to`, `mid` (the PM id), `reason?` | 4.3, section 5.2 |
| `mx/relay` | `reach` | `to`, `node`, `addrs[]` | 4.3, section 6.1 |
| `mx/relay` | `ping`, `pong` | `to`, `pid` | 4.3, liveness after a lost link (below) |
| `mx/hashlinks` | `hl_pub` | `h` (hashes), `size`, `n` (name) | section 8 |

Everyone subscribes to `mx/presence`, `mx/rooms`, `mx/search`, `mx/relay` and `mx/hashlinks`.
GossipSub floods them to all subscribers. The time-to-live is effectively the mesh diameter, and
messages expire through the `ts` freshness check.

**Presence** (rules of 4.3):
* A user is online while a hello-verified link to them exists, or while presence / room heartbeats
  keep arriving through the mesh. `pi` in a presence is the sender's announce interval in seconds.
* **Relayed users** (no direct link) time out after `2 x pi + 30 s` without presence: 150 s with the
  4.3 default of 60 s. Two announcements may be lost before a user disappears, and the interval is a
  trade-off: every presence is flooded to every node, so a shorter one costs the whole network
  bandwidth. A user that sends no `pi` (4.2 and older, 120 s announcements) times out after 330 s.
* **`bye`** (signed by the user, sent on a normal quit) marks the user offline at once, also when it
  comes from a directly linked user (4.2 ignored it from direct peers, so a user who quit stayed
  online up to 5.5 minutes).
* **Losing the last link to a direct peer** (quit without a bye arriving, crash, lost connection):
  if the user announced `ping` and we have other links, we publish `{t:"ping", to, pid}` on `mx/relay`;
  a user still reachable through the mesh answers `{t:"pong", to, pid}` and stays online. No pong within
  4 s, or no other links, or a 4.2 user: offline at once. A 4.2 user who is in fact still reachable
  through others reappears with their next presence (at most 120 s later).
* Going offline (by bye, lost link or time-out) also removes the user from the member lists of joined
  rooms at the same moment (Part line "X has left the room (connection lost)" / "(timed out)"), and an
  open private-message window gets the System line "X is offline (...)" / "X is online.".
* A user who comes back is online again with their first hello (direct link) or presence (sent 1 s
  after the first link, so through the mesh within a second or two); undelivered private messages to
  them are sent at that moment.

## 5. Chat

### 5.1 Rooms
* Room id = the name normalised: control characters stripped, whitespace collapsed, lower case,
  at most 64 characters. Topic = `"mx/room/" + hex(SHA1(room id))[0..20]`.
* `join {room, files, conn, node?, x?}` is sent about 1.2 s after subscribing, so the mesh has formed
  first. Each member then answers with an immediate `hb`. (4.3) Right after its join a member also
  publishes the room's `mx/rooms` entry and sends extra heartbeats 4, 9 and 16 s later, so nodes that
  just started relaying the room pass them on.
* `hb {room, files, conn, node?, x?, topic?, topicTs?, motd?, motdTs?}` every 30 s. A member that has
  been silent for 95 s is removed, and the UI gets the Part notice "(timed out)". (4.3) `node` and `x`
  are the sender's node id and box key: a member seen only in a room can be browsed, whois'd and
  messaged at once; a live `join` / `hb` / `msg` counts as presence (the member is online).
* **Relaying rooms (4.3)**: GossipSub forwards a topic only among its subscribers, so two members
  linked only through a non-member never saw each other. Every 4.3 node therefore also subscribes,
  without joining, to the topics of the rooms in its directory (the default rooms and announced ones,
  at most 64) and forwards their traffic; it drops the subscription when the room leaves the
  directory. 4.2 nodes do not, so a 4.2 node between two members does not carry the room.
* `part {room}` is sent on leave and on stop.
* `msg {room, text ≤ 1024 chars, kind?:"action"}`. `/me` is sent as `kind:"action"`. The sender
  does not receive its own message back.
* `topic {room, topic ≤ 256}`: **anyone may set the topic; last writer wins** on (`ts`, then user
  id). Timestamps more than 5 min in the future are rejected. Heartbeats carry the sender's
  current topic so late joiners converge. (No room admins in v1. A creator-key admin rule can be
  added later as new fields.)
* `motd {room, motd ≤ 1024 chars, at most 12 lines separated by "\n"}`: the room's message of the
  day, same last-writer-wins rule as the topic; heartbeats carry it too. Older clients ignore it.
* **Greeting**: a joining member shows the topic as one `Topic` line and the MOTD as one `Motd`
  line per MOTD line, as soon as the first `hb` answering its `join` arrives, at the latest
  1 s after the history request (an empty room: what it knows). Later changes are shown again;
  lines replayed from history or repeated by heartbeats are not. The default rooms have a
  built-in topic and MOTD with timestamp 0, so any `topic` / `motd` set by a member wins.
* **Flood control** (receiver side, per sender user id): token bucket of `messages/floodLimit` per
  10 s (default 6), on when `messages/blockFloods` is set. Extra messages are dropped and one
  System line "X is flooding the room and has been muted" is shown. Messages are deduplicated by
  `id`.
* **History replay**: 1.5 s after joining, the joiner sends `hist_req {room, n ≤ 50}` over the
  control channel to up to 2 directly connected peers that subscribe to the room topic. It retries
  every 2 s, up to 8 times. The answer is `hist {room, items:[base64 original envelope...]}`, the last
  N `msg`/`topic` envelopes. Each item is **verified with its author's signature** (the relaying
  peer cannot forge history), deduplicated, skipped if older than 24 h, and shown with its
  original time.
* **Directory**: `roomList()` = the default rooms (always listed: *OctoMX Lobby, Music, Movies,
  Help*), plus `room` announcements heard in the last 180 s, plus the rooms we have joined.

### 5.2 Private messages
* Direct: `pm {id, ts, name, text ≤ 2048}` on the control channel. The sender is the
  hello-verified user of the connection. `id` is 8 to 64 hex characters (4.3: the core's message id,
  16 hex) and stays the same for every copy and retry of one message.
* If the recipient's node is not linked (4.3), the message is **relayed at once, end-to-end
  encrypted** through the mesh, and we dial the recipient's known addresses in the background so later
  traffic goes direct. (4.2 dialled first and relayed only after 8 s.) The relayed form:
  `mx/relay` envelope `{t:"pm", id, to:<user id>, box}` with
  `box = eph_x25519_pub[32] | nonce[12] | ChaCha20-Poly1305(key, inner pm JSON, aad) | tag[16]`,
  `key = HKDF-SHA256(X25519(eph, recipient_x), salt = eph_pub‖recipient_x, info = P + " pm")`,
  `aad = "pm\n" + to + "\n" + id`. The outer envelope is signed by the sender, so the recipient
  knows who sent it. Relaying peers learn the sender, the recipient and the size, but not the
  text. The inner `id` must equal the outer one.
* Duplicates (a message that arrives both direct and relayed, or a retry) are shown once, by `id`.
* Privacy: `messages/allowPrivateMessages` = everyone | hotlist | nobody, and
  `messages/ignoreNonHotlist`, checked by the plugin; the core's further rules ("only from the
  following sources" in Settings) refuse through `ChatBackend::refusePrivateMessage` while the message
  is being delivered. A refused PM gets `pm_err {id, reason}`; `reason` = `hotlist` (only Hotlist
  users), `nobody`, `restricted` (the core's source rules) or `refused` (4.2). The sender shows
  "X accepts private messages only from users on their Hotlist." and so on. An ignored sender is not
  told (the message vanishes, as in the original WinMX; it counts as delivered).
* **Delivery receipts (4.3, `caps` `pm_ack`)**: the recipient's node answers every PM it showed with
  `pm_ack {id}`, every refused one with `pm_err {id, reason}`, on the path the PM came: on the control
  channel for a direct PM, as a signed `mx/relay` envelope `{t:"pm_ack"|"pm_err", to:<sender>, mid:<id>,
  reason?}` for a relayed one (no text in it). A copy of a PM it has answered already is answered again
  (the first answer may have been lost). Receipts are sent only to senders that announce `pm_ack`;
  refusals go to everyone (direct) so 4.2 senders still see them.
* **Sender side**: the core gives each PM an id (`ChatBackend::sendPrivateMessageWithId`) and the
  plugin reports `ChatEvents::privateMessageStatus(to, id, PmDelivery, detail)`: `Sending` at once;
  `Delivered` on `pm_ack`; `Refused` on `pm_err`; `Sent` when the recipient has no `pm_ack` (4.2: no
  receipt can come). A direct PM without receipt after 6 s is also relayed; a relayed one without
  receipt after 15 s is reported `NotDelivered` ("no answer from X; it is sent again when X is
  back"). A recipient who is offline, or no path at all, is reported `NotDelivered` at once ("X is
  offline; it is sent when X comes back online", "you are not connected to any other OctoMX user").
  An undelivered message is sent again when the user comes back (hello or presence after being
  offline), and every 2 minutes while they look online, for at most 1 hour; a later receipt reports
  `Delivered` with the detail "delivered now". The PM window colours our own line grey while sending,
  normal when delivered or sent, red when not delivered, and adds a red "Not delivered: ..." line.

## 6. Browse, whois, search

* `browse_req {rid}` → pages `browse {rid, page, pages, total, files:[entry...]}` of 400 entries,
  compressed. The answer is capped at 100 000 files. It is refused with `browse_err {rid, reason}`
  when `messages/allowBrowse` says so (everyone | hotlist | nobody); `reason` is `hotlist` (4.3: the
  asker sees "X does not allow browsing: only users on their Hotlist may browse") or `refused`. A
  browse that has received nothing when its link drops is sent once more (section 6.1).
  `entry = {n:name, p:"\\<parent dir>\\<name>" (never the full local path), s:size, d?:sec, b?:kbps,
  r?:Hz, m:MediaType, h:{hashes}}`.
* `whois_req {rid}` → `whois {rid, name, client, ver, files, bytes, conn, online:<sec>, free, total,
  rooms:[names]}`, or `whois_err {rid}` when `messages/allowWhois` is off. If the user cannot be
  reached, the plugin reports what presence knows, with `reachable:false`.
* **Search**: the `mx/search` envelope `q {qid, text ≤ 256, type:MediaType, ext?, exts?, min?, max?, node,
  addrs}`. Every peer matches it against its own shared files (all words are substrings of the
  file name, plus the type, extension and size filters). [ui-f] `exts` (array of ≤ 32 extensions, each
  ≤ 16 chars, lower case, no dot) widens `ext` to "any of these" (Lossless, Custom Extensions); a peer
  that does not know it answers with more files and the asker's own filter drops them. Type and
  extensions follow `mx::fileNameMatchesFileType()`: the file's extension decides. A peer with matches answers **directly
  to the asker's node** (dialling `addrs` if needed):
  `hits {qid, items:[entry...≤100], name, conn, free, total}`. The asker re-applies the same filters
  locally, drops replies for unknown `qid`s, deduplicates, and limits the total to `maxResults`.
  Each hit carries every hash the responder knows, plus a
  `SearchSource{net = Overlay, user, connection, freeSlots, totalSlots}`. The search is done after
  6 s without new hits, after 20 s in total, or after 2 s when no peer is connected. Each user may
  send at most 50 queries per second, with a burst of 200.

### 6.0 Ourselves (4.3)
Our own row (room user list, a search result, the Hotlist) is recognised by user id. Browse on it
lists our own shared files straight from the list the network sees (the same list the room, presence
and whois file counts come from), whois answers with what others would get, a private message is
refused with "this is you"; the UI disables Send Message, Add To Hotlist and Ignore on it. The screen
name shown everywhere (Networks page, title, own chat lines) is the display name of section 2: the
core stores `<base>_<suffix>` with the key-derived suffix (`NetworkManager::canonicalScreenName`),
and older profiles with a random suffix are migrated at start.

### 6.1 Reaching a user for browse, whois and transfers (4.3)
Browse, whois, search answers and transfers need a link to the user's node. When there is none, the
sender queues the request (`dialWaitMs`, 15 s) and, at the same time:
1. dials the user's announced addresses (from hello / presence);
2. publishes **connect-back** `{t:"reach", to:<user>, node:<our node>, addrs:[our addresses]}` on
   `mx/relay`: a firewalled user cannot be dialled but can dial out, so the target dials our addresses
   (and asks librats for a circuit to our node). At most one `reach` per target node per 5 s, and a node
   serves at most one per requesting user per 5 s;
3. asks librats for a **hole punch** and a **relayed circuit** to the node (`RatsNode::reachPeer`):
   librats asks up to 3 of our peers whether they hold the target and opens a circuit through the
   first that does and serves relays (`overlay/relay`, on by default). The Noise handshake runs end to
   end through the circuit; the relay sees only ciphertext. librats caps one circuit at 10 Mbit/s
   (about 1.2 MB/s) and an hour, at most 2 circuits per peer and 32 in total per relay; it keeps trying
   to replace a circuit with a punched direct link.
A user seen (in a room) whose node is not known yet (a 4.2 member before its next presence) gets the
request queued by user id and a `reach`; its hello delivers the queue. Requests still unanswered when
librats moves the peer to another link (a relayed circuit replaced by another, a punched link) are
sent again on the new link (browse, whois; a transfer source re-opens its slot or gives its
outstanding ranges back to the engine). The first link that completes the hello carries the queued
messages. A transfer source on a
relayed circuit reads "Transferring (relayed)" / "Waiting (relayed)" / "Remotely Queued (n),
relayed". If nothing worked within 15 s the request fails with "Cannot reach X: no direct connection,
no connect-back and no relay worked (when both sides are firewalled, an OctoMX user connected to both
who relays is needed)". Limits: two firewalled users need a third user linked to both who relays;
librats does not chain relays and has no reservations, so users who share no peer cannot be
connected (private messages and chat still work through the mesh).

## 7. Sources and direct transfer

* `src_q {qid, h, size}` on `mx/search`. Peers that share a file whose hashes `matches()` and whose
  size is equal answer `src {qid, h:<all hashes>, size, n, name, conn, free, total}`. The asker
  drops answers that conflict with what it knows and turns the rest into `sourcesFound`, plus
  `hashesLearned` when an answer adds hashes. The question is repeated every 5 min until
  `stopFindSources`.
* Download session (client-chosen random `sid:u32`):
  * `get_open {sid, h, size}` → `slot {sid, st}` where `st` is `ready`, `queued` (`pos` = queue
    position), `missing`, `denied` (`reason`: `hotlist` | `busy`) or `closed` (`reason`: `idle` |
    `cancelled` | ...). A queued client re-sends `get_open` every 60 s as a keep-alive. A queued
    entry is dropped after 5 min of silence.
  * `get {sid, rq:u32, off, len ≤ 4 MiB}`. The server allows at most 8 outstanding requests per
    session and serves them in order. Answers: data frames, or
    `got {sid, rq, ok:false, err}` (`no slot` | `bad range` | `read error`).
  * `get_cancel {sid}` drops the outstanding requests. `get_close {sid, reason?, why?}` ends the
    session. (5.3) `reason` says why the downloader stopped: `done` (its download is complete),
    `paused`, `cancelled`, `quit` (program closed), `banned` (`why` = the reason it shows, at most 160
    characters, e.g. "File Mismatch: a block failed the check"), `dropped` (any other: slow, failing,
    not needed now). The uploader's row ends with "Finished", "Cancelled by remote (download paused)",
    "Cancelled by remote (download cancelled)", "Cancelled by remote (program closed)", "Stopped by the
    other side: <why>" or "Cancelled by remote"; a get_close without `reason` (5.2 and older) reads
    "Finished". (5.3) get_close is sent twice: once as a librats *live* message, which leaves ahead of
    the file data already queued toward that peer, and once as an ordinary reliable message (the second
    copy finds no session and is ignored), so it does not wait behind megabytes of queued data.
  * **Data frame** (`mxd` channel):
    `'D' | 0x01 | sid:u32 BE | rq:u32 BE | off:u64 BE | payload (≤ 64 KiB)`. The frames of one
    request are contiguous and in order. The client checks `off` and drops data for cancelled
    `rq`s.
* Uploads: `transfers/maxUploads` slots in total (0 = no uploads), `transfers/maxUploadsPerUser`
  per user, and files at or below `queue/smallFileBypassKB` skip the queue. The FIFO queue follows
  `queue/enabled`, `queue/maxLength` and `queue/maxPerUser`. With `queue/hotlistPriority`, hotlist
  users queue ahead of everyone else. `transfers/uploadsToHotlistOnly` is honoured: others get
  `slot {st:"denied", reason:"hotlist"}`, shown as "Denied: uploads only to users on their Hotlist". Disk reads
  (256 KiB at a time) run on a 2-thread pool. Back-pressure: a session sends only while the
  librats send queue is under its low-water mark and resumes on `peerWritable`. Rate limits come
  from `RateLimiter::acquire(Up)`, and every byte is reported through
  `account(Up/Down, Net::Overlay, n)`. An active slot is closed after 60 s without requests.
* **Part hash sets** (added 2026-11-21, capability-flagged, still wire version 1):
  * A server that can describe the file adds `hs:["ed2k-parts"]` to every `slot` answer with `st`
    `ready` or `queued`. It does so when its library holds the eD2k part hashes of the file (the
    library hasher computes and stores them for every shared file ≥ 9 728 000 bytes; smaller files
    are one part whose hash is the ed2k hash itself, so nothing is offered).
  * (5.3) Only a set the server can vouch for: it must reduce to the file's ed2k hash (checked before
    it is offered and again before it is sent). For a file that is still downloading that is the
    trusted set its own parts are checked against, never a provisional one; with none, nothing is
    offered. 5.2 could offer a set computed over a half-downloaded file (holes included), see below.
  * The client asks once per session: `hs_req {sid, kind:"ed2k-parts"}` (allowed while queued).
  * Answer: `hs {sid, kind, ok:true, unit:9728000, n, hashes:<n × 32 hex chars, MD4 per part>}` or
    `hs {sid, kind, ok:false}` (no session, not offered, or asked more than 3 times). `n` is the
    eD2k part count with or without the trailing empty-part hash of an exact multiple of the part
    size; at most 120 000 parts (≈ 1.1 TB). The answer is compressed like any control message.
  * The client checks `unit`, `n` against the size and the hex, and hands the set to the engine
    attributed to this source (`RangeSource::hashSetReceived`). The engine trusts it only when it
    reduces to the trusted ed2k hash (eD2k rule: MD4 of the concatenated part hashes); without a
    trusted ed2k hash it is used provisionally and confirmed or refuted later. (5.3) A set never bans
    its sender: one that reduces to another value, or that is refuted later (by a trusted ed2k hash
    learned afterwards, or because parts of other sources do not match it), is dropped and that
    source's sets are not used again for the download, while its data keeps being used and is checked
    like anyone's. Parts that fail against a provisional set give no strikes until data of a source
    other than the set's sender has matched it; two such failures drop the set. A torrent download
    does not take eD2k sets at all (piece hashes judge it). 5.2 banned the sender in these cases.
  * Old peers: a v1 server without the extension sends no `hs`, so the client never asks; a v1
    client ignores the unknown `hs` field and never sends `hs_req`; an `hs_req` that reaches an old
    server is an unknown type and dropped. Nothing else changes.
* **Files that are still downloading** (5.3, `caps` `part`). A node serves a running download by
  the download's trusted identity, and only the byte ranges verified against trusted hashes (pieces
  libtorrent checked; units checked against a trusted eD2k set or TTH tree, never against a
  provisional set). A file a download is still writing is never offered as a complete file (5.2's
  library hashed a torrent's half-downloaded file in the shared incoming folder and shared it with
  hashes computed over its holes).
  * The client says it understands partial sources: `get_open {..., part:true}`,
    `src_q {..., part:true}`. A server answers `src_q` for a partial file, and opens a slot for it,
    only when `part` is set; a `get_open` without it (5.2 and older) gets `slot {st:"missing"}` for a
    partial file.
  * `slot` (`ready` / `queued`) of a partial file carries `have:[[start, end], ...]` (byte ranges,
    sorted, at most 4096). A slot without `have` is a complete file.
  * When the verified part grows, the server sends `have {sid, have:[...]}` (at most every 5 s, at
    once when a request asks for a range it holds by then), and `have {sid, full:true}` when the file
    is complete. A shrunk map (a provisional set the uploader itself dropped) is sent the same way.
  * `get` for a range outside the map is answered `got {sid, rq, ok:false, err:"not available"}`;
    the client removes the range from its map and reports `RangeSource::rangeNotAvailableError()`,
    which the engine does not count as a failure.
* `RangeSource::available()` reports the whole file (`{0,size}`) once the slot of a complete file is
  ready, the `have` map for a partial one.
* Compatibility of the 5.3 additions with 5.2 nodes: a 5.2 downloader never sends `part`, so a 5.3
  node does not offer it files it is still downloading (it gets complete files as before, and the row
  of such an upload ends with "Finished" when it closes). A 5.2 uploader never sends `have`: its files
  are complete (or, the 5.2 defect, a half-downloaded torrent file with wrong hashes; a 5.3 downloader
  then drops the set quietly, and only bad data costs the source strikes or a ban). Unknown fields
  and the unknown `have` message are ignored by 5.2 nodes.

## 8. Hash-link registry

* Lookup: `hl_q {qid, h, size}` on `mx/search`. A peer answers `hl {qid, h:<all>, size}` when its
  library, or its hash-link cache, has an entry that matches without conflict and adds at least
  one hash kind (and only while `overlay/shareHashLinks` is on). The asker ignores answers that
  `conflicts()` with the known hashes, that do not `matches()` them, or whose size differs. It
  emits `hashLinksFound(known, all, size)`. Answers are hints: the download engine still verifies
  the data.
* Publish: `hl_pub {h, size, n}` on `mx/hashlinks` for recently completed files
  (`publishHashLinks`). Every peer with `overlay/shareHashLinks` on stores sets that have at least
  2 hash kinds and size > 0 in a **bounded LRU (5 000 entries)**, persisted to
  `<dataDir>/hashlinks.json` (every 5 min and on stop), so it can answer for files it does not
  hold itself.

## 9. Link liveness (4.3)

A verified link that has been silent for 4 s gets a control `ping {ts}` (every node, 4.2 included,
answers `pong {ts}`); a link without any byte for 12 s is dropped. Without this a crashed peer
(power cut, kill) kept its UDP link, and its Online status, for librats' 45 s idle timeout. Losing the
link then follows the presence rules of section 4. After losing its last peer a node dials its
bootstrap peers again after 5 s (then every minute).

## 9a. Robustness limits

Each node may send at most 50 control messages per second, with a burst of 200. A peer whose
messages are dropped 2 000 times is disconnected. All JSON accessors are type-checked and
length-bounded: text is cut to its limit, arrays are capped, numbers are range-checked, hex
strings are validated per hash length. Zlib payloads are inflated only up to the length their
header declares, and that length is bounded. No message type can make the receiver allocate more
than 4 MiB.

## 10. Versioning

`v` is carried in every control message, envelope body and data frame (frame byte 1, envelope
byte 1). Version 1 peers ignore unknown `t` values and unknown fields. A future breaking change
would get a new protocol string (the number after the slash raised): the string is part of the Noise prologue and of
the DHT rendezvous key, so the two networks would stay apart, the way `rats-search/3` and
`rats-search/4` do.

## 11. Port probe ("Test my ports", diag-1)

The Connection Test asks up to 2 directly linked, hello-verified users that are not on a relayed
circuit to test whether our ports can be reached from outside:
```
probe  {t:"probe", v, rid:<token>, nonce:<random hex>, ports:[{p:"TCP"|"UDP", n:<port>, k:<kind>}...≤8]}
probed {t:"probed", v, rid, helper:<helper display name>, res:[{p, n, r:"yes"|"no"|"sent", e?}...], e?}
```
* The helper tests the address it sees our link come from (never an address named in the request,
  so it cannot be used to scan third parties): TCP = a connect within 6 s; UDP with `k:"dht"` = a
  mainline-DHT `ping` answered within 6 s; UDP with `k:"datagram"` = three datagrams
  `"WMXNG-PROBE " + nonce + " " + port` (answer `r:"sent"`), which the asker's own sockets recognise
  (`consumeProbeDatagram`); after 1 s the asker counts the port reachable if one arrived.
* Refusals come back as `probed` with `e` and no `res`: "this user does not test ports"
  (`diag/answerProbes` off), "asked too often, try again in a minute" (one probe per requesting user
  per 60 s), "busy, try again later" (at most 10 probes per 10 minutes in total), "connected through
  a relay, cannot reach your address", "no ports to test".
* The asker gives up after 16 s. Older nodes drop the unknown `probe` type; the test then says no
  user answered.

## 12. Compatibility with 4.2 and older nodes

A 4.3 node and a 4.2 node still connect, chat in rooms, search, browse, transfer and exchange
private messages in both directions; nothing in 4.3 is required of the other side. What degrades
when one side is 4.2 (tested in `tst_overlay_net` `olderNodeCompatibility` with a node speaking the
4.2 wire format):
* PMs to a 4.2 user show "Sent" (it sends no receipts) instead of "Delivered"; no retry when it is
  silently lost. PMs from a 4.2 user arrive as before and get no receipt (it would ignore one).
* Refusals: a 4.2 user still gets our direct `pm_err` and shows its generic "not accepting private
  messages"; a refusal of a relayed PM from a 4.2 user is not sent (4.2 drops relayed `pm_err`). A
  4.2 user refuses with `reason:"refused"` and we show the generic text.
* Presence: a 4.2 user times out after 330 s when only relayed (it announces every 120 s and sends no
  `pi`), and goes offline at once when its direct link drops even if still reachable through the mesh
  (it cannot answer a `ping`), until its next presence. A 4.2 node keeps showing a 4.3 user online
  for up to 5.5 minutes after a normal quit (it ignores a `bye` from a direct peer).
* Connect-back: a 4.2 user ignores `reach`; a firewalled 4.2 user is reachable for browse and
  transfers only by a relayed circuit (librats, present in 4.2 as well) or when it dials us.
* 4.3 relays PMs at once instead of after an 8 s dial; a 4.2 recipient decrypts them as before.
* Rooms: a 4.2 member's heartbeats carry no `node` / `x`; until its next presence (at most 120 s) it
  can be seen in the room but not browsed or messaged ("Cannot reach X yet: their program has not
  announced its network address ..."). A 4.2 node does not relay rooms it has not joined.
* A 4.2 node drops links only by librats' 45 s idle timeout, so it sees a crashed 4.3 user online
  longer; it answers our liveness pings, so we see a crashed 4.2 user offline within 12 s.

## 13. Test switches (instance tests only)

Read by the plugin at start, for running several real program instances on one host:
`MX_OVERLAY_DISCOVERY=0` (no DHT rendezvous, no mDNS, no peer exchange, no peer book: the instance
meets only its bootstrap peers, never real users) and `MX_OVERLAY_FIREWALLED=1` (behave like a node
behind a NAT without a forwarded port: direct inbound links are refused, no address is announced, no
hole punch; connect-back and relayed circuits still work). `tests/overlay/net_instances.sh` uses both.

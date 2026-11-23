// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX: BitTorrent network plugin (libtorrent-rasterbar 2.0). Owner: bt task.
// The only public header of target mxnet_bt.
//
//   mx::NetworkPlugin *p = mx::createBitTorrentPlugin(parent);
//   auto *backend = dynamic_cast<mx::TorrentBackend *>(p);      // always non-null
//   auto *control = dynamic_cast<mx::BitTorrentControl *>(p);   // always non-null
//   auto *events  = qobject_cast<mx::BitTorrentEvents *>(backend->torrentEvents());
//
// Threading: like every plugin, the object lives on the main thread and all methods must be called
// there; every signal is emitted there. One lt::session lives in a private worker thread that owns
// every blocking libtorrent call (status, peer lists, resume data, alerts). Main-thread getters
// (snapshot(), torrents(), pieceHash(), status()) read a cache refreshed at most once per second.
//
// Lifecycle: init(ctx) creates the session OFFLINE (paused, no listen socket, no DHT) and loads
// every persisted torrent from ctx.dataDir (<profile>/bt/), so torrents()/snapshot() work before
// start(). start() applies the configured network settings and resumes the session. stop() saves
// resume data of every torrent + session state (DHT nodes), waiting up to ~3 s for libtorrent,
// and takes the session offline again (torrents keep their own paused/running flags; nothing is
// lost). The destructor stops if needed and tears the session down (bounded by a 1 s tracker timeout).
//
// ---------------------------------------------------------------------------------------------
// TorrentBackend semantics (verified by tests/bt against libtorrent 2.0.15):
//
// * ids are the v1 info-hash in 40 lower-case hex chars; for v2-only torrents the truncated v2
//   hash (what libtorrent uses on the wire): such torrents have no v1 piece hashes.
// * addTorrent(uri, bytes, ...):
//     - uri "magnet:?..." → id returned immediately (metadata fetched from the swarm later →
//       torrentMetadata). bt/extraTrackers are appended to magnets.
//     - torrentFile non-empty (raw .torrent bytes) → parsed synchronously (bad data → "" + *error).
//     - uri is a local path / file:// URL to a .torrent → read and added like raw bytes.
//     - uri "http(s)://..." → the .torrent is downloaded asynchronously: returns "" with *error
//       left EMPTY (pending). Later BitTorrentEvents::urlTorrentAdded(url, id) + torrentAdded(id),
//       or urlTorrentFailed(url, error) + torrentError(url, error).
//     - adding a torrent that already exists returns its id (trackers of the new magnet/.torrent
//       are merged in, nothing else changes; torrentAdded is not emitted again).
//     - wantedFiles non-empty → only those file indexes are downloaded (also for magnets: applied
//       the moment metadata arrives, unwanted files are never requested).
// * File indexes are libtorrent's file indexes. Torrents created by other clients may contain
//   BEP 47 pad files: they appear in TorrentSnapshot::files with a path under ".pad/" and
//   priority 0 (the UI should hide them). Torrents we create (seedLocalFile) are v1, no pads.
// * TorrentSnapshot::totalSize/totalDone = sum over wanted files (piece granularity), so
//   reserved pieces are still counted as wanted. `finished` is true only when every wanted
//   piece is verified: reserved-but-missing pieces keep a torrent unfinished (see below).
// * setTorrentPriority(p): 1 = move to the top of libtorrent's queue, -1 = bottom, 0 = unchanged
//   position. Persisted.
// * pieceHash(id, piece): 20-byte v1 SHA-1 of the piece, from a main-thread copy of the piece
//   hashes. Empty when the metadata is not known yet, the index is out of range, or the torrent
//   is v2-only (v2 has only per-file SHA-256 merkle trees: no per-piece SHA-1 exists).
//
// * reservePieces(id, pieces, true): the pieces get libtorrent piece priority 0 (dont_download).
//   VERIFIED: the picker never issues new requests for them to BitTorrent peers while reserved
//   (test "mixed": the seeding peer uploads exactly the unreserved half). Requests already in
//   flight when you reserve are not cancelled; if such a piece still completes from a peer you
//   get torrentPieceFinished and a later inject is a no-op. Reservations are NOT persisted:
//   after a restart (or a stop/start of the plugin process) every piece is back to normal.
//   Changing wanted files keeps the reservations in force (they are re-applied after
//   prioritize_files, which would otherwise overwrite piece priorities).
//   While only reserved pieces are missing libtorrent considers the torrent "finished" and
//   drops its connections to seeds (close_redundant_connections); the plugin does NOT report
//   finished then (snapshot().finished == false, statusText "...waiting for N pieces from other
//   networks", totalSize still includes the reserved pieces).
// * reservePieces(id, pieces, false): the piece priority goes back to the value derived from
//   the file priorities (0 for pieces only in unwanted files) and the normal picker takes them
//   again. VERIFIED: a torrent that was "finished except reserved pieces" goes back to
//   downloading, reconnects to its peers and completes from BitTorrent. This needs a workaround
//   for a libtorrent 2.0.15 quirk: after leaving the finished state its peer list never
//   re-counts connect candidates when all known peers are seeds, so it would never reconnect.
//   The plugin then adds one unroutable RFC 5737 peer (192.0.2.x) to force the recount, and
//   re-announces (see Worker::wakeTorrent). Side effect of libtorrent itself: the torrent moves to
//   the end of the download queue (torrent::resume_download).
//   Reserving pieces of a torrent that is not yet in the session (just added) is queued.
// * injectPiece(id, piece, data): data must be the whole piece (piece size; the last piece is
//   shorter). Steps, all off the GUI thread:
//     1. already have the piece → no write; torrentPieceFinished(id, piece) is emitted again so the
//        caller can settle its bookkeeping.
//     2. v1 SHA-1 pre-check against the piece hash (skipped for v2-only torrents). Mismatch →
//        torrentPieceFailed, nothing is written to disk.
//     3. torrent is checking files → the piece is held and injected when checking ends.
//     4. lt::torrent_handle::add_piece(piece, data) WITHOUT overwrite_existing: blocks peers already
//        delivered (or are writing) are kept, the other blocks are written, outstanding peer
//        requests for those blocks are cancelled/ignored as redundant, then libtorrent hash-checks
//        the whole piece → torrentPieceFinished (counts in progress, have-bit set, announced to
//        peers) or, if a peer's block in it was bad, libtorrent resets the piece and the plugin
//        re-injects the held data (up to 2 times) before reporting torrentPieceFailed.
//     A reserved piece that verifies is automatically un-reserved (priority restored).
//     Injection works for paused torrents and torrents without peers.
// * torrentPieceFinished / torrentPieceFailed are emitted for every piece, whether it came from
//   peers or from injectPiece (batched per alert pump, in order). Pieces restored from resume data
//   at startup are NOT re-announced (libtorrent posts piece_finished for them; the plugin drops
//   those); a recheck() re-reports every piece that passes.
// * torrentFinished fires every time the torrent becomes complete for its wanted files (again after
//   setWantedFiles() adds incomplete files); snapshot(id) is already up to date when it fires.
// * Commands issued right after addTorrent() (pause, resume, reserve, addPeer, ...) are queued until
//   libtorrent has added the torrent, so they are never lost.
// * Persistence (<dataDir> = ctx.dataDir, normally <profile>/bt): session.dat (DHT state, every
//   5 min + at stop), nodes.json (user-added DHT nodes / trackers), torrents/<id>.json (our record:
//   magnet, save path, priority, seeded path), <id>.fastresume (libtorrent resume data incl. the
//   info dict; written at add, on structural changes within ~1 s, every 30 s when progress changed,
//   and at stop), <id>.torrent (exported metadata). After a restart every torrent comes back
//   without a recheck in the same paused/running state, priorities and save path; reservations are
//   cleared. kill -9 loses at most ~30 s of progress (tested: ≈7 s at 600 KiB/s).
// * Uploads: one TransferInfo per (torrent, peer) that received data from us in the last 15 s,
//   id "bt:<infohash>:<ip:port>", sampled once per second. cancelUpload(id) blocks that peer's IP
//   for the rest of the session (libtorrent has no public per-peer disconnect) → uploadFinished.
// * seedLocalFile(path): returns false (+ *error) if the file is missing/unreadable/empty. Else
//   hashes it in a background thread (progress: BitTorrentEvents::seedProgress), writes
//   <dataDir>/torrents/<id>.torrent, adds it in seed mode (save path = the file's directory)
//   and forces a DHT announce → torrentAdded(id) + BitTorrentEvents::seedReady(path, id).
//   Idempotent: calling it again for a path that is seeded (or being hashed) returns true and
//   re-emits seedReady (no second hashing).
// * Rates/limits: once per second the plugin reads ctx.limiter->budgetBps(Down/Up, Net::BitTorrent)
//   and applies it as libtorrent's session rate limit (0 = unlimited), and accounts the payload
//   bytes moved since the last second with ctx.limiter->account(dir, Net::BitTorrent, bytes).
//   LAN peers are exempt unless bandwidth/limitLan. BitTorrentControl::setRateLimits overrides it.
// * NetworkStatus: Disconnected before start()/after stop(); Connecting until a listen socket is up
//   and (with DHT on) the DHT has nodes, or any peer is connected; peers = DHT node count;
//   firewalled until an incoming connection has been accepted; rates include protocol overhead.
// ---------------------------------------------------------------------------------------------
#pragma once

#include "mx/plugin.h"

namespace mx {

// Extra control surface of the BitTorrent plugin (reach it with dynamic_cast from the plugin).
class BitTorrentControl {
public:
    virtual ~BitTorrentControl() = default;
    // Global libtorrent rate limits in BYTES per second (0 = unlimited). Optional: normally the
    // plugin polls ctx.limiter->budgetBps() once per second itself (ARCHITECTURE "Bandwidth").
    // Once this has been called, the explicit values win and polling stops (tools, tests, or a core
    // that prefers to push). Applied only when they change. Without a limiter: unlimited.
    virtual void setRateLimits(int downBps, int upBps) = 0;
    virtual int downloadRateLimit() const = 0;   // currently applied, bytes/s, 0 = unlimited
    virtual int uploadRateLimit() const = 0;
    // Connect a torrent to a peer explicitly ("ip:port" or "[v6]:port"). Used by tests and for
    // magnet x.pe hints; no-op if the torrent is unknown.
    virtual void addPeer(const QString &id, const QString &endpoint) = 0;
};

// The concrete type of TorrentBackend::torrentEvents() (qobject_cast-able). Adds a few signals on
// top of mx::TorrentEvents.
class BitTorrentEvents : public TorrentEvents {
    Q_OBJECT
public:
    using TorrentEvents::TorrentEvents;
signals:
    void urlTorrentAdded(const QString &url, const QString &id);          // http(s) .torrent fetched + added
    void urlTorrentFailed(const QString &url, const QString &error);
    void seedProgress(const QString &path, qint64 bytesHashed, qint64 totalBytes);  // ≤ 4/s
    void seedReady(const QString &path, const QString &id);
    void seedFailed(const QString &path, const QString &error);
};

// Factory. The returned object is-a NetworkPlugin, TorrentBackend and BitTorrentControl.
NetworkPlugin *createBitTorrentPlugin(QObject *parent = nullptr);

} // namespace mx

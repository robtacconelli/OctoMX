// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX engine: TorrentStore: libtorrent (via mx::TorrentBackend) owns data + verification;
// this class mirrors the backend's snapshot and feeds it pieces fetched from other networks.
//
// Foreign injection (see bt_plugin.h for the backend's exact semantics):
//   plan() reserves a batch of missing pieces that the foreign sources can deliver (pieces inside
//   a file that has foreign sources; boundary pieces only if every file they touch is coverable,
//   pad files count as coverable, they are zeros), capped so libtorrent is never starved
//   (≤ maxReservedShare of the missing pieces while libtorrent has peers, and ≤ what the foreign
//   sources deliver in reserveHorizonSec at their measured rate, and ≤ the memory budget).
//   The Download's range scheduler fetches fetchTarget() (the unfilled bytes of reserved pieces);
//   deliver() assembles whole pieces in memory, a worker pre-verifies each against pieceHash()
//   (so blame lands on the contributing sources) and injectPiece() hands it to libtorrent.
//   torrentPieceFinished → unitVerified; a piece a BT peer finished first → unitAbandoned;
//   reservations without progress time out and are returned to libtorrent.
#pragma once

#include <QtCore/QBitArray>
#include <QtCore/QMap>
#include <QtCore/QSet>

#include "core/engine/engine_types.h"
#include "core/engine/io_queue.h"
#include "core/engine/store.h"
#include "mx/plugin.h"

namespace mx::engine {

class TorrentStore : public Store {
    Q_OBJECT
public:
    struct Lane {                 // a file of the torrent that foreign sources can deliver
        int    file = -1;
        int    readySources = 0;
        qint64 rateBps = 0;       // measured aggregate (0 = unknown yet)
    };

    TorrentStore(TorrentBackend *backend, const QString &id, QThreadPool *pool, const Tuning *tuning,
                 QObject *parent = nullptr);
    ~TorrentStore() override;

    QString id() const { return m_id; }
    const TorrentSnapshot &snap() const { return m_snap; }
    void refresh();                                   // pull the backend's snapshot
    bool hasMetadata() const { return m_snap.hasMetadata && m_snap.pieceLength > 0; }
    qint64 payloadSize() const { return m_payload; }
    Range fileSpan(int file) const;                   // store range of file i (empty if unknown)
    bool isPadFile(int file) const;

    void plan(const QList<Lane> &lanes, qint64 nowMs);
    void releaseAll();                                // give every reservation back
    void onPieceFinished(int piece);
    void onPieceFailed(int piece);
    int reservedCount() const { return int(m_res.size()); }
    int injectedCount() const { return m_injectedCount; }
    qint64 injectedBytes() const { return m_injectedBytes; }
    QList<int> reservedPieces() const { return m_res.keys(); }
    int pieceFailures() const { return m_failTotal; }
    // [dl-1] the order in which plan() reserves pieces for other networks: first and last = the head
    // and tail band of each lane's file first; sequential = ascending (else from the end backwards)
    void setOrder(bool sequential, bool firstLast) { m_seq = sequential; m_firstLast = firstLast; }

    // Store
    Kind kind() const override { return Torrent; }
    qint64 extent() const override { return hasMetadata() ? m_payload : -1; }
    RangeSet fetchTarget() const override;
    Range unitAt(qint64 pos) const override;
    bool canVerifyUnit(qint64 pos) const override;
    RangeSet deliver(qint64 off, const QByteArray &bytes) override;
    qint64 bufferedBytes() const override { return m_buffered; }
    bool acceptingData() const override { return hasMetadata(); }
    qint64 verifiedBytes() const override { return m_snap.totalDone; }
    qint64 heldBytes() const override { return m_snap.totalDone; }
    void pieceMap(int cells, const RangeSet &inFlight, QByteArray *state, QByteArray *fill) const override;
    // [ui-i] the map of a snapshot: wanted, non-pad files laid end to end; complete = the pieces
    // libtorrent has, in progress = the pieces in its download queue with a block requested / being
    // written (TorrentSnapshot::downloading) and `moving` (pieces fetched from other networks now)
    static void mapOf(const TorrentSnapshot &sn, const QSet<int> &moving, int cells, QByteArray *state, QByteArray *fill);

private:
    struct Piece {
        qint64 start = 0, end = 0;
        QByteArray buf;
        RangeSet filled;
        qint64 reservedMs = 0, lastMs = 0;
        bool verifying = false, injected = false;
    };
    Range pieceRange(int p) const;
    bool pieceWanted(int p) const;
    void release(int p, bool emitAbandon);
    void startVerify(int p);
    void fillPads(Piece &pc);

    TorrentBackend *m_be;
    QString m_id;
    IoQueue m_io;
    const Tuning *m_tuning;
    TorrentSnapshot m_snap;
    qint64 m_payload = 0;
    QMap<int, Piece> m_res;
    QHash<int, int> m_failures;
    QSet<int> m_giveUp;
    QHash<int, qint64> m_cooldown;
    qint64 m_lastProgressMs = 0;
    QBitArray m_wanted;                  // per piece: belongs to a wanted, non-pad file         // last foreign byte assembled       // timed-out pieces stay with libtorrent until then
    qint64 m_buffered = 0;
    int m_injectedCount = 0;
    qint64 m_injectedBytes = 0;
    int m_failTotal = 0;
    bool m_seq = false, m_firstLast = false;   // [dl-1]
};

} // namespace mx::engine

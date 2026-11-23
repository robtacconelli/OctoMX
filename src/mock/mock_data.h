// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX mock core: deterministic demo data (see mock_core.h).
#pragma once

#include <QtCore/QList>
#include <QtCore/QRandomGenerator>

#include "mx/types.h"

namespace mx::mock {

// Search results for a query. "velorine" gives the list of reference/ref2 (164 results, grouped
// multi-source rows); "linux" gives distribution ISOs over BitTorrent/eD2k/HTTP; anything else
// gives a plausible generated mix over all networks. `batch` is filled with indexes telling in
// which delivery batch (0..3) each hit arrives (for the progressive non-frozen mode).
QList<SearchHit> searchHits(const SearchQuery &q, QList<int> *batch = nullptr);

QList<TransferInfo> downloads();
QList<TransferInfo> uploads();

// [ui-i] A believable map of a demo download (TransferInfo::pieceMap / pieceFill): the file in pieces,
// a few of them arriving at a time in the given order; done() = the bytes of finished pieces.
class PieceSim;
PieceSim demoPieces(const TransferInfo &t);   // the map of demo download t (invalid: none)
class PieceSim {
public:
    enum Order { Scattered = 0, Sequential, FirstLast };
    PieceSim() = default;
    // done: verified bytes to start with (rounded to whole pieces); active: pieces arriving at once;
    // piece: piece length (0 = about a thousand pieces, as a torrent would choose)
    PieceSim(qint64 size, qint64 done, Order order, int active, quint32 seed, qint64 piece = 0);
    bool isValid() const { return m_size > 0 && !m_got.isEmpty(); }
    void advance(qint64 bytes);           // spread over the arriving pieces
    void setRunning(bool running);        // not running: nothing is arriving (paused, queued)
    qint64 done() const;
    void apply(TransferInfo &t) const;    // done, pieceMap, pieceFill
private:
    qint64 pieceLen(int i) const;
    int pickNext();
    qint64 m_size = 0;
    qint64 m_piece = 0;
    QVector<qint64> m_got;                // bytes received per piece (a piece is verified when full)
    QList<int> m_active;
    Order m_order = Scattered;
    int m_wantActive = 0;
    quint32 m_seed = 1;
};

QList<NetworkStatus> networks();
QList<ServerEntry> servers();

QString helpRoomName();   // "OctoMX Help_49717657BABE" (backlog like web/chat_room_window.png)
QString bigRoomName();    // a 1500-user room (responsiveness)
QList<ChatRoomInfo> rooms();
QList<ChatUser> roomUsers(const QString &roomId);
QList<ChatMessage> roomBacklog(const ChatRoomInfo &room);
QStringList chatLines();   // random lines for live demo chatter

QList<HotlistEntry> hotlist();
QList<UserRef> ignored();

QString sharedRoot();     // "C:\\MX Shared" / "/home/demo/MX Shared" (does not exist on disk)
QString downloadsRoot();
QList<SharedFolder> sharedFolders();
QList<SharedFile> sharedFiles();
QList<SharedFile> demoFilesFor(const QString &root);   // the demo content of one of the two roots above

QList<BrowseEntry> browseFiles(const UserRef &u);
QVariantMap whois(const UserRef &u);

// One bandwidth sample for second `t` (deterministic curve: slow waves + bursts).
BandwidthSample bandwidthAt(qint64 t, qint64 timeMs);
// Bandwidth model of the demo: protocol overhead per network at second t, and the shape of the
// file-transfer payload in the history (0 = idle, 1 = today's rate).
struct NetRate {
    Net net;
    int down;
    int up;
};
QList<NetRate> overheadAt(qint64 t);
double payloadShape(qint64 secondsAgo, qint64 t);

UserRef user(Net n, const QString &name);

} // namespace mx::mock

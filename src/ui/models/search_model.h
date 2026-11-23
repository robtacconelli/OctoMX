// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: the search-results model (UI_SPEC §3.4, §2.2, §8.4).
//
// Two levels: one row per merged hit (mx::SearchHit, identified by its key); when a hit has more
// than one source the sources are its children ("N users" group row, [+] expander). Updates are
// applied in place (dataChanged / child rows inserted or removed), never by resetting, so
// selection, expansion and scroll position survive live updates.
//
// SearchProxy sorts (ItemRole::SortRole, "Available Files On Top") and filters by connection class.
#pragma once

#include <QtCore/QAbstractItemModel>
#include <QtCore/QHash>
#include <QtCore/QSortFilterProxyModel>
#include <QtGui/QColor>

#include "mx/types.h"
#include "ui/models/roles.h"

namespace mx::ui {

namespace search {
// [ui-e] File | Size | Time | Bitrate | Freq | Network | Peers | Status are shown by default; the classic
// User / Speed / Ping columns are optional (header right-click ▸ column chooser), hidden at first.
enum Column { File = 0, Size, Time, Bitrate, Freq, Network, Peers, Status, User, Speed, Ping, ColumnCount };
constexpr Column Bytes = Size;   // the classic name of the size column

// "BT", "eD2k", "Kad", "Gnutella", "HTTP", "OctoMX", "Web", "Rats"
QString netLabel(mx::Net n);
// the compact network list of a hit: "BT", "eD2k+Kad", "BT+Gnutella", "OctoMX"; a hit only known from
// torrent index sites: "LimeTorrents (BT)" / "LimeTorrents+2 (BT)"
QString networkText(const mx::SearchHit &h);
// every peer known for the hit across networks (SearchHit::totalPeers; computed from the sources
// when the core did not fill it); -1 = unknown
int totalPeers(const mx::SearchHit &h);

// connection class rank used by the Speed colours and the connection filter:
//   [qa-1] mx::connectionSpeedRank(): 0 Unknown, 1 DSL, 2 4G, 3 VDSL, 4 Satellite, 5 FTTC, 6 Cable, 7 5G,
//   8 FTTB, 9 FTTH, 10 2.5 Gbit, 11 5 Gbit, 12 10 Gbit+; -1 = not a line type
int connectionRank(const QString &connection);
constexpr int kFastRank = 3;        // VDSL and faster count as "fast" (green, the first number of "2 + 1")
constexpr int kBroadbandRank = 6;   // rank used for sources without a line type (HTTP mirrors, peers)
int sourceRank(const mx::SearchSource &s);          // torrents: by seeds (UI_SPEC §8.4)
bool isFast(const mx::SearchSource &s);             // VDSL or faster (green)
QColor speedColor(int rank);                         // green / yellow / red
QString connectionText(const QString &connection);  // [qa-1] display form: printable, max 16 chars, "Unknown"
QColor bitrateColor(int kbps);
QColor slotsColor(int freeSlots);                    // >=2 green, 1 orange, 0 white
bool isAvailable(const mx::SearchSource &s);
} // namespace search

class SearchModel : public QAbstractItemModel {
    Q_OBJECT
public:
    explicit SearchModel(QObject *parent = nullptr);

    QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation o, int role) const override;
    bool hasChildren(const QModelIndex &parent = QModelIndex()) const override;

    // Merge new / updated hits (by key). Returns the number of new top-level rows.
    int applyHits(const QList<mx::SearchHit> &hits);
    // Remove rows whose key is in `keys` (SearchService::hitsRemoved). Returns rows removed.
    int removeHits(const QStringList &keys);
    void clear();

    int hitCount() const { return int(m_hits.size()); }
    const mx::SearchHit &hitAt(int row) const { return m_hits.at(row); }
    int rowOfKey(const QString &key) const { return m_rowOfKey.value(key, -1); }
    bool isSourceRow(const QModelIndex &i) const { return i.isValid() && i.internalId() != 0; }
    // The hit for a row; for a source row: a copy of the hit with only that source (what a
    // double-click on a child downloads).
    mx::SearchHit hitFor(const QModelIndex &i) const;
    mx::SearchSource sourceFor(const QModelIndex &i) const;   // single-source rows: that source

    void setShowFullPaths(bool on);
    bool showFullPaths() const { return m_fullPaths; }
    // search/flagSuspects: suspected fakes (SearchHit::suspect) in the suspect colour + reason tooltip
    void setFlagSuspects(bool on);
    bool flagSuspects() const { return m_flagSuspects; }
    QString suspectReason(const mx::SearchHit &h) const;   // empty when not flagged / flagging off

private:
    QVariant dataFor(const QModelIndex &i, int role) const;
    QVariant hitData(const mx::SearchHit &h, int col, int role) const;
    QVariant sourceData(const mx::SearchHit &h, const mx::SearchSource &s, int col, int role, bool child) const;
    QColor overrideColor(const mx::SearchHit &h) const;
    static int childCount(const mx::SearchHit &h) { return h.sources.size() > 1 ? int(h.sources.size()) : 0; }

    QList<mx::SearchHit> m_hits;
    QHash<QString, int> m_rowOfKey;
    bool m_fullPaths = false;
    bool m_flagSuspects = true;
};

class SearchProxy : public QSortFilterProxyModel {
    Q_OBJECT
public:
    explicit SearchProxy(QObject *parent = nullptr);
    void setAvailableOnTop(bool on);
    bool availableOnTop() const { return m_availTop; }
    void setMinConnectionRank(int rank);      // 0 = any connection
    int minConnectionRank() const { return m_minRank; }
    void setHideSuspects(bool on);            // search/hideSuspects (the core hides them too)
    bool hideSuspects() const { return m_hideSuspects; }
    // [ui-f] the file-type part of the search (type, extension(s), bitrate), applied to the merged
    // rows with mx::hitMatchesFileType() like the core does to each answer: a bundle whose file list
    // arrives later and contradicts the type disappears; with extensions, a source row listed under
    // another name (SearchSource::extra "fileName") must have one of them too.
    void setTypeFilter(const mx::SearchQuery &q);
    bool hasTypeFilter() const { return m_typeOn; }
    bool typeAccepts(const mx::SearchHit &h) const;

protected:
    bool lessThan(const QModelIndex &a, const QModelIndex &b) const override;
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override;

private:
    bool m_availTop = false;
    int m_minRank = 0;
    bool m_hideSuspects = false;
    bool m_typeOn = false;
    mx::SearchQuery m_type;
    QStringList m_typeExts;
};

} // namespace mx::ui

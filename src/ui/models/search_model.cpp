// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/models/search_model.h"

#include <algorithm>

#include <QtCore/QSet>

#include "mx/settings_keys.h"
#include "ui/kit/colors.h"
#include "ui/kit/format.h"

namespace mx::ui {

// ------------------------------------------------------------------------------------ helpers

namespace search {

// [qa-1] owner request: modern connection types; the rank is mx::connectionSpeedRank() (0 Unknown,
// 1 DSL, 2 4G, 3 VDSL ... 12 "10 Gbit+"; original WinMX labels are mapped)
int connectionRank(const QString &c)
{
    if (c.trimmed().isEmpty())
        return 0;
    if (!isConnectionType(c))
        return -1;   // not a line type (e.g. "DHT", a client name): the caller decides
    return connectionSpeedRank(c);
}

int sourceRank(const SearchSource &s)
{
    const int r = connectionRank(s.connection);
    if (r >= 0 && s.net != Net::BitTorrent)
        return r;
    if (s.seeds >= 0) {
        // swarm health on the same scale (UI_SPEC 8.4: many seeds = a very fast source)
        const int seeds = s.seeds;
        if (seeds >= 50) return 12;
        if (seeds >= 20) return 9;
        if (seeds >= 10) return 6;
        if (seeds >= 5) return 3;
        if (seeds >= 2) return 2;
        if (seeds >= 1) return 1;
        return 0;
    }
    if (r >= 0)
        return r;
    // no line type: HTTP mirrors / BitTorrent peers count as broadband; a label another user sent that
    // we do not know ranks as Unknown (owner request)
    return s.net == Net::BitTorrent || s.net == Net::Http ? kBroadbandRank : 0;
}

QString connectionText(const QString &c)
{
    QString out;
    for (const QChar ch : c)
        if (ch.isPrint())
            out += ch;
    out = out.simplified();
    if (out.isEmpty())
        return QObject::tr("Unknown");
    return out.size() > 16 ? out.left(15) + QStringLiteral("...") : out;
}

bool isFast(const SearchSource &s)
{
    return sourceRank(s) >= kFastRank;
}

// Unknown red, DSL / 4G yellow (the old middle tier), everything faster green
QColor speedColor(int rank)
{
    if (rank >= kFastRank)
        return color(keys::colors::SpeedFast);
    if (rank >= 1)
        return color(keys::colors::SpeedMedium);
    return color(keys::colors::SpeedSlow);
}

QColor bitrateColor(int kbps)
{
    if (kbps >= 160)
        return color(keys::colors::BitrateHigh);
    if (kbps >= 112)
        return color(keys::colors::BitrateMedium);
    return color(keys::colors::BitrateLow);
}

QColor slotsColor(int freeSlots)
{
    if (freeSlots >= 2)
        return color(keys::colors::Available);
    if (freeSlots == 1)
        return color(ckeys::OneSlotAvailable);
    return color(keys::colors::Unavailable);
}

bool isAvailable(const SearchSource &s)
{
    if (s.net == Net::BitTorrent || (s.freeSlots < 0 && s.seeds >= 0))
        return s.seeds > 0;
    return s.freeSlots > 0 || s.freeSlots < 0;
}

QString netLabel(Net n)
{
    switch (n) {
    case Net::BitTorrent: return QStringLiteral("BT");
    case Net::ED2K: return QStringLiteral("eD2k");
    case Net::Kad: return QStringLiteral("Kad");
    case Net::Gnutella: return QStringLiteral("Gnutella");
    case Net::Http: return QStringLiteral("HTTP");
    case Net::Overlay: return QStringLiteral("OctoMX");
    case Net::Web: return QStringLiteral("Web");
    case Net::Rats: return QStringLiteral("Rats");
    case Net::Unknown: break;
    }
    return QString();
}

static QString providerOf(const SearchSource &s)
{
    return s.extra.value(QStringLiteral("providerName")).toString();
}

QString networkText(const SearchHit &h)
{
    // networks that answered themselves (a torrent index site is not a network: its BitTorrent
    // entries count as BT only next to other networks)
    static const Net order[] = {Net::Overlay, Net::BitTorrent, Net::ED2K, Net::Kad, Net::Gnutella,
                                Net::Http, Net::Rats, Net::Web};
    QSet<int> live;
    QStringList indexes = h.indexes;
    bool indexBt = false;
    for (const SearchSource &s : h.sources) {
        const QString idx = providerOf(s);
        if (!idx.isEmpty()) {
            if (!indexes.contains(idx))
                indexes << idx;
            if (s.net == Net::BitTorrent || s.net == Net::Web)
                indexBt = true;
            else
                live.insert(int(s.net));   // a direct HTTP link an index listed
            continue;
        }
        if (s.net != Net::Unknown)
            live.insert(int(s.net));
    }
    if (live.isEmpty()) {
        for (Net n : h.networks)
            if (n != Net::Web && n != Net::Unknown)
                live.insert(int(n));
        if (indexBt && !indexes.isEmpty())
            live.remove(int(Net::BitTorrent));
    }
    if (live.isEmpty() && !indexes.isEmpty()) {
        QString t = indexes.first();
        if (indexes.size() > 1)
            t += QStringLiteral("+%1").arg(indexes.size() - 1);
        return t + QStringLiteral(" (BT)");
    }
    if (indexBt)
        live.insert(int(Net::BitTorrent));
    QStringList parts;
    for (Net n : order)
        if (live.contains(int(n)))
            parts << netLabel(n);
    return parts.join(QLatin1Char('+'));
}

int totalPeers(const SearchHit &h)
{
    if (h.totalPeers >= 0)
        return h.totalPeers;
    // the core's rule (SearchManager::fillHitNumbers) for cores / demo data that do not fill it: one
    // BitTorrent swarm (the best figures of every tracker / index), plus what the other networks name
    int btSeeds = -1, btPeers = -1, others = 0;
    bool any = false;
    QSet<QString> users;
    for (const SearchSource &s : h.sources) {
        if (s.net == Net::BitTorrent) {
            btSeeds = qMax(btSeeds, s.seeds);
            btPeers = qMax(btPeers, s.peers);
            continue;
        }
        const QString u = s.user.isValid() ? s.user.key() : s.user.address;
        if (!u.isEmpty()) {
            if (!users.contains(u)) {
                users.insert(u);
                ++others;
            }
            any = true;
        } else if (qMax(s.seeds, s.peers) > 0) {
            others += qMax(s.seeds, s.peers);
            any = true;
        }
    }
    if (btSeeds < 0 && btPeers < 0 && !any)
        return -1;
    return qMax(0, btSeeds) + qMax(0, btPeers) + others;
}

} // namespace search

using namespace search;

namespace {
int queueOf(const SearchSource &s)
{
    return s.extra.value(QStringLiteral("queue"), 0).toInt();
}

bool allTorrent(const SearchHit &h)
{
    for (const SearchSource &s : h.sources)
        if (s.net != Net::BitTorrent)
            return false;
    return !h.sources.isEmpty();
}

int seedsOf(const SearchHit &h, int *leechers)
{
    int seeds = 0, peers = 0;
    for (const SearchSource &s : h.sources) {
        seeds = qMax(seeds, s.seeds);      // trackers / DHT report the same swarm: take the max
        peers = qMax(peers, s.peers);
    }
    if (leechers)
        *leechers = peers;
    return seeds;
}

QString userLabel(const SearchSource &s)
{
    if (s.user.isValid() || !s.user.name.isEmpty())
        return s.user.name.isEmpty() ? s.user.id : s.user.name;
    if (!s.connection.isEmpty())
        return s.connection;
    return netName(s.net);
}

QString serverLabel(const SearchSource &s)
{
    return s.extra.value(QStringLiteral("server")).toString();
}
} // namespace

// --------------------------------------------------------------------------------- SearchModel

SearchModel::SearchModel(QObject *parent) : QAbstractItemModel(parent)
{
    connect(Colors::instance(), &Colors::changed, this, [this] {
        if (!m_hits.isEmpty())
            emit dataChanged(index(0, 0), index(int(m_hits.size()) - 1, ColumnCount - 1));
    });
}

// internalId: 0 = hit row; r + 1 = source child of hit row r
QModelIndex SearchModel::index(int row, int column, const QModelIndex &parent) const
{
    if (row < 0 || column < 0 || column >= ColumnCount)
        return {};
    if (!parent.isValid())
        return row < m_hits.size() ? createIndex(row, column, quintptr(0)) : QModelIndex();
    if (parent.internalId() != 0 || parent.row() >= m_hits.size())
        return {};
    if (row >= childCount(m_hits.at(parent.row())))
        return {};
    return createIndex(row, column, quintptr(parent.row() + 1));
}

QModelIndex SearchModel::parent(const QModelIndex &child) const
{
    if (!child.isValid() || child.internalId() == 0)
        return {};
    return createIndex(int(child.internalId()) - 1, 0, quintptr(0));
}

int SearchModel::rowCount(const QModelIndex &parent) const
{
    if (!parent.isValid())
        return int(m_hits.size());
    if (parent.internalId() != 0 || parent.column() != 0 || parent.row() >= m_hits.size())
        return 0;
    return childCount(m_hits.at(parent.row()));
}

bool SearchModel::hasChildren(const QModelIndex &parent) const
{
    return rowCount(parent) > 0;
}

int SearchModel::columnCount(const QModelIndex &) const
{
    return ColumnCount;
}

QVariant SearchModel::headerData(int section, Qt::Orientation o, int role) const
{
    if (o != Qt::Horizontal)
        return {};
    if (role == Qt::DisplayRole) {
        switch (section) {
        case File: return tr("File");
        case Size: return fmt::exactSizes() ? tr("Bytes") : tr("Size");
        case Time: return tr("Time");
        case Bitrate: return tr("Bitrate");
        case Freq: return tr("Freq");
        case Network: return tr("Network");
        case Peers: return tr("Peers");
        case Status: return tr("Status");
        case User: return tr("User");
        case Speed: return tr("Speed");
        case Ping: return tr("Ping");
        default: return {};
        }
    }
    if (role == Qt::TextAlignmentRole) {
        switch (section) {
        case Size: case Time: case Bitrate: case Freq: case Ping: case Peers:
            return int(Qt::AlignRight | Qt::AlignVCenter);
        default:
            return int(Qt::AlignLeft | Qt::AlignVCenter);
        }
    }
    return {};
}

int SearchModel::applyHits(const QList<SearchHit> &hits)
{
    QList<SearchHit> fresh;
    for (const SearchHit &h : hits) {
        const QString key = h.key.isEmpty() ? h.fileName + QString::number(h.size) : h.key;
        const int row = m_rowOfKey.value(key, -1);
        if (row < 0) {
            // also merge duplicates inside one batch
            bool merged = false;
            for (SearchHit &f : fresh)
                if ((f.key.isEmpty() ? f.fileName + QString::number(f.size) : f.key) == key) {
                    f = h;
                    merged = true;
                    break;
                }
            if (!merged)
                fresh << h;
            continue;
        }
        const QModelIndex pi = index(row, 0);
        const int oldKids = childCount(m_hits.at(row));
        const int newKids = childCount(h);
        if (newKids > oldKids) {
            if (oldKids == 0) {
                beginInsertRows(pi, 0, newKids - 1);
                m_hits[row] = h;
                endInsertRows();
            } else {
                beginInsertRows(pi, oldKids, newKids - 1);
                m_hits[row] = h;
                endInsertRows();
            }
        } else if (newKids < oldKids) {
            beginRemoveRows(pi, newKids, oldKids - 1);
            m_hits[row] = h;
            endRemoveRows();
        } else {
            m_hits[row] = h;
        }
        m_hits[row].key = key;
        emit dataChanged(index(row, 0), index(row, ColumnCount - 1));
        if (newKids > 0)
            emit dataChanged(index(0, 0, pi), index(newKids - 1, ColumnCount - 1, pi));
    }
    if (!fresh.isEmpty()) {
        const int first = int(m_hits.size());
        beginInsertRows(QModelIndex(), first, first + int(fresh.size()) - 1);
        for (SearchHit &h : fresh) {
            if (h.key.isEmpty())
                h.key = h.fileName + QString::number(h.size);
            m_rowOfKey.insert(h.key, int(m_hits.size()));
            m_hits << h;
        }
        endInsertRows();
    }
    return int(fresh.size());
}

int SearchModel::removeHits(const QStringList &keys)
{
    QList<int> rows;
    for (const QString &k : keys) {
        const int r = m_rowOfKey.value(k, -1);
        if (r >= 0 && !rows.contains(r))
            rows << r;
    }
    if (rows.isEmpty())
        return 0;
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    for (int r : std::as_const(rows)) {
        beginRemoveRows(QModelIndex(), r, r);
        m_hits.removeAt(r);
        endRemoveRows();
    }
    m_rowOfKey.clear();
    for (int i = 0; i < m_hits.size(); ++i)
        m_rowOfKey.insert(m_hits[i].key, i);
    return int(rows.size());
}

void SearchModel::clear()
{
    beginResetModel();
    m_hits.clear();
    m_rowOfKey.clear();
    endResetModel();
}

void SearchModel::setShowFullPaths(bool on)
{
    if (m_fullPaths == on)
        return;
    m_fullPaths = on;
    if (!m_hits.isEmpty())
        emit dataChanged(index(0, File), index(int(m_hits.size()) - 1, File));
}

SearchHit SearchModel::hitFor(const QModelIndex &i) const
{
    if (!i.isValid())
        return {};
    if (i.internalId() == 0)
        return i.row() < m_hits.size() ? m_hits.at(i.row()) : SearchHit();
    const int r = int(i.internalId()) - 1;
    if (r >= m_hits.size())
        return {};
    SearchHit h = m_hits.at(r);
    const SearchSource s = h.sources.value(i.row());
    h.sources = {s};
    return h;
}

SearchSource SearchModel::sourceFor(const QModelIndex &i) const
{
    const SearchHit h = hitFor(i);
    return h.sources.size() == 1 ? h.sources.first() : SearchSource();
}

QColor SearchModel::overrideColor(const SearchHit &h) const
{
    if (h.currentlyDownloading)
        return color(keys::colors::CurrentlyDownloading);
    if (h.canJoinRunningDownload)
        return color(keys::colors::CanJoinDownload);
    if (h.matchesIncompleteLocal)
        return color(keys::colors::MatchIncomplete);
    if (m_flagSuspects && !h.suspect.isEmpty())
        return color(ckeys::SuspectText);
    return {};
}

QString SearchModel::suspectReason(const SearchHit &h) const
{
    return m_flagSuspects ? h.suspect : QString();
}

void SearchModel::setFlagSuspects(bool on)
{
    if (m_flagSuspects == on)
        return;
    m_flagSuspects = on;
    if (!m_hits.isEmpty())
        emit dataChanged(index(0, 0), index(int(m_hits.size()) - 1, ColumnCount - 1));
}

// [merge-1] a row that holds copies matched only by name and size says so in its tool tip; a folder
// torrent names its files that other networks have too
static QString probableNote(const SearchHit &h)
{
    QStringList nets;
    bool inner = false;
    for (const SearchSource &s : h.sources) {
        if (!s.extra.value(QStringLiteral("candidate")).toBool())
            continue;
        if (s.extra.contains(QStringLiteral("innerOf")))
            inner = true;
        const QString n = netName(s.net);
        if (!nets.contains(n))
            nets << n;
    }
    QStringList lines;
    if (!nets.isEmpty())
        lines << QObject::tr("Probably the same file on %1: same name and exact size, but no common hash.").arg(nets.join(QStringLiteral(", ")))
              << QObject::tr("Every block is checked while downloading; a network whose copy differs is dropped.");
    if (inner)
        lines << QObject::tr("A folder torrent holds a file with this name and size: only that file is fetched from it.");
    for (const SearchHit::InnerMatch &m : h.innerMatches) {
        QStringList mn;
        for (Net n : m.networks)
            mn << netName(n);
        lines << QObject::tr("Also found elsewhere (same name and size): %1 (%2)").arg(m.path, mn.join(QStringLiteral(", ")));
    }
    return lines.join(QLatin1Char('\n'));
}

QVariant SearchModel::data(const QModelIndex &i, int role) const
{
    if (!i.isValid())
        return {};
    if (role == Qt::ToolTipRole && i.internalId() == 0 && i.row() < m_hits.size()
        && (i.column() == File || i.column() == Network || i.column() == Status)) {
        const QString note = probableNote(m_hits.at(i.row()));
        if (!note.isEmpty()) {
            const QString why = suspectReason(m_hits.at(i.row()));
            QString base = dataFor(i, role).toString();
            if (!why.isEmpty())
                base += (base.isEmpty() ? QString() : QStringLiteral("\n")) + tr("Suspected fake: %1").arg(why);
            return base.isEmpty() ? note : base + QLatin1Char('\n') + note;
        }
    }
    if (role == Qt::ToolTipRole) {
        // a suspected fake names the reason on every cell of its rows
        const int r = i.internalId() == 0 ? i.row() : int(i.internalId()) - 1;
        if (r < m_hits.size()) {
            const QString why = suspectReason(m_hits.at(r));
            if (!why.isEmpty()) {
                const QString base = dataFor(i, role).toString();
                const QString line = tr("Suspected fake: %1").arg(why);
                return base.isEmpty() ? line : base + QLatin1Char('\n') + line;
            }
        }
    }
    return dataFor(i, role);
}

QVariant SearchModel::dataFor(const QModelIndex &i, int role) const
{
    if (i.internalId() == 0) {
        if (i.row() >= m_hits.size())
            return {};
        const SearchHit &h = m_hits.at(i.row());
        if (h.sources.size() == 1)
            return sourceData(h, h.sources.first(), i.column(), role, false);
        return hitData(h, i.column(), role);
    }
    const int r = int(i.internalId()) - 1;
    if (r >= m_hits.size())
        return {};
    const SearchHit &h = m_hits.at(r);
    if (i.row() >= h.sources.size())
        return {};
    return sourceData(h, h.sources.at(i.row()), i.column(), role, true);
}

static QVariant commonFileData(const SearchHit &h, int col, int role, bool fullPaths, const QColor &ovr)
{
    switch (role) {
    case Qt::DisplayRole:
        switch (col) {
        case File: return fullPaths && !h.path.isEmpty() ? h.path : h.fileName;
        case Size: return h.size > 0 ? fmt::size(h.size) : QStringLiteral("?");   // unknown size (magnet): "?"
        case Time: return fmt::duration(h.durationSec);
        case Bitrate: return fmt::bitrate(h.bitrateKbps);
        case Freq: return fmt::frequency(h.sampleRateHz);
        default: return {};
        }
    case Qt::ForegroundRole:
        if (col == Bitrate)
            return h.bitrateKbps > 0 ? bitrateColor(h.bitrateKbps) : QColor();
        if (ovr.isValid() && (col == File || col == Size || col == Time || col == Freq || col == User || col == Network))
            return ovr;
        return {};
    case SortRole:
        switch (col) {
        case File: return h.fileName.toLower();
        case Size: return h.size > 0 ? h.size : qint64(-1);
        case Time: return h.durationSec;
        case Bitrate: return h.bitrateKbps;
        case Freq: return h.sampleRateHz;
        default: return {};
        }
    case Qt::TextAlignmentRole:
        switch (col) {
        case Size: case Time: case Bitrate: case Freq: case Ping: case Peers:
            return int(Qt::AlignRight | Qt::AlignVCenter);
        default:
            return int(Qt::AlignLeft | Qt::AlignVCenter);
        }
    case Qt::ToolTipRole:
        if (col == File)
            return h.path.isEmpty() ? h.fileName : h.path;
        if (col == Size)
            return h.size > 0 ? fmt::exactBytes(h.size) : QObject::tr("Size not known yet");
        return {};
    case IdRole:
        return h.key;
    default:
        return {};
    }
}

// [ui-e] the Peers column: one number, right-aligned, "?" when unknown (sorts last)
static QVariant peersData(int peers, int role)
{
    switch (role) {
    case Qt::DisplayRole: return peers >= 0 ? fmt::number(peers) : QStringLiteral("?");
    case SortRole: return peers;
    case Qt::TextAlignmentRole: return int(Qt::AlignRight | Qt::AlignVCenter);
    case Qt::ToolTipRole:
        return peers >= 0 ? QObject::tr("%1 peers known on every network").arg(fmt::number(peers)) : QObject::tr("Number of peers not known");
    default: return {};
    }
}

static QString seedsPeersText(int seeds, int leechers)
{
    const QString a = seeds == 1 ? QObject::tr("1 seed") : QObject::tr("%1 seeds").arg(qMax(0, seeds));
    const QString b = leechers == 1 ? QObject::tr("1 peer") : QObject::tr("%1 peers").arg(qMax(0, leechers));
    return a + QStringLiteral(", ") + b;
}

QVariant SearchModel::hitData(const SearchHit &h, int col, int role) const
{
    const QColor ovr = overrideColor(h);
    const bool torrent = allTorrent(h);
    if (col <= Freq) {
        if (role == KindRole)
            return 0;
        return commonFileData(h, col, role, m_fullPaths, ovr);
    }
    if (col == Peers) {
        if (role == IdRole)
            return h.key;
        if (role == KindRole)
            return 0;
        return peersData(totalPeers(h), role);
    }
    if (col == Network) {
        switch (role) {
        case Qt::DisplayRole: return networkText(h);
        case Qt::ForegroundRole: return ovr.isValid() ? QVariant(ovr) : QVariant();
        case SortRole: return networkText(h).toLower();
        case Qt::ToolTipRole: {
            QStringList nets;
            for (Net n : h.networks)
                nets << netName(n);
            QString t = nets.join(QStringLiteral(", "));
            if (!h.indexes.isEmpty())
                t += (t.isEmpty() ? QString() : QStringLiteral("\n")) + tr("Listed by: %1").arg(h.indexes.join(QStringLiteral(", ")));
            return t.isEmpty() ? QVariant() : QVariant(t);
        }
        case IdRole: return h.key;
        case KindRole: return 0;
        default: return {};
        }
    }
    // group columns: Status and the classic User / Speed / Ping
    int fast = 0, slow = 0, availFast = 0, availSlow = 0;
    QList<int> queues;
    for (const SearchSource &s : h.sources) {
        const bool f = isFast(s);
        (f ? fast : slow)++;
        if (isAvailable(s))
            (f ? availFast : availSlow)++;
        else
            queues << queueOf(s);
    }
    std::sort(queues.begin(), queues.end());
    int leechers = 0;
    const int seeds = torrent ? seedsOf(h, &leechers) : 0;
    const QColor white = color(keys::colors::ListText);
    switch (role) {
    case Qt::DisplayRole:
        switch (col) {
        // a torrent group counts the swarm; with no swarm numbers yet (index sites only) its sources
        case User: {
            // [qa-1] "1 user", never "1 users" (a torrent group with one known peer)
            const int n = torrent && seeds + leechers > 0 ? seeds + leechers : int(h.sources.size());
            return n == 1 ? tr("1 user") : tr("%1 users").arg(n);
        }
        case Speed: return torrent ? QStringLiteral("%1 + %2").arg(seeds).arg(leechers) : QStringLiteral("%1 + %2").arg(fast).arg(slow);
        case Ping: return {};
        case Status:
            if (torrent)
                return seedsPeersText(seeds, leechers);
            if (availFast + availSlow > 0)
                return tr("%1 + %2 available users").arg(availFast).arg(availSlow);
            return tr("%1 in lowest queues").arg(QStringLiteral("%1 %2").arg(queues.value(0)).arg(queues.value(1, queues.value(0))));
        default: return {};
        }
    case SegmentsRole:
        if (col == Speed) {
            const int a = torrent ? seeds : fast, b = torrent ? leechers : slow;
            return makeSegments({{QString::number(a), color(keys::colors::SpeedFast), {}},
                                 {QStringLiteral(" + "), white, {}},
                                 {QString::number(b), color(keys::colors::SpeedSlow), {}}});
        }
        if (col == Status && !torrent && availFast + availSlow > 0)
            return makeSegments({{QString::number(availFast), color(keys::colors::SpeedFast), {}},
                                 {QStringLiteral(" + "), white, {}},
                                 {QString::number(availSlow), color(keys::colors::SpeedSlow), {}},
                                 {tr(" available users"), color(keys::colors::Available), {}}});
        return {};
    case Qt::ForegroundRole:
        if (col == User)
            return ovr.isValid() ? ovr : QVariant();
        if (col == Status && torrent)
            return slotsColor(seeds);
        return {};
    case Qt::TextAlignmentRole:
        return col == User ? int(Qt::AlignHCenter | Qt::AlignVCenter) : int(Qt::AlignLeft | Qt::AlignVCenter);
    case ClipRole:
        return col == Speed;
    case SortRole:
        switch (col) {
        case User: return int(h.sources.size());
        case Speed: return torrent ? seeds : fast;
        case Ping: return 1000000;
        case Status: return torrent ? seeds : availFast + availSlow;
        default: return {};
        }
    case IdRole:
        return h.key;
    case KindRole:
        return 0;
    default:
        return {};
    }
}

namespace {
// the source's own text of the Status column: slots / queue / swarm, its declared line type, ping
QString sourceStatus(const SearchSource &s, bool torrentSrc)
{
    QStringList parts;
    // [merge-1] the copy matched only by name and size: the mark the user sees
    if (s.extra.value(QStringLiteral("candidate")).toBool())
        parts << QObject::tr("same name and size, checked while downloading");
    if (torrentSrc && s.net != Net::BitTorrent && s.net != Net::Unknown) {
        // [ui-j] eD2k / Kad report sources (and how many have the complete file), not a swarm:
        // their own words, never "seeds" and "peers"
        const int all = qMax(qMax(0, s.peers), qMax(0, s.seeds));
        const int complete = qMax(0, s.seeds);
        QString t = all == 1 ? QObject::tr("1 source") : QObject::tr("%1 sources").arg(all);
        if (complete > 0 && complete < all)
            t += QObject::tr(" (%1 complete)").arg(complete);
        parts << t;
    } else if (torrentSrc) {
        parts << seedsPeersText(qMax(0, s.seeds), qMax(0, s.peers));
    } else if (s.totalSlots > 0 && s.freeSlots > 0) {
        parts << QObject::tr("%1 of %2 available").arg(s.freeSlots).arg(s.totalSlots);
    } else if (s.totalSlots > 0 && s.freeSlots == 0) {
        parts << QObject::tr("%1 in queue (0 of %2 available)").arg(queueOf(s)).arg(s.totalSlots);
    } else if (!serverLabel(s).isEmpty()) {
        parts << serverLabel(s);
    }
    // the connection an OctoMX / Gnutella user declared
    if (s.net != Net::BitTorrent && isConnectionType(s.connection))
        parts << s.connection;
    if (s.pingMs >= 0)
        parts << QObject::tr("%1 ms").arg(s.pingMs);
    return parts.join(QStringLiteral(", "));
}

int sourcePeers(const SearchSource &s, bool torrentSrc)
{
    if (torrentSrc)
        return s.seeds < 0 && s.peers < 0 ? -1 : qMax(0, s.seeds) + qMax(0, s.peers);
    if (s.user.isValid() || !s.user.name.isEmpty() || !s.user.address.isEmpty())
        return 1;
    const int n = qMax(s.seeds, s.peers);
    return n > 0 ? n : -1;
}

QString sourceNetworkText(const SearchSource &s)
{
    // [merge-1] a folder torrent that holds this file
    const QString inside = s.extra.value(QStringLiteral("insideBundle")).toString();
    if (!inside.isEmpty())
        return QObject::tr("BT (inside \"%1\")").arg(inside);
    const QString idx = s.extra.value(QStringLiteral("providerName")).toString();
    if (!idx.isEmpty() && s.net == Net::BitTorrent)
        return idx + QStringLiteral(" (BT)");
    const QString net = netLabel(s.net);
    QString who;
    if (s.user.isValid() || !s.user.name.isEmpty())
        who = s.user.name.isEmpty() ? s.user.id : s.user.name;
    else if (!s.connection.isEmpty() && !isConnectionType(s.connection) && s.connection != net)
        who = s.connection;   // a tracker, DHT, a web server
    return who.isEmpty() ? net : net + QStringLiteral(": ") + who;
}
} // namespace

QVariant SearchModel::sourceData(const SearchHit &h, const SearchSource &s, int col, int role, bool child) const
{
    const QColor ovr = overrideColor(h);
    if (role == KindRole)
        return child ? 1 : 0;
    const int partial = s.extra.value(QStringLiteral("partialPercent"), 0).toInt();
    const bool torrentSrc = s.net == Net::BitTorrent || (s.freeSlots < 0 && s.seeds >= 0);
    if (col == Size && partial > 0) {
        switch (role) {
        case Qt::DisplayRole: return fmt::sizeOfPercent(partial, h.size);
        case Qt::ForegroundRole: return color(keys::colors::PartialText);
        default: break;
        }
    }
    if (col <= Freq)
        return commonFileData(h, col, role, m_fullPaths, ovr);
    if (col == Peers) {
        if (role == IdRole)
            return h.key;
        // a single-source row stands for the whole hit
        return peersData(child ? sourcePeers(s, torrentSrc) : totalPeers(h), role);
    }
    const QColor white = color(keys::colors::ListText);
    switch (role) {
    case Qt::DisplayRole:
        switch (col) {
        case Network: return child ? sourceNetworkText(s) : networkText(h);
        case User: return userLabel(s);
        case Speed:
            if (torrentSrc)
                return QStringLiteral("%1 + %2").arg(qMax(0, s.seeds)).arg(qMax(0, s.peers));
            return connectionText(s.connection);
        case Ping: return s.pingMs >= 0 ? QString::number(s.pingMs) : (s.extra.value(QStringLiteral("pinged")).toBool() ? QStringLiteral("?") : QString());
        case Status: return sourceStatus(s, torrentSrc);
        default: return {};
        }
    case SegmentsRole:
        if (col == Speed && torrentSrc)
            return makeSegments({{QString::number(qMax(0, s.seeds)), color(keys::colors::SpeedFast), {}},
                                 {QStringLiteral(" + "), white, {}},
                                 {QString::number(qMax(0, s.peers)), color(keys::colors::SpeedSlow), {}}});
        return {};
    case Qt::ForegroundRole:
        switch (col) {
        case User: return ovr.isValid() ? ovr : QVariant();
        case Network: return ovr.isValid() ? QVariant(ovr) : QVariant();
        case Speed: return speedColor(sourceRank(s));
        case Status:
            if (torrentSrc)
                return slotsColor(qMax(0, s.seeds));
            if (s.totalSlots > 0)
                return slotsColor(qMax(0, s.freeSlots));
            return {};
        default: return {};
        }
    case Qt::TextAlignmentRole:
        return col == Ping ? int(Qt::AlignRight | Qt::AlignVCenter) : int(Qt::AlignLeft | Qt::AlignVCenter);
    case ClipRole:
        return col == Speed;
    case SortRole:
        switch (col) {
        case Network: return (child ? sourceNetworkText(s) : networkText(h)).toLower();
        case User: return userLabel(s).toLower();
        case Speed: return sourceRank(s);
        case Ping: return s.pingMs >= 0 ? s.pingMs : 1000000;
        case Status: return torrentSrc ? qMax(0, s.seeds) : (s.freeSlots > 0 ? s.freeSlots : -queueOf(s));
        default: return {};
        }
    case Qt::ToolTipRole:
        if (col == User || col == Network)
            return QStringLiteral("%1 (%2)").arg(userLabel(s), netName(s.net));
        return {};
    case IdRole:
        return h.key;
    default:
        return {};
    }
}

// --------------------------------------------------------------------------------- SearchProxy

SearchProxy::SearchProxy(QObject *parent) : QSortFilterProxyModel(parent)
{
    setSortRole(SortRole);
    setDynamicSortFilter(true);
    setRecursiveFilteringEnabled(false);
    setSortCaseSensitivity(Qt::CaseInsensitive);
}

void SearchProxy::setAvailableOnTop(bool on)
{
    if (m_availTop == on)
        return;
    m_availTop = on;
    invalidate();
    if (sortColumn() < 0 && on)
        sort(0, Qt::AscendingOrder);
}

void SearchProxy::setMinConnectionRank(int rank)
{
    if (m_minRank == rank)
        return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange();
    m_minRank = rank;
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
#else
    m_minRank = rank;
    invalidateFilter();
#endif
}

static bool hitAvailable(const SearchModel *m, const QModelIndex &src)
{
    const SearchHit h = m->hitFor(src);
    for (const SearchSource &s : h.sources)
        if (search::isAvailable(s))
            return true;
    return false;
}

bool SearchProxy::lessThan(const QModelIndex &a, const QModelIndex &b) const
{
    if (m_availTop && !a.parent().isValid()) {
        const auto *m = static_cast<const SearchModel *>(sourceModel());
        const bool aa = hitAvailable(m, a), ba = hitAvailable(m, b);
        if (aa != ba)
            return sortOrder() == Qt::AscendingOrder ? aa : !aa;
    }
    const QVariant va = a.data(SortRole), vb = b.data(SortRole);
    if (va.typeId() == QMetaType::QString || vb.typeId() == QMetaType::QString)
        return QString::compare(va.toString(), vb.toString(), Qt::CaseInsensitive) < 0;
    // [ui-e] unknown peers / size ("?", -1) sort last in both directions
    if (a.column() == search::Peers || a.column() == search::Size) {
        const bool ua = va.toLongLong() < 0, ub = vb.toLongLong() < 0;
        if (ua != ub)
            return sortOrder() == Qt::AscendingOrder ? ub : ua;
    }
    return va.toLongLong() < vb.toLongLong();
}

void SearchProxy::setHideSuspects(bool on)
{
    if (m_hideSuspects == on)
        return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange();
    m_hideSuspects = on;
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
#else
    m_hideSuspects = on;
    invalidateFilter();
#endif
}

void SearchProxy::setTypeFilter(const SearchQuery &q)
{
    SearchQuery t;   // only the file-type part
    t.type = q.type;
    t.extensions = queryExtensions(q);
    t.minBitrate = q.minBitrate;
    t.maxBitrate = q.maxBitrate;
    const bool on = t.type != MediaType::Any || !t.extensions.isEmpty() || t.minBitrate > 0 || t.maxBitrate > 0;
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange();
#endif
    m_type = t;
    m_typeExts = t.extensions;
    m_typeOn = on;
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
#else
    invalidateFilter();
#endif
}

bool SearchProxy::typeAccepts(const SearchHit &h) const
{
    return !m_typeOn || hitMatchesFileType(m_type, h);
}

bool SearchProxy::filterAcceptsRow(int row, const QModelIndex &parent) const
{
    const auto *m = static_cast<const SearchModel *>(sourceModel());
    if (m_hideSuspects && !parent.isValid() && row < m->hitCount() && !m->hitAt(row).suspect.isEmpty())
        return false;
    if (m_typeOn && !parent.isValid() && row < m->hitCount() && !typeAccepts(m->hitAt(row)))
        return false;
    if (m_typeOn && parent.isValid() && !m_typeExts.isEmpty()) {
        const QString name = m->sourceFor(m->index(row, 0, parent)).extra.value(QStringLiteral("fileName")).toString();
        if (!name.isEmpty()) {
            const int dot = name.lastIndexOf(QLatin1Char('.'));
            if (dot <= 0 || !m_typeExts.contains(name.mid(dot + 1).toLower()))
                return false;
        }
    }
    if (m_minRank <= 0)
        return true;
    const QModelIndex src = m->index(row, 0, parent);
    if (parent.isValid())
        return search::sourceRank(m->sourceFor(src)) >= m_minRank;
    const SearchHit &h = m->hitAt(row);
    for (const SearchSource &s : h.sources)
        if (search::sourceRank(s) >= m_minRank)
            return true;
    return false;
}

} // namespace mx::ui

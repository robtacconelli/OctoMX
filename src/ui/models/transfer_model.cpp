// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/models/transfer_model.h"

#include "mx/services.h"
#include "mx/settings_keys.h"
#include "ui/kit/colors.h"
#include "ui/kit/format.h"
#include "ui/kit/piece_bar.h"
#include "ui/models/search_model.h"

namespace mx::ui {

namespace transfers {

QColor statusColor(const QString &text, TransferState st, bool upload)
{
    const QString s = text.toLower();
    if (s.startsWith(QLatin1String("remotely queued")))
        return color(ckeys::TransferRemoteQueued);
    if (s.startsWith(QLatin1String("busy")))
        return color(ckeys::TransferBusy);
    if (s.startsWith(QLatin1String("timed out")) || s.contains(QLatin1String("refused")) ||
        s.contains(QLatin1String("unavailable")) || s.startsWith(QLatin1String("disconnected")) ||
        s.contains(QLatin1String("reset by peer")) || s.contains(QLatin1String("firewalled")) ||
        s.startsWith(QLatin1String("file mismatch")) || s.startsWith(QLatin1String("segment error")) ||
        s.contains(QLatin1String("does not match")) || s.contains(QLatin1String("is a different file")))   // [eng-1] a copy dropped
        return color(keys::colors::TransferError);
    if (s.startsWith(QLatin1String("connected, negotiating")))
        return color(ckeys::TransferNegotiating);
    if (s.startsWith(QLatin1String("cancelled")))
        return color(ckeys::TransferCancelled);
    if (s.startsWith(QLatin1String("finding sources")) || s.startsWith(QLatin1String("waiting")) ||
        s.startsWith(QLatin1String("connecting")) || s.startsWith(QLatin1String("checking")))
        return color(ckeys::TransferWaiting);
    switch (st) {
    case TransferState::Transferring: return color(keys::colors::TransferActive);
    case TransferState::Completed:
    case TransferState::Seeding: return color(keys::colors::TransferComplete);
    case TransferState::Queued: return upload ? color(ckeys::TransferRemoteQueued) : color(keys::colors::TransferQueued);
    case TransferState::RemotelyQueued: return color(ckeys::TransferRemoteQueued);
    case TransferState::FindingSources:
    case TransferState::Connecting:
    case TransferState::Verifying: return color(ckeys::TransferWaiting);
    case TransferState::Error:
    case TransferState::TimedOut: return color(keys::colors::TransferError);
    case TransferState::Paused:
    case TransferState::Cancelled: return color(keys::colors::TransferPaused);
    }
    return color(keys::colors::ListText);
}

// [qa-1] some engines report bare peer states (BitTorrent: "Connecting", "Handshake", "Connected",
// "Waiting"); show them in the original WinMX wording of UI_SPEC 8.7 (exact matches only)
QString classicSourceStatus(const QString &raw)
{
    if (raw == QLatin1String("Connecting"))
        return QStringLiteral("Connecting to Remote User");
    if (raw == QLatin1String("Handshake") || raw == QLatin1String("Connected"))
        return QStringLiteral("Connected, Negotiating...");
    if (raw == QLatin1String("Waiting"))
        return QStringLiteral("Remotely Queued");
    // [ui-e] a BitTorrent peer that has the whole file (engine: "Seed")
    if (raw == QLatin1String("Seed") || raw == QLatin1String("Seeding"))
        return QStringLiteral("Connected (complete source)");
    return raw;
}

// [ui-e] the Network column: "BT+eD2k" for a file row, "eD2k", "OctoMX, DSL", "Kad network" ... for a source
QString networksText(const QList<Net> &nets)
{
    QStringList parts;
    for (Net n : nets) {
        const QString l = search::netLabel(n);
        if (!l.isEmpty() && !parts.contains(l))
            parts << l;
    }
    return parts.join(QLatin1Char('+'));
}

QString sourceNetworkText(const TransferSource &s)
{
    const QString net = search::netLabel(s.net);
    if (isConnectionType(s.connection) && s.connection != QLatin1String("Unknown"))
        return net.isEmpty() ? s.connection : net + QStringLiteral(", ") + s.connection;
    if (isConnectionType(s.connection))
        return net;
    // a channel row ("DHT", "Tracker", "Search") already names where the sources come from
    if (!s.connection.isEmpty() && s.connection != net)
        return net.isEmpty() ? search::connectionText(s.connection) : net + QStringLiteral(" ") + search::connectionText(s.connection);
    return net;
}

QColor sourceStatusColor(const TransferSource &s)
{
    if (s.statusText.isEmpty())
        return s.rateBps > 0 ? color(keys::colors::TransferActive) : color(keys::colors::ListText);
    if (s.statusText.startsWith(QLatin1String("Complete")))
        return color(keys::colors::TransferComplete);
    return statusColor(classicSourceStatus(s.statusText), s.rateBps > 0 ? TransferState::Transferring : TransferState::Connecting, false);
}

// the core's generic "Transferring" text adds nothing to the progress bar the original WinMX draws
bool plainTransferring(const TransferInfo &t)
{
    return t.state == TransferState::Transferring &&
           (t.statusText.isEmpty() || t.statusText.compare(QLatin1String("Transferring"), Qt::CaseInsensitive) == 0 ||
            t.statusText == transferStateText(TransferState::Transferring));
}

QString displayStatus(const TransferInfo &t)
{
    if (!t.statusText.isEmpty() && !plainTransferring(t))
        return t.statusText;
    switch (t.state) {
    case TransferState::Transferring: return fmt::percent(t.done, t.size);
    case TransferState::Verifying: return QObject::tr("Checking...");
    default: return transferStateText(t.state);
    }
}

bool isFinished(const TransferInfo &t)
{
    return t.state == TransferState::Completed || t.state == TransferState::Seeding || t.state == TransferState::Cancelled;
}

// [ui-i] a download row shows the bar (a map of the file when the core gives one) while it transfers,
// and also while it waits, is paused, queued or checked once it has something: the status text is
// then drawn in the bar. Errors keep their red text (it runs on over the empty cells); finished rows
// say Complete as in the original WinMX.
bool showsBar(const TransferInfo &t)
{
    if (t.isUpload || isFinished(t) || t.state == TransferState::Error || t.state == TransferState::TimedOut)
        return false;
    if (plainTransferring(t))
        return true;
    if (t.size <= 0)
        return false;
    if (t.done > 0)
        return true;
    for (char c : t.pieceMap)
        if (c == 2)
            return true;
    for (char c : t.pieceFill)
        if (c != 0)
            return true;
    return false;
}

} // namespace transfers

using namespace transfers;

TransferModel::TransferModel(TransferService *svc, bool uploads, Settings *settings, QObject *parent)
    : QAbstractItemModel(parent), m_svc(svc), m_settings(settings), m_uploads(uploads)
{
    if (svc) {
        connect(svc, &TransferService::transferAdded, this, [this](const TransferInfo &t) {
            if (t.isUpload == m_uploads)
                addTransfer(t);
        });
        connect(svc, &TransferService::transferChanged, this, [this](const TransferInfo &t) {
            if (t.isUpload == m_uploads)
                updateTransfer(t);
        });
        connect(svc, &TransferService::transferRemoved, this, [this](const QString &id, bool wasUpload) {
            if (wasUpload == m_uploads)
                removeTransfer(id);
        });
    }
    if (settings)
        connect(settings, &Settings::changed, this, [this](const QString &k, const QVariant &) {
            // [ui-e] the global AFS / AEQ (shown where a download has no own value) and the size format
            if (!m_rows.isEmpty() && ((!m_uploads && (k == QLatin1String(keys::search::AutoFindSourcesMin) ||
                                                       k == QLatin1String(keys::search::AutoEnterQueue))) ||
                                      k == QLatin1String(keys::ui::ExactSizes)))
                emit dataChanged(index(0, 0), index(int(m_rows.size()) - 1, columnCount() - 1));
        });
    connect(Colors::instance(), &Colors::changed, this, [this] {
        if (!m_rows.isEmpty())
            emit dataChanged(index(0, 0), index(int(m_rows.size()) - 1, columnCount() - 1));
    });
    reload();
}

void TransferModel::reload()
{
    beginResetModel();
    m_rows = m_svc ? (m_uploads ? m_svc->uploads() : m_svc->downloads()) : QList<TransferInfo>();
    m_rowOfId.clear();
    for (int i = 0; i < m_rows.size(); ++i)
        m_rowOfId.insert(m_rows[i].id, i);
    endResetModel();
}

void TransferModel::addTransfer(const TransferInfo &t)
{
    if (m_rowOfId.contains(t.id)) {
        updateTransfer(t);
        return;
    }
    const int row = int(m_rows.size());
    beginInsertRows(QModelIndex(), row, row);
    m_rows << t;
    m_rowOfId.insert(t.id, row);
    endInsertRows();
}

void TransferModel::updateTransfer(const TransferInfo &t)
{
    const int row = m_rowOfId.value(t.id, -1);
    if (row < 0) {
        addTransfer(t);
        return;
    }
    const QModelIndex pi = index(row, 0);
    const int oldKids = childCount(m_rows[row]);
    const int newKids = childCount(t);
    if (newKids > oldKids) {
        beginInsertRows(pi, oldKids, newKids - 1);
        m_rows[row] = t;
        endInsertRows();
    } else if (newKids < oldKids) {
        beginRemoveRows(pi, newKids, oldKids - 1);
        m_rows[row] = t;
        endRemoveRows();
    } else {
        m_rows[row] = t;
    }
    emit dataChanged(index(row, 0), index(row, columnCount() - 1));
    if (newKids > 0)
        emit dataChanged(index(0, 0, pi), index(newKids - 1, columnCount() - 1, pi));
}

void TransferModel::removeTransfer(const QString &id)
{
    const int row = m_rowOfId.value(id, -1);
    if (row < 0)
        return;
    beginRemoveRows(QModelIndex(), row, row);
    m_rows.removeAt(row);
    m_rowOfId.clear();
    for (int i = 0; i < m_rows.size(); ++i)
        m_rowOfId.insert(m_rows[i].id, i);
    endRemoveRows();
}

QModelIndex TransferModel::index(int row, int column, const QModelIndex &parent) const
{
    if (row < 0 || column < 0 || column >= columnCount())
        return {};
    if (!parent.isValid())
        return row < m_rows.size() ? createIndex(row, column, quintptr(0)) : QModelIndex();
    if (parent.internalId() != 0 || parent.row() >= m_rows.size() || row >= childCount(m_rows.at(parent.row())))
        return {};
    return createIndex(row, column, quintptr(parent.row() + 1));
}

QModelIndex TransferModel::parent(const QModelIndex &child) const
{
    if (!child.isValid() || child.internalId() == 0)
        return {};
    return createIndex(int(child.internalId()) - 1, 0, quintptr(0));
}

int TransferModel::rowCount(const QModelIndex &parent) const
{
    if (!parent.isValid())
        return int(m_rows.size());
    if (parent.internalId() != 0 || parent.column() != 0 || parent.row() >= m_rows.size())
        return 0;
    return childCount(m_rows.at(parent.row()));
}

bool TransferModel::hasChildren(const QModelIndex &parent) const
{
    return rowCount(parent) > 0;
}

int TransferModel::columnCount(const QModelIndex &) const
{
    return m_uploads ? int(UColumnCount) : int(DColumnCount);
}

QVariant TransferModel::headerData(int section, Qt::Orientation o, int role) const
{
    if (o != Qt::Horizontal)
        return {};
    if (role == Qt::DisplayRole) {
        switch (section) {
        case 0: return tr("File");
        case 1: return m_uploads ? tr("User") : tr("User  -  AFS  -  AEQ");
        // [ui-e] the column shows the network (and the line type a user declared), as in Search
        case 2: return fmt::exactSizes() ? tr("Speed") : tr("Network");
        case 3: return fmt::exactSizes() ? tr("Bytes") : tr("Size");
        case 4: return tr("Status");
        case 5: return fmt::exactSizes() ? tr("K/s") : tr("Rate");
        case 6: return tr("Time Left");
        default: return {};
        }
    }
    if (role == Qt::TextAlignmentRole)
        return (section == 5 || section == 6) ? int(Qt::AlignRight | Qt::AlignVCenter) : int(Qt::AlignLeft | Qt::AlignVCenter);
    return {};
}

TransferInfo TransferModel::transferFor(const QModelIndex &i) const
{
    if (!i.isValid())
        return {};
    const int row = i.internalId() == 0 ? i.row() : int(i.internalId()) - 1;
    return row < m_rows.size() ? m_rows.at(row) : TransferInfo();
}

TransferSource TransferModel::sourceFor(const QModelIndex &i) const
{
    if (!isSourceRow(i))
        return {};
    return transferFor(i).sources.value(i.row());
}

QString TransferModel::afsAeq(const TransferInfo &t) const
{
    int afs = 10, aeq = 100;
    if (m_settings) {
        afs = m_settings->i(keys::search::AutoFindSourcesMin);
        aeq = m_settings->i(keys::search::AutoEnterQueue);
    }
    // [qa-1] engine-4: the effective per-download values (-1 = not reported -> the global setting)
    if (t.autoFindSourcesMin >= 0)
        afs = t.autoFindSourcesMin;
    if (t.autoEnterQueue >= 0)
        aeq = t.autoEnterQueue;
    return QStringLiteral("%1 - %2").arg(afs > 0 ? QStringLiteral("%1m").arg(afs) : tr("Never"),
                                         aeq > 0 ? QString::number(aeq) : tr("Never"));
}

QString TransferModel::summaryText() const
{
    int n = 0, users = 0;
    qint64 bps = 0;
    for (const TransferInfo &t : m_rows) {
        if (t.state != TransferState::Transferring)
            continue;
        ++n;
        if (m_uploads) {
            ++users;
            bps += t.rateUpBps;
        } else {
            int active = 0;
            for (const TransferSource &s : t.sources)
                if (s.rateBps > 0)
                    ++active;
            users += qMax(active, t.sourcesActive);
            bps += t.rateDownBps;
        }
    }
    // [ui-e] "at 449 KB/s total" (classic with exact byte counts: "at 460 540 bytes per second total")
    // [qa-2] singular forms: "1 downloads from 1 users" (real core, one active download)
    const QString who = users == 1 ? tr("1 user") : tr("%1 users").arg(users);
    if (m_uploads)
        return n == 1 ? tr("1 upload to %1 at %2 total").arg(who, fmt::rateLong(bps))
                      : tr("%1 uploads to %2 at %3 total").arg(n).arg(who, fmt::rateLong(bps));
    return n == 1 ? tr("1 download from %1 at %2 total").arg(who, fmt::rateLong(bps))
                  : tr("%1 downloads from %2 at %3 total").arg(n).arg(who, fmt::rateLong(bps));
}

QVariant TransferModel::data(const QModelIndex &i, int role) const
{
    if (!i.isValid())
        return {};
    const TransferInfo t = transferFor(i);
    if (t.id.isEmpty())
        return {};
    if (role == IdRole)
        return t.id;
    if (role == KindRole)
        return isSourceRow(i) ? 1 : 0;
    if (role == ClipRole)
        return i.column() == 2;
    if (role == OverflowRole)
        return i.column() == 4;
    if (m_uploads)
        return uploadData(t, i.column(), role);
    if (isSourceRow(i))
        return sourceData(t, t.sources.value(i.row()), i.column(), role);
    return fileData(t, i.column(), role);
}

static QVariant alignFor(int col)
{
    // (columns 2 = Speed: clipped, 4 = Status: may overflow; see roleExtra below)
    return (col == 5 || col == 6) ? int(Qt::AlignRight | Qt::AlignVCenter) : int(Qt::AlignLeft | Qt::AlignVCenter);
}

// [dl-1] the two order flags of a download: a small mark after the name, the details in the tool tip
static QString orderMarks(const TransferInfo &t)
{
    if (t.isUpload || transfers::isFinished(t) || (!t.sequential && !t.firstLastFirst))
        return QString();
    if (t.sequential && t.firstLastFirst)
        return QStringLiteral("  [Seq F+L]");
    return t.sequential ? QStringLiteral("  [Seq]") : QStringLiteral("  [F+L]");
}

static QString orderTip(const TransferInfo &t)
{
    QStringList what;
    if (t.sequential)
        what << TransferModel::tr("in sequential order");
    if (t.firstLastFirst)
        what << TransferModel::tr("first and last pieces first");
    QString tip = TransferModel::tr("Download order: %1.").arg(what.join(TransferModel::tr(", ")));
    if (!t.orderFollowedBy.isEmpty()) {
        QStringList n;
        for (Net x : t.orderFollowedBy)
            n << netName(x);
        tip += QLatin1Char(' ') + TransferModel::tr("Followed by: %1.").arg(n.join(QStringLiteral(", ")));
    } else if (!transfers::isFinished(t)) {
        tip += QLatin1Char(' ') + TransferModel::tr("No connected source can follow it yet; the file downloads normally meanwhile.");
    }
    return tip;
}

QVariant TransferModel::fileData(const TransferInfo &t, int col, int role) const
{
    const QColor statusC = statusColor(t.statusText, t.state, false);
    // file rows of downloads without a live source are drawn red ("gone red")
    QColor rowC;
    switch (t.state) {
    case TransferState::Transferring: rowC = color(keys::colors::TransferActive); break;
    case TransferState::Completed:
    case TransferState::Seeding: rowC = color(keys::colors::TransferComplete); break;
    case TransferState::Paused:
    case TransferState::Cancelled: rowC = color(keys::colors::TransferPaused); break;
    case TransferState::Queued: rowC = color(keys::colors::TransferQueued); break;
    case TransferState::Verifying: rowC = color(ckeys::TransferWaiting); break;
    default: rowC = color(keys::colors::TransferError); break;
    }
    switch (role) {
    case Qt::DisplayRole:
        switch (col) {
        case DFile: return t.name + orderMarks(t);   // [dl-1] [Seq] / [F+L] / [Seq F+L]
        case DUser: {
            // the known sources, also those the core does not list as rows (not connected yet); [ui-e]
            // network / channel / "+ N more peers" rows are not users: sourcesTotal when the core has it
            int listed = 0;
            for (const TransferSource &src : t.sources)
                if (!src.id.isEmpty() || src.user.isValid())
                    ++listed;
            const int n = t.sourcesTotal > 0 ? t.sourcesTotal : listed;
            if (n > 1)
                return tr("%1 users - %2").arg(n).arg(afsAeq(t));
            if (n == 1 && t.sources.isEmpty())
                return tr("1 user - %1").arg(afsAeq(t));
            return afsAeq(t);
        }
        case DSpeed: return fmt::exactSizes() ? QString() : networksText(t.networks);
        case DBytes: return fmt::sizeOf(t.done, t.size);
        case DStatus: return displayStatus(t);
        case DRate:
            // only transferring rows show a rate, so long status strings can run on (as in the original WinMX)
            return t.state == TransferState::Transferring ? fmt::rate(t.rateDownBps) : QString();
        case DTimeLeft: return t.state == TransferState::Transferring ? fmt::etaText(t.etaSec) : QString();
        default: return {};
        }
    case ProgressRole:
        if (col == DStatus && showsBar(t))
            return fmt::percentInt(t.done, t.size);
        return {};
    case PieceMapRole:   // [ui-i]
        return col == DStatus && showsBar(t) ? QVariant(t.pieceMap) : QVariant();
    case PieceFillRole:
        return col == DStatus && showsBar(t) ? QVariant(t.pieceFill) : QVariant();
    case BarTextColorRole:   // a status text in the bar keeps its colour; the percentage is classic blue
        return col == DStatus && showsBar(t) && !plainTransferring(t) ? QVariant(statusC) : QVariant();
    case Qt::ForegroundRole:
        return col == DStatus ? statusC : rowC;
    case Qt::TextAlignmentRole:
        return alignFor(col);
    case Qt::ToolTipRole:
        if (col == DFile) {
            // [ui-e] dates in the system locale
            QString tip = t.savePath.isEmpty() ? t.name : t.savePath;
            if (!t.isUpload && (t.sequential || t.firstLastFirst) && !transfers::isFinished(t))   // [dl-1]
                tip += QLatin1Char('\n') + orderTip(t);
            if (t.added.isValid())
                tip += QLatin1Char('\n') + tr("Added: %1").arg(fmt::dateTime(t.added));
            if (t.completed.isValid())
                tip += QLatin1Char('\n') + tr("Completed: %1").arg(fmt::dateTime(t.completed));
            return tip;
        }
        if (col == DBytes)
            return t.size > 0 ? tr("%1 of %2").arg(fmt::exactBytes(t.done), fmt::exactBytes(t.size)) : fmt::exactBytes(t.done);
        if (col == DStatus) {
            QString tip = t.errorText.isEmpty() ? displayStatus(t) : displayStatus(t) + QLatin1Char('\n') + t.errorText;
            // [ui-i] what the bar shows, in numbers: "412 MB of 1.09 GB (38 %), 6 parts downloading"
            if (showsBar(t)) {
                const QString line = piecebar::tipLine(t.done, t.size, t.pieceMap);
                tip = plainTransferring(t) ? line : tip + QLatin1Char('\n') + line;
            }
            if (!t.diagnosis.isEmpty()) tip += QStringLiteral("\n\n") + t.diagnosis;   // [diag-1] why it is slow
            if (!t.isUpload && (t.sequential || t.firstLastFirst))   // [dl-1] the download order
                tip += QStringLiteral("\n\n") + orderTip(t);
            return tip;
        }
        return {};
    case SortRole:
        switch (col) {
        case DFile: return t.name.toLower();
        case DBytes: return t.size;
        case DStatus: return int(t.state);
        case DRate: return t.rateDownBps;
        case DTimeLeft: return t.etaSec;
        default: return {};
        }
    default:
        return {};
    }
}

QVariant TransferModel::sourceData(const TransferInfo &t, const TransferSource &s, int col, int role) const
{
    Q_UNUSED(t);
    switch (role) {
    case Qt::DisplayRole:
        switch (col) {
        case DUser: return s.user.name.isEmpty() ? (s.user.id.isEmpty() ? netName(s.net) : s.user.id) : s.user.name;
        case DSpeed:
            if (fmt::exactSizes())
                return s.connection.isEmpty() ? QString() : search::connectionText(s.connection);
            return sourceNetworkText(s);
        case DBytes: return s.bytes > 0 ? fmt::size(s.bytes) : QString();
        case DStatus:
            if (!s.statusText.isEmpty())
                return classicSourceStatus(s.statusText);
            return s.rateBps > 0 ? tr("Transferring") : QString();
        case DRate: return s.rateBps > 0 ? fmt::rate(s.rateBps) : QString();
        default: return {};
        }
    case Qt::ForegroundRole:
        if (col == DSpeed) {
            if (!fmt::exactSizes() && !isConnectionType(s.connection))
                return color(keys::colors::NetworkLabel);
            return search::speedColor(search::connectionRank(s.connection) < 0 ? search::kBroadbandRank : search::connectionRank(s.connection));
        }
        return sourceStatusColor(s);
    case Qt::TextAlignmentRole:
        return alignFor(col);
    case Qt::ToolTipRole:
        if (col == DUser)
            return s.client.isEmpty() ? netName(s.net) : QStringLiteral("%1 (%2)").arg(netName(s.net), s.client);
        if (col == DStatus)   // [ui-e] the full text of a status the column cuts off
            return s.statusText.isEmpty() ? QVariant() : QVariant(classicSourceStatus(s.statusText));
        if (col == DBytes && s.bytes > 0)
            return fmt::exactBytes(s.bytes);
        return {};
    case SortRole:
        switch (col) {
        case DUser: return s.user.name.toLower();
        case DBytes: return s.bytes;
        case DRate: return s.rateBps;
        default: return {};
        }
    default:
        return {};
    }
}

QVariant TransferModel::uploadData(const TransferInfo &t, int col, int role) const
{
    const TransferSource s = t.sources.value(0);
    const QString conn = s.connection;
    const QColor c = statusColor(t.statusText, t.state, true);
    switch (role) {
    case Qt::DisplayRole:
        switch (col) {
        case UFile: return t.name;
        case UUser: return t.peer.name.isEmpty() ? s.user.name : t.peer.name;
        case USpeed:
            if (!fmt::exactSizes()) {
                const Net n = t.peer.net != Net::Unknown ? t.peer.net : s.net;
                const QString net = search::netLabel(n);
                if (isConnectionType(conn) && conn != QLatin1String("Unknown"))
                    return net.isEmpty() ? conn : net + QStringLiteral(", ") + conn;
                if (!net.isEmpty())
                    return net;
            }
            return conn.isEmpty() ? tr("Unknown") : conn;
        case UBytes: return fmt::sizeOf(t.done, t.size);
        case UStatus: return displayStatus(t);
        case URate: return t.state == TransferState::Transferring ? fmt::rate(t.rateUpBps) : QString();
        case UTimeLeft: return t.state == TransferState::Transferring ? fmt::etaText(t.etaSec) : QString();
        default: return {};
        }
    case ProgressRole:
        if (col == UStatus && plainTransferring(t))
            return fmt::percentInt(t.done, t.size);
        return {};
    case Qt::ForegroundRole:
        if (col == USpeed)
            return search::speedColor(search::connectionRank(conn) < 0 ? search::kBroadbandRank : search::connectionRank(conn));
        return c;
    case Qt::TextAlignmentRole:
        return alignFor(col);
    case Qt::ToolTipRole:
        if (col == UBytes)
            return tr("%1 of %2").arg(fmt::exactBytes(t.done), fmt::exactBytes(t.size));
        if (col == UStatus)
            return displayStatus(t);
        return {};
    case SortRole:
        switch (col) {
        case UFile: return t.name.toLower();
        case UUser: return t.peer.name.toLower();
        case UBytes: return t.size;
        case UStatus: return int(t.state);
        case URate: return t.rateUpBps;
        case UTimeLeft: return t.etaSec;
        default: return {};
        }
    default:
        return {};
    }
}

} // namespace mx::ui

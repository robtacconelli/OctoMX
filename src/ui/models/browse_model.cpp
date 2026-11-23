// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/models/browse_model.h"

#include <algorithm>

#include "ui/kit/colors.h"
#include "ui/kit/format.h"
#include "ui/kit/icons.h"
#include "ui/models/roles.h"
#include "ui/models/search_model.h"

namespace mx::ui {

namespace {
int lastSep(const QString &p)
{
    return int(qMax(p.lastIndexOf(QLatin1Char('\\')), p.lastIndexOf(QLatin1Char('/'))));
}
} // namespace

BrowseModel::BrowseModel(QObject *parent) : QAbstractItemModel(parent)
{
    connect(Colors::instance(), &Colors::changed, this, [this] {
        const int n = rowCount();
        if (n)
            emit dataChanged(index(0, 0), index(n - 1, ColumnCount - 1));
    });
}

QString BrowseModel::fileNameOf(const QString &p)
{
    return p.mid(lastSep(p) + 1);
}

QString BrowseModel::dirOf(const QString &p)
{
    const int s = lastSep(p);
    return s < 0 ? QString() : p.left(s + 1);
}

void BrowseModel::clear()
{
    beginResetModel();
    m_entries.clear();
    m_local.clear();
    m_folders.clear();
    m_total = 0;
    endResetModel();
}

void BrowseModel::append(const QList<BrowseEntry> &entries)
{
    beginResetModel();
    for (const BrowseEntry &e : entries) {
        m_entries << e;
        m_local << (m_matcher ? m_matcher(e) : NoMatch);
        m_total += e.size;
    }
    rebuild();
    endResetModel();
}

void BrowseModel::setShowFullPaths(bool on)
{
    if (on == m_fullPaths)
        return;
    beginResetModel();
    m_fullPaths = on;
    rebuild();
    endResetModel();
}

void BrowseModel::refreshLocal()
{
    if (!m_matcher)
        return;
    bool changed = false;
    for (int i = 0; i < m_entries.size(); ++i) {
        const Local l = m_matcher(m_entries.at(i));
        if (l != m_local.at(i)) {
            m_local[i] = l;
            changed = true;
        }
    }
    if (changed) {
        // simplest correct notification for a two-level tree: everything may have changed colour
        emit layoutAboutToBeChanged();
        emit layoutChanged();
    }
}

void BrowseModel::rebuild()
{
    m_folders.clear();
    if (m_fullPaths)
        return;
    QHash<QString, int> rowOf;
    for (int i = 0; i < m_entries.size(); ++i) {
        const QString d = dirOf(m_entries.at(i).path);
        auto it = rowOf.constFind(d.toLower());
        int r;
        if (it == rowOf.constEnd()) {
            r = int(m_folders.size());
            rowOf.insert(d.toLower(), r);
            Folder f;
            f.path = d;
            m_folders << f;
        } else {
            r = it.value();
        }
        m_folders[r].files << i;
    }
    std::sort(m_folders.begin(), m_folders.end(),
              [](const Folder &a, const Folder &b) { return a.path.compare(b.path, Qt::CaseInsensitive) < 0; });
}

// tree encoding: folder rows internalId 0, file rows (folder + 1); flat mode: every row is a file
// with internalId 0 and the row = entry number.
QModelIndex BrowseModel::index(int row, int column, const QModelIndex &parent) const
{
    if (row < 0 || column < 0 || column >= ColumnCount)
        return {};
    if (m_fullPaths) {
        if (parent.isValid() || row >= m_entries.size())
            return {};
        return createIndex(row, column, quintptr(0));
    }
    if (!parent.isValid())
        return row < m_folders.size() ? createIndex(row, column, quintptr(0)) : QModelIndex();
    if (parent.internalId() != 0 || row >= m_folders.at(parent.row()).files.size())
        return {};
    return createIndex(row, column, quintptr(parent.row() + 1));
}

QModelIndex BrowseModel::parent(const QModelIndex &child) const
{
    if (!child.isValid() || m_fullPaths || child.internalId() == 0)
        return {};
    return createIndex(int(child.internalId() - 1), 0, quintptr(0));
}

int BrowseModel::rowCount(const QModelIndex &parent) const
{
    if (m_fullPaths)
        return parent.isValid() ? 0 : int(m_entries.size());
    if (!parent.isValid())
        return int(m_folders.size());
    if (parent.internalId() != 0 || parent.column() != 0)
        return 0;
    return int(m_folders.at(parent.row()).files.size());
}

int BrowseModel::columnCount(const QModelIndex &) const
{
    return ColumnCount;
}

bool BrowseModel::hasChildren(const QModelIndex &parent) const
{
    return rowCount(parent) > 0;
}

bool BrowseModel::isFolderRow(const QModelIndex &i) const
{
    return i.isValid() && !m_fullPaths && i.internalId() == 0;
}

const BrowseEntry *BrowseModel::entryAt(const QModelIndex &i) const
{
    if (!i.isValid())
        return nullptr;
    if (m_fullPaths)
        return i.row() < m_entries.size() ? &m_entries.at(i.row()) : nullptr;
    if (i.internalId() == 0)
        return nullptr;
    return &m_entries.at(m_folders.at(int(i.internalId() - 1)).files.at(i.row()));
}

QList<int> BrowseModel::entriesOf(const QModelIndex &i) const
{
    if (!i.isValid())
        return {};
    if (m_fullPaths)
        return {i.row()};
    if (i.internalId() == 0)
        return m_folders.at(i.row()).files;
    return {m_folders.at(int(i.internalId() - 1)).files.at(i.row())};
}

QVariant BrowseModel::headerData(int section, Qt::Orientation o, int role) const
{
    if (o != Qt::Horizontal)
        return {};
    if (role == Qt::DisplayRole) {
        switch (section) {
        case File: return tr("File");
        case Bytes: return fmt::exactSizes() ? tr("Bytes") : tr("Size");
        case Time: return tr("Time");
        case Bitrate: return tr("Bitrate");
        case Freq: return tr("Freq");
        }
    }
    if (role == Qt::TextAlignmentRole)
        return int((section == File ? Qt::AlignLeft : Qt::AlignRight) | Qt::AlignVCenter);
    return {};
}

QVariant BrowseModel::data(const QModelIndex &idx, int role) const
{
    if (!idx.isValid())
        return {};
    const int col = idx.column();
    if (isFolderRow(idx)) {
        const Folder &f = m_folders.at(idx.row());
        switch (role) {
        case Qt::DisplayRole:
            if (col == File)
                return f.path;
            return {};
        case Qt::DecorationRole:
            return col == File ? QVariant(iconPixmap(IconId::Folder)) : QVariant();
        case Qt::ForegroundRole:
            return color(keys::colors::ListText);
        case SortRole:
            return f.path.toLower();
        case KindRole:
            return 0;
        }
        return {};
    }
    const int n = m_fullPaths ? idx.row() : m_folders.at(int(idx.internalId() - 1)).files.at(idx.row());
    const BrowseEntry &e = m_entries.at(n);
    const Local local = m_local.value(n, NoMatch);
    switch (role) {
    case Qt::DisplayRole:
        switch (col) {
        case File: return m_fullPaths ? e.path : fileNameOf(e.path);
        case Bytes: return fmt::size(e.size);
        case Time: return fmt::duration(e.durationSec);
        case Bitrate: return fmt::bitrate(e.bitrateKbps);
        case Freq: return fmt::frequency(e.sampleRateHz);
        }
        return {};
    case Qt::ForegroundRole: {
        if (col == Bitrate && e.bitrateKbps > 0)
            return search::bitrateColor(e.bitrateKbps);
        switch (local) {
        case MatchesIncomplete: return color(keys::colors::MatchIncomplete);
        case CanJoin: return color(keys::colors::CanJoinDownload);
        case Downloading: return color(keys::colors::CurrentlyDownloading);
        default: return color(keys::colors::ListText);
        }
    }
    case Qt::TextAlignmentRole:
        return int((col == File ? Qt::AlignLeft : Qt::AlignRight) | Qt::AlignVCenter);
    case Qt::ToolTipRole:
        return col == File ? e.path : QVariant();
    case SortRole:
        switch (col) {
        case File: return (m_fullPaths ? e.path : fileNameOf(e.path)).toLower();
        case Bytes: return e.size;
        case Time: return e.durationSec;
        case Bitrate: return e.bitrateKbps;
        case Freq: return e.sampleRateHz;
        }
        return {};
    case KindRole:
        return 1;
    case IdRole:
        return e.path;
    }
    return {};
}

BrowseProxy::BrowseProxy(QObject *parent) : QSortFilterProxyModel(parent)
{
    setSortRole(SortRole);
    setDynamicSortFilter(false);
}

bool BrowseProxy::lessThan(const QModelIndex &a, const QModelIndex &b) const
{
    const QVariant va = a.data(SortRole), vb = b.data(SortRole);
    if (va.typeId() == QMetaType::QString || vb.typeId() == QMetaType::QString)
        return va.toString() < vb.toString();
    return va.toLongLong() < vb.toLongLong();
}

} // namespace mx::ui

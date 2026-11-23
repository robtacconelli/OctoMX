// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/models/shared_model.h"

#include <QtCore/QDir>
#include <algorithm>

#include "ui/kit/colors.h"
#include "ui/kit/format.h"
#include "ui/kit/icons.h"
#include "ui/models/roles.h"
#include "ui/models/search_model.h"

namespace mx::ui {

namespace shared {
QString dirKey(const QString &path)
{
    QString k = QDir::fromNativeSeparators(path);
    while (k.size() > 1 && k.endsWith(QLatin1Char('/')) && !(k.size() == 3 && k.at(1) == QLatin1Char(':')))
        k.chop(1);
    return k;
}

QString displayDir(const QString &key)
{
    QString d = QDir::toNativeSeparators(key);
    if (!d.endsWith(QDir::separator()))
        d += QDir::separator();
    return d;
}
} // namespace shared

using namespace shared;

namespace {
QString parentKey(const QString &filePath)
{
    const QString k = QDir::fromNativeSeparators(filePath);
    const int slash = k.lastIndexOf(QLatin1Char('/'));
    if (slash < 0)
        return QString();
    if (slash == 0)
        return QStringLiteral("/");
    if (slash == 2 && k.at(1) == QLatin1Char(':'))
        return k.left(3);
    return k.left(slash);
}
} // namespace

SharedFilesModel::SharedFilesModel(QObject *parent) : QAbstractItemModel(parent)
{
    connect(Colors::instance(), &Colors::changed, this, [this] {
        if (!m_folders.isEmpty())
            emit dataChanged(index(0, 0), index(int(m_folders.size()) - 1, ColumnCount - 1));
    });
}

void SharedFilesModel::setContent(const QList<SharedFolder> &roots, const QList<SharedFile> &files)
{
    beginResetModel();
    m_files = files;
    m_folders.clear();
    m_rowOfDir.clear();
    m_fileOfPath.clear();
    m_roots.clear();
    m_total = 0;
    m_hashed = 0;

    QHash<QString, int> tmp;
    auto folderFor = [&](const QString &key) -> Folder & {
        auto it = tmp.constFind(key);
        if (it != tmp.constEnd())
            return m_folders[it.value()];
        Folder f;
        f.key = key;
        f.display = displayDir(key);
        tmp.insert(key, int(m_folders.size()));
        m_folders.append(f);
        return m_folders.last();
    };
    for (const SharedFolder &r : roots) {
        const QString k = dirKey(r.path);
        m_roots << k;
        folderFor(k).root = true;
    }
    for (int i = 0; i < m_files.size(); ++i) {
        const SharedFile &f = m_files.at(i);
        Folder &fo = folderFor(parentKey(f.path));
        fo.files << i;
        fo.bytes += f.size;
        m_total += f.size;
        if (f.hashed)
            ++m_hashed;
        m_fileOfPath.insert(f.path, i);
    }
    // folders sorted by path (case-insensitive), files by name inside each folder
    std::sort(m_folders.begin(), m_folders.end(), [](const Folder &a, const Folder &b) {
        const int c = a.key.compare(b.key, Qt::CaseInsensitive);
        return c != 0 ? c < 0 : a.key < b.key;
    });
    for (Folder &fo : m_folders) {
        std::sort(fo.files.begin(), fo.files.end(), [this](int a, int b) {
            return m_files.at(a).name.compare(m_files.at(b).name, Qt::CaseInsensitive) < 0;
        });
    }
    for (int i = 0; i < m_folders.size(); ++i)
        m_rowOfDir.insert(m_folders.at(i).key, i);
    endResetModel();
}

QModelIndex SharedFilesModel::index(int row, int column, const QModelIndex &parent) const
{
    if (column < 0 || column >= ColumnCount || row < 0)
        return {};
    if (!parent.isValid())
        return row < m_folders.size() ? createIndex(row, column, quintptr(0)) : QModelIndex();
    if (parent.internalId() != 0 || parent.row() >= m_folders.size())
        return {};
    if (row >= m_folders.at(parent.row()).files.size())
        return {};
    return createIndex(row, column, quintptr(parent.row() + 1));
}

QModelIndex SharedFilesModel::parent(const QModelIndex &child) const
{
    if (!child.isValid() || child.internalId() == 0)
        return {};
    return createIndex(int(child.internalId() - 1), 0, quintptr(0));
}

int SharedFilesModel::rowCount(const QModelIndex &parent) const
{
    if (!parent.isValid())
        return int(m_folders.size());
    if (parent.internalId() != 0 || parent.column() != 0)
        return 0;
    return int(m_folders.at(parent.row()).files.size());
}

int SharedFilesModel::columnCount(const QModelIndex &) const
{
    return ColumnCount;
}

bool SharedFilesModel::hasChildren(const QModelIndex &parent) const
{
    if (!parent.isValid())
        return !m_folders.isEmpty();
    if (parent.internalId() != 0)
        return false;
    return !m_folders.at(parent.row()).files.isEmpty();
}

QVariant SharedFilesModel::headerData(int section, Qt::Orientation o, int role) const
{
    if (o != Qt::Horizontal)
        return {};
    if (role == Qt::DisplayRole) {
        switch (section) {
        case Path: return tr("File Path");
        case Size: return tr("Size");
        case Bitrate: return tr("Bitrate");
        case Time: return tr("Time");
        case Freq: return tr("Freq");
        }
    }
    if (role == Qt::TextAlignmentRole)
        return int((section == Path ? Qt::AlignLeft : Qt::AlignRight) | Qt::AlignVCenter);
    return {};
}

QVariant SharedFilesModel::data(const QModelIndex &idx, int role) const
{
    if (!idx.isValid())
        return {};
    const int col = idx.column();
    if (idx.internalId() == 0) {
        const Folder &fo = m_folders.at(idx.row());
        switch (role) {
        case Qt::DisplayRole:
            return col == Path ? fo.display : QVariant();
        case Qt::DecorationRole:
            return col == Path ? QVariant(iconPixmap(IconId::Folder)) : QVariant();
        case Qt::ForegroundRole:
            return color(keys::colors::ListText);
        case Qt::ToolTipRole:
            if (col != Path)
                return QVariant();
            // [qa-2] "%n file(s)" showed literally as "3 file(s)" (no translator loaded)
            return (fo.files.size() == 1 ? tr("1 file, %1") : tr("%1 files, %2").arg(fmt::number(fo.files.size())))
                .arg(fmt::exactSizes() ? fmt::exactBytes(fo.bytes) : fmt::size(fo.bytes));
        case SortRole:
            if (col == Size)
                return fo.bytes;
            return fo.key.toLower();
        case IdRole:
            return fo.key;
        case KindRole:
            return 0;
        }
        return {};
    }
    const Folder &fo = m_folders.at(int(idx.internalId() - 1));
    const SharedFile &f = m_files.at(fo.files.at(idx.row()));
    switch (role) {
    case Qt::DisplayRole:
        switch (col) {
        case Path: return f.name;
        case Size: return fmt::size(f.size);
        case Bitrate: return fmt::bitrate(f.bitrateKbps);
        case Time: return fmt::duration(f.durationSec);
        case Freq: return fmt::frequency(f.sampleRateHz);
        }
        return {};
    case Qt::ForegroundRole:
        if (col == Bitrate && f.bitrateKbps > 0 && f.hashed)
            return search::bitrateColor(f.bitrateKbps);
        return f.hashed ? color(keys::colors::ListText) : color(ckeys::ListInactive);
    case Qt::TextAlignmentRole:
        return int((col == Path ? Qt::AlignLeft : Qt::AlignRight) | Qt::AlignVCenter);
    case Qt::ToolTipRole:
        return col == Path ? QDir::toNativeSeparators(f.path) : QVariant();
    case SortRole:
        switch (col) {
        case Path: return f.name.toLower();
        case Size: return f.size;
        case Bitrate: return f.bitrateKbps;
        case Time: return f.durationSec;
        case Freq: return f.sampleRateHz;
        }
        return {};
    case IdRole:
        return f.path;
    case KindRole:
        return 1;
    }
    return {};
}

QString SharedFilesModel::folderKey(const QModelIndex &i) const
{
    if (!i.isValid())
        return {};
    if (i.internalId() == 0)
        return m_folders.at(i.row()).key;
    return m_folders.at(int(i.internalId() - 1)).key;
}

bool SharedFilesModel::isSharedRoot(const QModelIndex &i) const
{
    return isFolderRow(i) && m_folders.at(i.row()).root;
}

const SharedFile *SharedFilesModel::fileAt(const QModelIndex &i) const
{
    if (!i.isValid() || i.internalId() == 0)
        return nullptr;
    const Folder &fo = m_folders.at(int(i.internalId() - 1));
    return &m_files.at(fo.files.at(i.row()));
}

QModelIndex SharedFilesModel::folderIndex(const QString &key) const
{
    const int r = m_rowOfDir.value(dirKey(key), -1);
    return r < 0 ? QModelIndex() : index(r, 0);
}

QModelIndex SharedFilesModel::fileIndex(const QString &path) const
{
    const int fi = m_fileOfPath.value(path, -1);
    if (fi < 0)
        return {};
    const int r = m_rowOfDir.value(parentKey(path), -1);
    if (r < 0)
        return {};
    const int pos = int(m_folders.at(r).files.indexOf(fi));
    return pos < 0 ? QModelIndex() : index(pos, 0, index(r, 0));
}

QString SharedFilesModel::rootOf(const QString &key) const
{
    QString best;
    for (const QString &r : m_roots) {
        if (key == r || key.startsWith(r.endsWith(QLatin1Char('/')) ? r : r + QLatin1Char('/')))
            if (r.size() > best.size())
                best = r;
    }
    return best;
}

QStringList SharedFilesModel::filesInFolder(const QString &key) const
{
    QStringList l;
    const int r = m_rowOfDir.value(dirKey(key), -1);
    if (r < 0)
        return l;
    for (int fi : m_folders.at(r).files)
        l << m_files.at(fi).path;
    return l;
}

// ------------------------------------------------------------------------------------ SharedProxy

SharedProxy::SharedProxy(QObject *parent) : QSortFilterProxyModel(parent)
{
    setSortRole(SortRole);
    setDynamicSortFilter(false);
}

bool SharedProxy::lessThan(const QModelIndex &a, const QModelIndex &b) const
{
    const QVariant va = a.data(SortRole), vb = b.data(SortRole);
    if (va.typeId() == QMetaType::QString || vb.typeId() == QMetaType::QString)
        return va.toString() < vb.toString();
    return va.toLongLong() < vb.toLongLong();
}

} // namespace mx::ui

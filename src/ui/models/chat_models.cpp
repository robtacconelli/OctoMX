// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/models/chat_models.h"

#include "ui/kit/colors.h"
#include "ui/kit/format.h"
#include "ui/models/roles.h"
#include "ui/models/search_model.h"

namespace mx::ui {

namespace {
bool variantLess(const QVariant &va, const QVariant &vb)
{
    if (va.typeId() == QMetaType::QString || vb.typeId() == QMetaType::QString)
        return va.toString().compare(vb.toString(), Qt::CaseInsensitive) < 0;
    return va.toLongLong() < vb.toLongLong();
}
} // namespace

// ---------------------------------------------------------------------------------- ChannelModel

ChannelModel::ChannelModel(QObject *parent) : QAbstractTableModel(parent)
{
    connect(Colors::instance(), &Colors::changed, this, [this] {
        if (!m_rooms.isEmpty())
            emit dataChanged(index(0, 0), index(rowCount() - 1, ColumnCount - 1));
    });
}

int ChannelModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rooms.size());
}

int ChannelModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

void ChannelModel::setRooms(const QList<ChatRoomInfo> &rooms)
{
    beginResetModel();
    m_rooms = rooms;
    endResetModel();
}

QVariant ChannelModel::headerData(int section, Qt::Orientation o, int role) const
{
    if (o != Qt::Horizontal)
        return {};
    if (role == Qt::DisplayRole) {
        switch (section) {
        case Channel: return tr("Channel");
        case Users: return tr("Users");
        case Topic: return tr("Topic");
        }
    }
    if (role == Qt::TextAlignmentRole)
        return int((section == Users ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter);
    return {};
}

QVariant ChannelModel::data(const QModelIndex &idx, int role) const
{
    if (!idx.isValid() || idx.row() >= m_rooms.size())
        return {};
    const ChatRoomInfo &r = m_rooms.at(idx.row());
    switch (role) {
    case Qt::DisplayRole:
        switch (idx.column()) {
        case Channel: return r.name;
        case Users: return r.limit > 0 ? QStringLiteral("%1 / %2").arg(r.users).arg(r.limit) : QString::number(r.users);
        case Topic: return r.topic;
        }
        return {};
    case Qt::ForegroundRole:
        switch (idx.column()) {
        case Channel: return color(keys::colors::ChannelName);
        case Users: return color(keys::colors::ChannelUsers);
        default: return color(keys::colors::ChannelTopic);
        }
    case Qt::TextAlignmentRole:
        return int((idx.column() == Users ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter);
    case Qt::ToolTipRole:
        return idx.column() == Topic ? r.topic : QVariant();
    case SortRole:
        switch (idx.column()) {
        case Channel: return r.name;
        case Users: return qint64(r.users);
        default: return r.topic;
        }
    case IdRole:
        return r.id;
    }
    return {};
}

ChannelProxy::ChannelProxy(QObject *parent) : QSortFilterProxyModel(parent)
{
    setSortRole(SortRole);
    setDynamicSortFilter(true);
}

void ChannelProxy::setFilter(const QString &text, bool enabled, bool includeTopic)
{
    if (text == m_text && enabled == m_enabled && includeTopic == m_topic)
        return;
    beginFilterChange();
    m_text = text;
    m_enabled = enabled;
    m_topic = includeTopic;
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
}

bool ChannelProxy::filterAcceptsRow(int row, const QModelIndex &parent) const
{
    if (!m_enabled || m_text.isEmpty())
        return true;
    const auto *m = static_cast<const ChannelModel *>(sourceModel());
    const ChatRoomInfo &r = m->room(row);
    Q_UNUSED(parent);
    if (r.name.contains(m_text, Qt::CaseInsensitive))
        return true;
    return m_topic && r.topic.contains(m_text, Qt::CaseInsensitive);
}

bool ChannelProxy::lessThan(const QModelIndex &a, const QModelIndex &b) const
{
    return variantLess(a.data(SortRole), b.data(SortRole));
}

// --------------------------------------------------------------------------------- ChatUserModel

ChatUserModel::ChatUserModel(QObject *parent) : QAbstractTableModel(parent)
{
    connect(Colors::instance(), &Colors::changed, this, [this] {
        if (!m_users.isEmpty())
            emit dataChanged(index(0, 0), index(rowCount() - 1, ColumnCount - 1));
    });
}

int ChatUserModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_users.size());
}

int ChatUserModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

void ChatUserModel::setUsers(const QList<ChatUser> &users)
{
    beginResetModel();
    m_users = users;
    endResetModel();
}

int ChatUserModel::rowOfKey(const QString &key) const
{
    for (int i = 0; i < m_users.size(); ++i)
        if (m_users.at(i).user.key() == key)
            return i;
    return -1;
}

QString ChatUserModel::displayName(const ChatUser &u)
{
    const QString n = u.user.name.isEmpty() ? u.user.id : u.user.name;
    if (u.rank >= 2)
        return QLatin1Char('@') + n;
    if (u.rank == 1)
        return QLatin1Char('+') + n;
    return n;
}

QVariant ChatUserModel::headerData(int section, Qt::Orientation o, int role) const
{
    if (o != Qt::Horizontal)
        return {};
    if (role == Qt::DisplayRole) {
        switch (section) {
        case User: return tr("User");
        case Files: return tr("Files");
        case Speed: return tr("Speed");
        }
    }
    if (role == Qt::TextAlignmentRole)
        return int(Qt::AlignLeft | Qt::AlignVCenter);   // [ui-d] Files is left-aligned in chat_room_window.png
    return {};
}

QVariant ChatUserModel::data(const QModelIndex &idx, int role) const
{
    if (!idx.isValid() || idx.row() >= m_users.size())
        return {};
    const ChatUser &u = m_users.at(idx.row());
    switch (role) {
    case Qt::DisplayRole:
        switch (idx.column()) {
        case User: return displayName(u);
        case Files: return QString::number(u.files);
        case Speed: return search::connectionText(u.connection);   // [qa-1] sanitised / truncated
        }
        return {};
    case Qt::ForegroundRole:
        if (idx.column() == Speed)
            return search::speedColor(qMax(0, search::connectionRank(u.connection)));
        // [ui-h] Settings > Appearance > Channel Window: room administrators (@) and voiced users (+)
        if (idx.column() == User && u.rank >= 2)
            return color(keys::colors::ChatUserAdmin);
        if (idx.column() == User && u.rank == 1)
            return color(keys::colors::ChatUserVoiced);
        return color(keys::colors::ChatUserListText);
    case Qt::TextAlignmentRole:
        return int(Qt::AlignLeft | Qt::AlignVCenter);
    case ClipRole:
        return idx.column() == Speed;
    case SortRole:
        switch (idx.column()) {
        case User: return QStringLiteral("%1%2").arg(9 - qBound(0, u.rank, 9)).arg((u.user.name.isEmpty() ? u.user.id : u.user.name).toLower());
        case Files: return qint64(u.files);
        case Speed: return qint64(qMax(0, search::connectionRank(u.connection)));
        }
        return {};
    case IdRole:
        return u.user.key();
    case Qt::ToolTipRole:
        if (idx.column() == User)
            return u.user.address.isEmpty() ? netName(u.user.net) : QStringLiteral("%1 %2").arg(netName(u.user.net), u.user.address);
        return {};
    }
    return {};
}

ChatUserProxy::ChatUserProxy(QObject *parent) : QSortFilterProxyModel(parent)
{
    setSortRole(SortRole);
    setDynamicSortFilter(true);
}

bool ChatUserProxy::lessThan(const QModelIndex &a, const QModelIndex &b) const
{
    return variantLess(a.data(SortRole), b.data(SortRole));
}

// ---------------------------------------------------------------------------------- IgnoredModel

IgnoredModel::IgnoredModel(QObject *parent) : QAbstractListModel(parent)
{
    connect(Colors::instance(), &Colors::changed, this, [this] {
        if (!m_users.isEmpty())
            emit dataChanged(index(0), index(rowCount() - 1));
    });
}

int IgnoredModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_users.size());
}

void IgnoredModel::setUsers(const QList<UserRef> &users)
{
    beginResetModel();
    m_users = users;
    endResetModel();
}

QString IgnoredModel::entryText(const UserRef &u)
{
    const QString name = u.name.isEmpty() ? u.id : u.name;
    if (u.net == Net::Overlay)
        return u.id.isEmpty() || u.id == u.name ? QStringLiteral("%1 OctoMX").arg(name)
                                               : QStringLiteral("%1 OctoMX %2").arg(name, u.id.left(12));
    const QString where = u.address.isEmpty() ? (u.id == name ? QString() : u.id) : u.address;
    return where.isEmpty() ? QStringLiteral("%1 [%2]").arg(name, netName(u.net))
                           : QStringLiteral("%1 [%2] %3").arg(name, netName(u.net), where);
}

QVariant IgnoredModel::data(const QModelIndex &idx, int role) const
{
    if (!idx.isValid() || idx.row() >= m_users.size())
        return {};
    switch (role) {
    case Qt::DisplayRole: return entryText(m_users.at(idx.row()));
    case Qt::ForegroundRole: return color(keys::colors::IgnoredText);
    case IdRole: return m_users.at(idx.row()).key();
    }
    return {};
}

} // namespace mx::ui

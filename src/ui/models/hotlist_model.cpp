// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/models/hotlist_model.h"

#include "mx/services.h"
#include "ui/kit/colors.h"
#include "ui/models/roles.h"

namespace mx::ui {

HotlistModel::HotlistModel(mx::UserService *users, mx::ChatService *chat, QObject *parent)
    : QAbstractListModel(parent), m_users(users)
{
    connect(users, &UserService::hotlistChanged, this, &HotlistModel::reload);
    if (chat) {
        connect(chat, &ChatService::messageReceived, this, [this](const ChatMessage &m) {
            if (m.kind != ChatKind::Private)
                return;
            // the partner: the recipient of our own echo, else the sender
            const UserRef partner = m.to.isValid() ? m.to : m.from;
            if (partner.isValid() && partner.name != m_ownName)
                addTemporary(partner);
        });
    }
    connect(Colors::instance(), &Colors::changed, this, [this] {
        if (!m_rows.isEmpty())
            emit dataChanged(index(0), index(int(m_rows.size()) - 1));
    });
    reload();
}

QString HotlistModel::entryText(const UserRef &u, const QString &note)
{
    const QString name = note.isEmpty() ? u.name : note;
    if (!u.address.isEmpty() && u.net != Net::Overlay)
        return QStringLiteral("%1 [%2] %3").arg(name, netName(u.net), u.address);
    return QStringLiteral("%1 %2").arg(name, netName(u.net));
}

void HotlistModel::addTemporary(const UserRef &u)
{
    for (const UserRef &t : std::as_const(m_temporary))
        if (t.key() == u.key())
            return;
    for (const HotlistEntry &e : m_users->hotlist())
        if (e.user.key() == u.key())
            return;
    m_temporary << u;
    reload();
}

void HotlistModel::removeTemporary(const UserRef &u)
{
    for (int i = 0; i < m_temporary.size(); ++i)
        if (m_temporary[i].key() == u.key()) {
            m_temporary.removeAt(i);
            reload();
            return;
        }
}

void HotlistModel::reload()
{
    const QList<HotlistEntry> hl = m_users->hotlist();
    // temporary users that became permanent leave the temporary section
    for (int i = int(m_temporary.size()) - 1; i >= 0; --i)
        for (const HotlistEntry &e : hl)
            if (e.user.key() == m_temporary[i].key()) {
                m_temporary.removeAt(i);
                break;
            }
    QList<Row> rows;
    if (!m_temporary.isEmpty()) {
        rows << Row{SectionRow, tr("Temporary"), {}};
        for (const UserRef &u : std::as_const(m_temporary))
            rows << Row{TemporaryRow, entryText(u), u};
    }
    QList<Row> on, off;
    for (const HotlistEntry &e : hl)
        (e.online ? on : off) << Row{e.online ? OnlineRow : OfflineRow, entryText(e.user, e.note), e.user};
    if (!on.isEmpty()) {
        rows << Row{SectionRow, tr("Online"), {}};
        rows << on;
    }
    if (!off.isEmpty()) {
        rows << Row{SectionRow, tr("Offline"), {}};
        rows << off;
    }
    // Small list: a reset is fine here (and keeps section rows consistent).
    beginResetModel();
    m_rows = rows;
    endResetModel();
}

int HotlistModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

HotlistModel::Kind HotlistModel::kind(int row) const
{
    return row >= 0 && row < m_rows.size() ? m_rows.at(row).kind : SectionRow;
}

UserRef HotlistModel::user(int row) const
{
    return row >= 0 && row < m_rows.size() ? m_rows.at(row).user : UserRef();
}

Qt::ItemFlags HotlistModel::flags(const QModelIndex &index) const
{
    if (kind(index.row()) == SectionRow)
        return Qt::ItemIsEnabled;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

QVariant HotlistModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size())
        return {};
    const Row &r = m_rows.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
        return r.text;
    case SectionHeaderRole:
        return r.kind == SectionRow;
    case KindRole:
        return int(r.kind);
    case IdRole:
        return r.user.key();
    case Qt::ForegroundRole:
        switch (r.kind) {
        case TemporaryRow: return color(ckeys::HotlistTemporary);
        case OnlineRow: return color(keys::colors::UserOnline);
        case OfflineRow: return color(keys::colors::UserOffline);
        default: return color(ckeys::HotlistHeader);
        }
    default:
        return {};
    }
}

} // namespace mx::ui

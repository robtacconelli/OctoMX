// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/models/server_model.h"

#include "mx/services.h"
#include "ui/kit/colors.h"
#include "ui/kit/format.h"
#include "ui/models/roles.h"

namespace mx::ui {

// internalId: 0 = network row, n+1 = child of network row n

ServerModel::ServerModel(mx::NetworkService *net, QObject *parent) : QAbstractItemModel(parent), m_net(net)
{
    connect(net, &NetworkService::networkChanged, this, &ServerModel::updateNetwork);
    connect(net, &NetworkService::serversChanged, this, &ServerModel::reloadServers);
    connect(Colors::instance(), &Colors::changed, this, [this] {
        if (rowCount())
            emit layoutChanged();
    });
    reloadAll();
}

void ServerModel::reloadAll()
{
    beginResetModel();
    m_nets = m_net->networks();
    m_servers.clear();
    for (const NetworkStatus &s : std::as_const(m_nets))
        m_servers << m_net->servers(s.net);
    endResetModel();
}

void ServerModel::reloadServers(Net n)
{
    const QModelIndex pi = indexOfNet(n);
    if (!pi.isValid()) {
        reloadAll();
        return;
    }
    const int row = pi.row();
    const QList<ServerEntry> fresh = m_net->servers(n);
    QList<ServerEntry> &cur = m_servers[row];
    // update in place when the set of ids is unchanged (keeps selection), else replace
    bool sameIds = cur.size() == fresh.size();
    for (int i = 0; sameIds && i < cur.size(); ++i)
        sameIds = cur[i].id == fresh[i].id;
    if (sameIds) {
        cur = fresh;
        if (!cur.isEmpty())
            emit dataChanged(index(0, 0, pi), index(int(cur.size()) - 1, 0, pi));
        return;
    }
    if (!cur.isEmpty()) {
        beginRemoveRows(pi, 0, int(cur.size()) - 1);
        cur.clear();
        endRemoveRows();
    }
    if (!fresh.isEmpty()) {
        beginInsertRows(pi, 0, int(fresh.size()) - 1);
        cur = fresh;
        endInsertRows();
    }
}

void ServerModel::updateNetwork(const NetworkStatus &st)
{
    for (int i = 0; i < m_nets.size(); ++i)
        if (m_nets[i].net == st.net) {
            m_nets[i] = st;
            const QModelIndex idx = index(i, 0);
            emit dataChanged(idx, idx);
            return;
        }
    reloadAll();
}

QModelIndex ServerModel::index(int row, int column, const QModelIndex &parent) const
{
    if (column != 0 || row < 0)
        return {};
    if (!parent.isValid())
        return row < m_nets.size() ? createIndex(row, 0, quintptr(0)) : QModelIndex();
    if (parent.internalId() != 0)
        return {};
    const int p = parent.row();
    if (p >= m_servers.size() || row >= m_servers[p].size())
        return {};
    return createIndex(row, 0, quintptr(p + 1));
}

QModelIndex ServerModel::parent(const QModelIndex &child) const
{
    if (!child.isValid() || child.internalId() == 0)
        return {};
    return createIndex(int(child.internalId()) - 1, 0, quintptr(0));
}

int ServerModel::rowCount(const QModelIndex &parent) const
{
    if (!parent.isValid())
        return int(m_nets.size());
    if (parent.internalId() != 0 || parent.row() >= m_servers.size())
        return 0;
    return int(m_servers[parent.row()].size());
}

Net ServerModel::netAt(const QModelIndex &i) const
{
    if (!i.isValid())
        return Net::Unknown;
    const int row = i.internalId() == 0 ? i.row() : int(i.internalId()) - 1;
    return row < m_nets.size() ? m_nets[row].net : Net::Unknown;
}

ServerEntry ServerModel::serverAt(const QModelIndex &i) const
{
    if (!i.isValid() || i.internalId() == 0)
        return ServerEntry();
    const int p = int(i.internalId()) - 1;
    if (p >= m_servers.size() || i.row() >= m_servers[p].size())
        return ServerEntry();
    return m_servers[p][i.row()];
}

QModelIndex ServerModel::indexOfNet(Net n) const
{
    for (int i = 0; i < m_nets.size(); ++i)
        if (m_nets[i].net == n)
            return index(i, 0);
    return {};
}

QString ServerModel::serverStatusText(const ServerEntry &e)
{
    switch (e.state) {
    case ConnState::Connected: {
        QStringList parts;
        // [ui-e] DHT bootstrap rows (BitTorrent, Kad, rats) count nodes, not users (qa-1 finding)
        const bool dht = e.net == Net::BitTorrent || e.net == Net::Kad || e.net == Net::Rats;
        // The core reports DHT rows in ServerEntry::nodes; an older plugin may still put the count in users.
        const qint64 nodes = e.nodes >= 0 ? e.nodes : (dht ? e.users : -1);
        const qint64 users = dht ? -1 : e.users;
        if (nodes >= 0)
            parts << (nodes == 1 ? QObject::tr("1 node") : QObject::tr("%1 nodes").arg(fmt::number(nodes)));
        if (users >= 0)
            parts << (users == 1 ? QObject::tr("1 user") : QObject::tr("%1 users").arg(fmt::number(users)));   // [qa-2] not "1 users"
        if (e.files >= 0)
            parts << (e.files == 1 ? QObject::tr("1 file") : QObject::tr("%1 files").arg(fmt::number(e.files)));
        return parts.isEmpty() ? QObject::tr("Ready") : QObject::tr("Ready (%1)").arg(parts.join(QStringLiteral(", ")));
    }
    case ConnState::Connecting:
        return QObject::tr("Connecting...");
    case ConnState::Error:
        return e.description.isEmpty() ? QObject::tr("Connection refused") : e.description;
    case ConnState::Disabled:
        return QObject::tr("Disabled");
    default:
        return e.description;
    }
}

QString ServerModel::networkRowText(const NetworkStatus &s, bool enabled)
{
    QString status = s.statusText;
    status.replace(QStringLiteral(" \u00B7 "), QStringLiteral(", "));   // [ui-e] the original WinMX wording uses commas
    if (!enabled)
        status = QObject::tr("Disabled");
    else if (status.isEmpty())
        status = s.state == ConnState::Connected ? QObject::tr("Ready")
               : s.state == ConnState::Connecting ? QObject::tr("Connecting...")
                                                  : QObject::tr("Not connected");
    return QStringLiteral("[%1]  %2").arg(netName(s.net), status);
}

static QColor stateColor(ConnState s)
{
    switch (s) {
    case ConnState::Connected: return color(keys::colors::ServerConnected);
    case ConnState::Connecting: return color(keys::colors::ServerConnecting);
    case ConnState::Disabled: return color(ckeys::ListInactive);
    default: return color(keys::colors::ServerDisconnected);
    }
}

QVariant ServerModel::data(const QModelIndex &i, int role) const
{
    if (!i.isValid())
        return {};
    if (i.internalId() == 0) {
        if (i.row() >= m_nets.size())
            return {};
        const NetworkStatus &s = m_nets[i.row()];
        const bool enabled = s.state != ConnState::Disabled;
        switch (role) {
        case Qt::DisplayRole: return networkRowText(s, enabled);
        case Qt::ForegroundRole: return stateColor(s.state);
        case IdRole: return netName(s.net);
        case KindRole: return 0;
        default: return {};
        }
    }
    const ServerEntry e = serverAt(i);
    switch (role) {
    case Qt::DisplayRole: {
        const QString name = e.name.isEmpty() ? netName(e.net) : e.name;
        return QStringLiteral("[%1]  %2  %3").arg(name, e.address, serverStatusText(e));
    }
    case Qt::ForegroundRole: return stateColor(e.state);
    case IdRole: return e.id;
    case KindRole: return 1;
    default: return {};
    }
}

} // namespace mx::ui

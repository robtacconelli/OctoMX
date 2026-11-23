// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: model of the "Other Protocol Networks" list (UI_SPEC §3.1.2 / §8.2): one top-level
// row per network ("[eD2k]  Ready (512341 users, ...)"), its servers / bootstrap nodes as children
// ("[eMule Security No1]  45.82.80.155:5687  Ready (...)"). Rows coloured by connection state.
#pragma once

#include <QtCore/QAbstractItemModel>

#include "mx/types.h"

namespace mx {
class NetworkService;
}

namespace mx::ui {

class ServerModel : public QAbstractItemModel {
    Q_OBJECT
public:
    explicit ServerModel(mx::NetworkService *net, QObject *parent = nullptr);

    QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override { Q_UNUSED(parent); return 1; }
    QVariant data(const QModelIndex &index, int role) const override;

    bool isNetworkRow(const QModelIndex &i) const { return i.isValid() && i.internalId() == 0; }
    mx::Net netAt(const QModelIndex &i) const;
    mx::ServerEntry serverAt(const QModelIndex &i) const;   // invalid net for network rows
    QModelIndex indexOfNet(mx::Net n) const;

    static QString serverStatusText(const mx::ServerEntry &e);
    static QString networkRowText(const mx::NetworkStatus &s, bool enabled);

public slots:
    void reloadAll();

private:
    void reloadServers(mx::Net n);
    void updateNetwork(const mx::NetworkStatus &st);
    mx::NetworkService *m_net;
    QList<mx::NetworkStatus> m_nets;
    QList<QList<mx::ServerEntry>> m_servers;   // parallel to m_nets
};

} // namespace mx::ui

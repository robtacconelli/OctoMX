// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: models of the Chat page (UI_SPEC §3.3):
//   ChannelModel      the channel list: Channel / Users ("23 / 90") / Topic, arrival order
//   ChannelProxy      Filter button + edit (name, and topic when chat/topicInFilter), header sort
//   ChatUserModel     a room's user list: User ("@op", "+voice") / Files / Speed (§2.4 colours)
//   ChatUserProxy     ops first, then voiced, then alphabetical (column 0); other columns by value
//   IgnoredModel      the Ignored Users window list (§5.1), "#FFFFCC" entries
#pragma once

#include <QtCore/QAbstractListModel>
#include <QtCore/QAbstractTableModel>
#include <QtCore/QSortFilterProxyModel>

#include "mx/types.h"

namespace mx::ui {

class ChannelModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { Channel = 0, Users, Topic, ColumnCount };
    explicit ChannelModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation o, int role) const override;

    void setRooms(const QList<mx::ChatRoomInfo> &rooms);
    const mx::ChatRoomInfo &room(int row) const { return m_rooms.at(row); }
    int roomCount() const { return int(m_rooms.size()); }

private:
    QList<mx::ChatRoomInfo> m_rooms;
};

class ChannelProxy : public QSortFilterProxyModel {
    Q_OBJECT
public:
    explicit ChannelProxy(QObject *parent = nullptr);
    void setFilter(const QString &text, bool enabled, bool includeTopic);

protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override;
    bool lessThan(const QModelIndex &a, const QModelIndex &b) const override;

private:
    QString m_text;
    bool m_enabled = false;
    bool m_topic = false;
};

class ChatUserModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { User = 0, Files, Speed, ColumnCount };
    explicit ChatUserModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation o, int role) const override;

    void setUsers(const QList<mx::ChatUser> &users);
    const mx::ChatUser &user(int row) const { return m_users.at(row); }
    int userCount() const { return int(m_users.size()); }
    int rowOfKey(const QString &key) const;
    static QString displayName(const mx::ChatUser &u);   // "@name" / "+name" / "name"

private:
    QList<mx::ChatUser> m_users;
};

class ChatUserProxy : public QSortFilterProxyModel {
    Q_OBJECT
public:
    explicit ChatUserProxy(QObject *parent = nullptr);

protected:
    bool lessThan(const QModelIndex &a, const QModelIndex &b) const override;
};

class IgnoredModel : public QAbstractListModel {
    Q_OBJECT
public:
    explicit IgnoredModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    void setUsers(const QList<mx::UserRef> &users);
    const mx::UserRef &user(int row) const { return m_users.at(row); }
    int userCount() const { return int(m_users.size()); }
    // "Nick [Net] opennap.example.net:8888" / "HelpBot.bot123_42591 OctoMX 904174ce1871"
    static QString entryText(const mx::UserRef &u);

private:
    QList<mx::UserRef> m_users;
};

} // namespace mx::ui

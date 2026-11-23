// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: model of the Hotlist pane (UI_SPEC §3.5): section rows "Temporary" / "Online" /
// "Offline" (only when non-empty) followed by their entries.
//   Temporary = users we exchanged private messages with this session (not on the hotlist),
//   Online / Offline = permanent hotlist entries from mx::UserService.
#pragma once

#include <QtCore/QAbstractListModel>

#include "mx/types.h"

namespace mx {
class UserService;
class ChatService;
}

namespace mx::ui {

class HotlistModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Kind { SectionRow = 0, TemporaryRow, OnlineRow, OfflineRow };

    HotlistModel(mx::UserService *users, mx::ChatService *chat, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

    Kind kind(int row) const;
    mx::UserRef user(int row) const;
    void addTemporary(const mx::UserRef &u);
    void removeTemporary(const mx::UserRef &u);
    static QString entryText(const mx::UserRef &u, const QString &note = QString());
    void setOwnName(const QString &n) { m_ownName = n; }

public slots:
    void reload();

private:
    struct Row {
        Kind kind = SectionRow;
        QString text;
        mx::UserRef user;
    };
    mx::UserService *m_users;
    QList<mx::UserRef> m_temporary;
    QList<Row> m_rows;
    QString m_ownName;
};

} // namespace mx::ui

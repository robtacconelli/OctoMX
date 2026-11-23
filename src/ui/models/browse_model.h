// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: model of the Browse window (UI_SPEC §5.2): another user's shared files.
//
// Columns File / Bytes / Time / Bitrate / Freq. Without "Show Full Paths" the files are grouped
// under one row per remote folder ("C:\My Music\Jazz\", [+] expander, folder icon); with it the
// list is flat and shows every file's full remote path. Row colours follow the Search legend
// (UI_SPEC §2.2): a matcher supplied by the window says whether a file matches an incomplete local
// download (yellow), can join a running download (cyan) or is currently downloading (blue).
#pragma once

#include <QtCore/QAbstractItemModel>
#include <QtCore/QHash>
#include <QtCore/QSortFilterProxyModel>
#include <functional>

#include "mx/types.h"

namespace mx::ui {

class BrowseModel : public QAbstractItemModel {
    Q_OBJECT
public:
    enum Column { File = 0, Bytes, Time, Bitrate, Freq, ColumnCount };
    enum Local { NoMatch = 0, MatchesIncomplete, CanJoin, Downloading };

    explicit BrowseModel(QObject *parent = nullptr);

    QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    bool hasChildren(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation o, int role) const override;

    void clear();
    void append(const QList<mx::BrowseEntry> &entries);   // rebuilds (model reset)
    void setShowFullPaths(bool on);
    bool showFullPaths() const { return m_fullPaths; }
    void setLocalMatcher(std::function<Local(const mx::BrowseEntry &)> m) { m_matcher = std::move(m); }
    void refreshLocal();                                   // re-run the matcher on every row

    bool isFolderRow(const QModelIndex &i) const;
    const mx::BrowseEntry *entryAt(const QModelIndex &i) const;
    QList<int> entriesOf(const QModelIndex &i) const;      // a file row: itself; a folder: its files
    const mx::BrowseEntry &entry(int n) const { return m_entries.at(n); }
    int entryCount() const { return int(m_entries.size()); }
    qint64 totalBytes() const { return m_total; }
    int folderCount() const { return int(m_folders.size()); }
    static QString fileNameOf(const QString &remotePath);
    static QString dirOf(const QString &remotePath);

private:
    void rebuild();
    struct Folder {
        QString path;         // "C:\My Music\Jazz\"
        QList<int> files;
    };
    QList<mx::BrowseEntry> m_entries;
    QList<Local> m_local;
    QList<Folder> m_folders;
    qint64 m_total = 0;
    bool m_fullPaths = false;
    std::function<Local(const mx::BrowseEntry &)> m_matcher;
};

class BrowseProxy : public QSortFilterProxyModel {
    Q_OBJECT
public:
    explicit BrowseProxy(QObject *parent = nullptr);

protected:
    bool lessThan(const QModelIndex &a, const QModelIndex &b) const override;
};

} // namespace mx::ui

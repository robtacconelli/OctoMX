// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: model of the Shared Files page (UI_SPEC §3.2.1, reference
// web/shared_files_page_columns_353.png, web/menu_shared_files_folder_353.png).
//
// Top-level rows = one row per folder that holds shared files (full path with a trailing
// separator, small yellow folder icon), sorted by path, exactly as WinMX 3.53 lists
// "C:\MX Shared\", "C:\MX Shared\Artist 1\" ... Shared roots without files of their own are listed
// too (without an expander). Expanding a folder shows its files: File Path (name) / Size / Bitrate
// / Time / Freq. Files the hasher has not finished yet are drawn in the inactive gray.
//
// The model is rebuilt from mx::LibraryService (folders() + files()) with setContent(); the page
// keeps expansion and selection across rebuilds. SharedProxy sorts by ItemRole::SortRole.
#pragma once

#include <QtCore/QAbstractItemModel>
#include <QtCore/QHash>
#include <QtCore/QSortFilterProxyModel>

#include "mx/types.h"

namespace mx::ui {

namespace shared {
enum Column { Path = 0, Size, Bitrate, Time, Freq, ColumnCount };
// "/home/x/MX Shared" style key for a directory (forward slashes, no trailing slash)
QString dirKey(const QString &path);
// native display form with trailing separator ("C:\MX Shared\")
QString displayDir(const QString &dirKey);
} // namespace shared

class SharedFilesModel : public QAbstractItemModel {
    Q_OBJECT
public:
    explicit SharedFilesModel(QObject *parent = nullptr);

    QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    bool hasChildren(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation o, int role) const override;

    // Rebuild everything (model reset).
    void setContent(const QList<mx::SharedFolder> &roots, const QList<mx::SharedFile> &files);

    bool isFolderRow(const QModelIndex &i) const { return i.isValid() && i.internalId() == 0; }
    // folder key of a folder row, or of the folder a file row belongs to
    QString folderKey(const QModelIndex &i) const;
    bool isSharedRoot(const QModelIndex &i) const;      // folder row that is a shared root
    const mx::SharedFile *fileAt(const QModelIndex &i) const;
    QModelIndex folderIndex(const QString &dirKey) const;
    QModelIndex fileIndex(const QString &path) const;
    int folderCount() const { return int(m_folders.size()); }
    int fileCount() const { return int(m_files.size()); }
    qint64 totalBytes() const { return m_total; }
    int hashedCount() const { return m_hashed; }
    // the shared root that contains `dirKey` (empty if none)
    QString rootOf(const QString &dirKey) const;
    QStringList filesInFolder(const QString &dirKey) const;   // paths

private:
    struct Folder {
        QString key;          // dirKey()
        QString display;      // native, trailing separator
        QList<int> files;     // indexes into m_files
        bool root = false;
        qint64 bytes = 0;
    };
    QList<mx::SharedFile> m_files;
    QList<Folder> m_folders;
    QHash<QString, int> m_rowOfDir;
    QHash<QString, int> m_fileOfPath;
    QStringList m_roots;      // dir keys of the shared roots
    qint64 m_total = 0;
    int m_hashed = 0;
};

class SharedProxy : public QSortFilterProxyModel {
    Q_OBJECT
public:
    explicit SharedProxy(QObject *parent = nullptr);

protected:
    bool lessThan(const QModelIndex &a, const QModelIndex &b) const override;
};

} // namespace mx::ui

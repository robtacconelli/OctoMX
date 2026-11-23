// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: the Shared Files page dialogs (UI_SPEC §3.2.1):
//   ShareFolderDialog      "Browse for Folder" with the caption "Hold SHIFT while you click OK to
//                          leave the sub folders OUT." (layout: reference web/dialog_share_folder_browse_353.png)
//   SharedFileTypesDialog  "Shared File Types": nine type check boxes ([ui-f], from the type table) + "Other file extensions"
//                          (reference web/dialog_shared_file_types_353.png)
#pragma once

#include <QtWidgets/QDialog>

class QCheckBox;
class QFileSystemModel;
class QLineEdit;
class QTreeView;

namespace mx {
class Settings;
class LibraryService;
}

namespace mx::ui {

// Settings keys behind LibraryService::sharedTypes() / otherExtensions() (the core filters by them).
namespace sharekeys {
inline const QString SharedTypes = QStringLiteral("library/sharedTypes");          // StringList of type ids
inline const QString OtherExtensions = QStringLiteral("library/otherExtensions");  // String, space separated
}

class ShareFolderDialog : public QDialog {
    Q_OBJECT
public:
    explicit ShareFolderDialog(const QString &startPath, QWidget *parent = nullptr);
    QString folder() const { return m_path; }
    bool recursive() const { return m_recursive; }
    void setFolder(const QString &path);     // select a folder programmatically (tests)

    void done(int r) override;

private:
    QTreeView *m_tree;
    QFileSystemModel *m_fs;
    QString m_path;
    bool m_recursive = true;
};

class SharedFileTypesDialog : public QDialog {
    Q_OBJECT
public:
    // [ui-d] reads / applies LibraryService::sharedTypes() / otherExtensions() (the core filters by them)
    // `settings`: library/shareUnlistedTypes ("files of other types" check box; nullptr = hidden)
    explicit SharedFileTypesDialog(mx::LibraryService *library, mx::Settings *settings = nullptr, QWidget *parent = nullptr);
    static QStringList typeIds();             // "mp3", "ogg", "video", ...
    static QStringList typeLabels();          // [ui-f] "Audio (.mp3 .flac .m4a .ogg ...)", ...
    void accept() override;

private:
    mx::LibraryService *m_library;
    mx::Settings *m_settings = nullptr;
    QCheckBox *m_unlisted = nullptr;
    QList<QCheckBox *> m_checks;
    QLineEdit *m_other;
};

} // namespace mx::ui

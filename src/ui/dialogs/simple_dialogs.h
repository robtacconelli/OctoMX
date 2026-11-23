// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: small classic dialogs: Change Screen Name, Add/Edit Server, Add Link,
// bundle (multi-file torrent) file selection.
#pragma once

#include <QtWidgets/QDialog>

#include "mx/types.h"

class QCheckBox;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QTreeWidget;
class QLabel;

namespace mx::ui {

class ChangeNameDialog : public QDialog {
    Q_OBJECT
public:
    ChangeNameDialog(const QString &current, QWidget *parent = nullptr);
    QString name() const;

private:
    QLineEdit *m_edit;
};

// [ui-f] Search ▸ file type ▸ "Custom Extensions...": one line of extensions ("mkv srt", ".mkv, .srt").
class CustomExtensionsDialog : public QDialog {
    Q_OBJECT
public:
    CustomExtensionsDialog(const QString &current, QWidget *parent = nullptr);
    QString text() const;
    QStringList extensions() const;   // mx::parseExtensionList(text())

private:
    QLineEdit *m_edit;
};

class ServerDialog : public QDialog {
    Q_OBJECT
public:
    ServerDialog(const QString &title, QWidget *parent = nullptr);
    void setNetwork(mx::Net n);
    void setAddress(const QString &a);
    void setName(const QString &n);
    mx::Net network() const;
    QString address() const;
    QString name() const;

private:
    QComboBox *m_net;
    QLineEdit *m_name;
    QLineEdit *m_host;
    QLineEdit *m_port;
};

// "Add Link": paste magnet: / ed2k:// / http(s):// links (one per line) or browse for .torrent files.
class AddLinkDialog : public QDialog {
    Q_OBJECT
public:
    explicit AddLinkDialog(QWidget *parent = nullptr);
    void setText(const QString &t);
    QStringList links() const;
    // [dl-1] the two order flags for the new downloads (defaults: transfers/sequentialDefault,
    // transfers/firstLastDefault, set by the caller)
    void setOrder(bool sequential, bool firstLast);
    bool sequential() const;
    bool firstLast() const;

private:
    QPlainTextEdit *m_edit;
    QCheckBox *m_seq = nullptr;
    QCheckBox *m_firstLast = nullptr;
};

// Choose which files of a bundle (multi-file torrent) to download. Classic white tree with check
// boxes, "Select All" / "Select None", total size line.
class BundleFilesDialog : public QDialog {
    Q_OBJECT
public:
    BundleFilesDialog(const QString &bundleName, const QList<mx::BundleFile> &files, const QList<int> &selected,
                      QWidget *parent = nullptr);
    QList<int> selectedIndexes() const;   // empty list means "all"

private:
    void updateTotal();
    QTreeWidget *m_tree;
    QLabel *m_total;
    QList<mx::BundleFile> m_files;
};

} // namespace mx::ui

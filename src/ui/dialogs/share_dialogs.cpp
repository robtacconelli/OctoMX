// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/dialogs/share_dialogs.h"

#include "mx/services.h"
#include "mx/settings_keys.h"

#include <QtCore/QDir>
#include <QtGui/QGuiApplication>
#include <QtGui/QFileSystemModel>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QTreeView>

#include "mx/services.h"
#include "ui/kit/file_types.h"
#include "ui/kit/widgets.h"
#include "ui/kit/scale.h"

namespace mx::ui {

// ---------------------------------------------------------------------------- ShareFolderDialog

ShareFolderDialog::ShareFolderDialog(const QString &startPath, QWidget *parent) : QDialog(parent)
{
    setObjectName(QStringLiteral("shareFolderDialog"));
    setWindowTitle(tr("Browse for Folder"));
    setFixedSize(SS(317, 312));
    auto *label = new WrapLabel(tr("Hold SHIFT while you click OK to leave the sub folders OUT."), this);
    label->setGeometry(SR(12, 9, 260, 28));

    m_fs = new QFileSystemModel(this);
    m_fs->setFilter(QDir::Dirs | QDir::Drives | QDir::NoDotAndDotDot);
    m_fs->setReadOnly(true);
    m_fs->setRootPath(QString());
    m_tree = new QTreeView(this);
    m_tree->setObjectName(QStringLiteral("shareFolderTree"));
    m_tree->setModel(m_fs);
    for (int c = 1; c < m_fs->columnCount(); ++c)
        m_tree->hideColumn(c);
    m_tree->setHeaderHidden(true);
    m_tree->setAnimated(false);
    m_tree->setGeometry(SR(12, 40, 291, 214));
    connect(m_tree->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex &i) { m_path = i.isValid() ? m_fs->filePath(i) : QString(); });

    auto *ok = new PushButton(tr("OK"), this);
    ok->setObjectName(QStringLiteral("dlgOk"));
    ok->setDefault(true);
    ok->setGeometry(SR(148, 271, 75, 23));
    auto *cancel = new PushButton(tr("Cancel"), this);
    cancel->setObjectName(QStringLiteral("dlgCancel"));
    cancel->setGeometry(SR(229, 271, 75, 23));
    connect(ok, &QPushButton::clicked, this, &QDialog::accept);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    if (!startPath.isEmpty())
        setFolder(startPath);
}

void ShareFolderDialog::setFolder(const QString &path)
{
    m_path = QDir::cleanPath(path);
    const QModelIndex i = m_fs->index(m_path);
    if (i.isValid()) {
        m_tree->setCurrentIndex(i);
        m_tree->scrollTo(i);
    }
}

void ShareFolderDialog::done(int r)
{
    if (r == QDialog::Accepted) {
        if (m_path.isEmpty())
            return;   // nothing selected: keep the dialog open
        // as in the original WinMX: SHIFT held while clicking OK excludes the sub folders
        m_recursive = !(QGuiApplication::keyboardModifiers() & Qt::ShiftModifier);
    }
    QDialog::done(r);
}

// ------------------------------------------------------------------------ SharedFileTypesDialog

QStringList SharedFileTypesDialog::typeIds()
{
    return mx::sharedTypeIds();   // [ui-f] audio video pictures documents software archives torrents servers clr
}

QStringList SharedFileTypesDialog::typeLabels()
{
    // [ui-f] the Search menu's labels, from the same table (the tool tips list every extension)
    QStringList out;
    for (const QString &id : typeIds())
        out << filetypes::menuLabel(id);
    return out;
}

SharedFileTypesDialog::SharedFileTypesDialog(mx::LibraryService *library, mx::Settings *settings, QWidget *parent)
    : QDialog(parent), m_library(library), m_settings(settings)
{
    setObjectName(QStringLiteral("sharedFileTypesDialog"));
    setWindowTitle(tr("Shared File Types"));
    // [ui-d] + one row: "files of other types" (library/shareUnlistedTypes, NG) above the buttons
    const int extra = settings ? 22 : 0;
    const int dy = (int(typeIds().size()) - 8) * 22;   // [ui-f] nine groups (WinMX 3.53 had eight)
    setFixedSize(SS(329, 292 + extra + dy));
    const QStringList ids = typeIds();
    const QStringList labels = typeLabels();
    const QStringList on = library->sharedTypes();
    for (int i = 0; i < ids.size(); ++i) {
        auto *c = new QCheckBox(labels.at(i), this);
        c->setObjectName(QStringLiteral("type_") + ids.at(i));
        c->setChecked(on.contains(ids.at(i)));
        c->setToolTip(filetypes::kindDescription(ids.at(i)));
        c->setGeometry(SR(12, 13 + i * 22, 300, 17));
        m_checks << c;
    }
    auto *l = new StaticText(tr("Other file extensions"), this);
    l->setGeometry(SR(16, 191 + dy, 200, 14));
    m_other = new QLineEdit(this);
    m_other->setObjectName(QStringLiteral("otherExtensions"));
    m_other->setToolTip(tr("More extensions to share besides the groups above, separated by spaces, for example: nfo cue."));
    m_other->setText(library->otherExtensions());
    m_other->setGeometry(SR(12, 206 + dy, 274, 21));
    auto *l2 = new StaticText(tr("(separate with spaces)"), this);
    l2->setGeometry(SR(16, 231 + dy, 200, 14));
    auto *ok = new PushButton(tr("OK"), this);
    ok->setObjectName(QStringLiteral("dlgOk"));
    ok->setDefault(true);
    ok->setGeometry(SR(93, 256 + extra + dy, 75, 23));
    auto *cancel = new PushButton(tr("Cancel"), this);
    cancel->setObjectName(QStringLiteral("dlgCancel"));
    cancel->setGeometry(SR(174, 256 + extra + dy, 75, 23));
    if (settings) {
        m_unlisted = new QCheckBox(tr("Share files of all other types too"), this);
        m_unlisted->setObjectName(QStringLiteral("shareUnlistedTypes"));
        m_unlisted->setToolTip(tr("Also share files whose type is in none of the groups above, for example files without an extension."));
        m_unlisted->setChecked(settings->b(mx::keys::library::ShareUnlistedTypes));
        m_unlisted->setGeometry(SR(12, 250 + dy, 300, 17));
    }
    connect(ok, &QPushButton::clicked, this, &QDialog::accept);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
}

void SharedFileTypesDialog::accept()
{
    QStringList on;
    const QStringList ids = typeIds();
    for (int i = 0; i < m_checks.size(); ++i)
        if (m_checks.at(i)->isChecked())
            on << ids.at(i);
    m_library->setSharedTypes(on);
    m_library->setOtherExtensions(m_other->text().simplified());
    if (m_settings && m_unlisted)
        m_settings->setValue(mx::keys::library::ShareUnlistedTypes, m_unlisted->isChecked());
    QDialog::accept();
}

} // namespace mx::ui

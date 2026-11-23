// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/pages/shared_page.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QClipboard>
#include <QtGui/QDesktopServices>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QScrollBar>

#include "mx/services.h"
#include "ui/dialogs/share_dialogs.h"
#include "ui/kit/tips.h"
#include "ui/kit/format.h"
#include "ui/kit/list_view.h"
#include "ui/kit/widgets.h"
#include "ui/kit/launcher.h"
#include "ui/kit/links.h"
#include "ui/kit/scale.h"
#include "ui/main_window.h"
#include "ui/models/roles.h"
#include "ui/models/shared_model.h"

namespace mx::ui {

namespace {
const QString kLastShareDir = QStringLiteral("ui/lastShareDir");
}

SharedPage::SharedPage(mx::Core *core, MainWindow *mw, QWidget *parent)
    : Page(QStringLiteral("shared"), tr("Shared Files"), core, mw, parent)
{
    struct B { IconId icon; const char *text; const char *name; };
    const B defs[] = {
        {IconId::ShareFolder, QT_TR_NOOP("Share Folder"), "sharedShareFolder"},
        {IconId::UnshareFolder, QT_TR_NOOP("Unshare Folder"), "sharedUnshareFolder"},
        {IconId::Refresh, QT_TR_NOOP("Refresh"), "sharedRefresh"},
        {IconId::SharedFileTypes, QT_TR_NOOP("Shared File Types"), "sharedFileTypes"},
        {IconId::ScanForMedia, QT_TR_NOOP("Scan For Media"), "sharedScanForMedia"},
    };
    for (const B &b : defs) {
        auto *btn = new ThinButton(b.icon, tr(b.text), this);
        btn->setObjectName(QString::fromLatin1(b.name));
        m_buttons << btn;
    }
    // [ui-g] what each button does
    m_buttons[0]->setToolTip(tr("Choose a folder whose files other users may download (with its sub folders; hold Shift on OK for "
                                "this folder only)"));
    m_buttons[1]->setToolTip(tr("Stop sharing one of the shared folders; the files stay on your disk"));
    m_buttons[2]->setToolTip(tr("Look for new, changed and removed files in the shared folders now"));
    m_buttons[3]->setToolTip(tr("Choose which kinds of files (audio, video, documents ...) of the shared folders are offered"));
    m_buttons[4]->setToolTip(tr("Share the usual media folders of this computer (Music, Videos, Pictures, Downloads) that are not "
                                "shared yet; you are asked first"));
    LibraryService *lib = core->library();
    connect(m_buttons[0], &QAbstractButton::clicked, this, &SharedPage::shareFolder);
    connect(m_buttons[1], &QAbstractButton::clicked, this, &SharedPage::showUnshareMenu);
    connect(m_buttons[2], &QAbstractButton::clicked, lib, &LibraryService::rescan);
    connect(m_buttons[3], &QAbstractButton::clicked, this, &SharedPage::showFileTypes);
    connect(m_buttons[4], &QAbstractButton::clicked, this, &SharedPage::scanForMedia);

    m_model = new SharedFilesModel(this);
    m_proxy = new SharedProxy(this);
    m_proxy->setSourceModel(m_model);
    m_list = new MxListView(this);
    m_list->setFilterEnabled(true);   // [ui-e] Ctrl+F
    m_list->setObjectName(QStringLiteral("sharedList"));
    m_list->setTreeMode(true);
    // 3.53 layout (menu_shared_files_folder_353.png): [+] 5 px into the cell, no dotted connectors,
    // folder icon at +20, text 2 px after the icon
    m_list->setTreeGeometry(5, 20, false, 2);
    m_list->setRowHeight(17);   // 17-px pitch measured on menu_shared_files_folder_353.png
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setModel(m_proxy);
    // measured on web/shared_files_page_columns_353.png
    m_list->setDefaultColumnWidths({489, 122, 44, 48, 43});
    m_list->setStateKey(core->settings(), QStringLiteral("shared"));
    connect(m_list, &MxListView::contextMenuAt, this, &SharedPage::showContextMenu);
    connect(m_list, &MxListView::rowActivated, this, &SharedPage::activate);

    m_reloadTimer = new QTimer(this);
    m_reloadTimer->setSingleShot(true);
    m_reloadTimer->setInterval(300);
    connect(m_reloadTimer, &QTimer::timeout, this, &SharedPage::reload);
    connect(lib, &LibraryService::foldersChanged, this, &SharedPage::scheduleReload);
    connect(lib, &LibraryService::filesChanged, this, &SharedPage::scheduleReload);
    connect(lib, &LibraryService::scanProgress, this, [this](int done, int total, const QString &file) {
        if (total <= 0 || done >= total) {
            m_scan.clear();
            scheduleReload();   // final hashed state
        } else {
            m_scan = file.isEmpty() ? tr("Hashing %1 of %2").arg(done).arg(total)
                                    : tr("Hashing %1 of %2: %3").arg(done).arg(total).arg(QFileInfo(file).fileName());
        }
        emit statusChanged();
    });
    reload();
}

void SharedPage::scheduleReload()
{
    if (!m_reloadTimer->isActive())
        m_reloadTimer->start();
}

void SharedPage::reload()
{
    // keep expanded folders, selection and scroll position across the rebuild
    QSet<QString> expanded;
    for (int r = 0; r < m_proxy->rowCount(); ++r) {
        const QModelIndex vi = m_proxy->index(r, 0);
        if (m_list->isExpanded(vi))
            expanded.insert(vi.data(IdRole).toString());
    }
    QStringList selected;
    for (const QModelIndex &s : m_list->selectedSourceRows())
        selected << s.data(IdRole).toString();
    const QString current = m_list->currentSourceIndex().data(IdRole).toString();
    const int scroll = m_list->verticalScrollBar()->value();

    LibraryService *lib = core()->library();
    m_model->setContent(lib->folders(), lib->files());
    if (m_list->header()->sortIndicatorSection() >= 0)
        m_proxy->sort(m_list->header()->sortIndicatorSection(), m_list->header()->sortIndicatorOrder());

    for (const QString &k : std::as_const(expanded)) {
        const QModelIndex vi = m_list->mapFromSource(m_model->folderIndex(k));
        if (vi.isValid())
            m_list->expand(vi);
    }
    QItemSelectionModel *sm = m_list->selectionModel();
    for (const QString &id : std::as_const(selected)) {
        QModelIndex si = m_model->folderIndex(id);
        if (!si.isValid())
            si = m_model->fileIndex(id);
        const QModelIndex vi = m_list->mapFromSource(si);
        if (vi.isValid())
            sm->select(vi, QItemSelectionModel::Select | QItemSelectionModel::Rows);
    }
    if (!current.isEmpty()) {
        QModelIndex si = m_model->folderIndex(current);
        if (!si.isValid())
            si = m_model->fileIndex(current);
        const QModelIndex vi = m_list->mapFromSource(si);
        if (vi.isValid())
            sm->setCurrentIndex(vi, QItemSelectionModel::NoUpdate);
    }
    m_list->verticalScrollBar()->setValue(scroll);
    if (lib->isScanning() && m_scan.isEmpty())
        m_scan = lib->scanStatus();
    else if (!lib->isScanning() && m_scan == lib->scanStatus())
        m_scan.clear();
    emit statusChanged();
}

QStringList SharedPage::statusFields() const
{
    const int n = m_model->fileCount();
    const qint64 megs = m_model->totalBytes() / (1024 * 1024);
    QStringList f;
    // [ui-e] "74 files shared (3.20 GB)"; classic with exact byte counts: "(3 277 megs)"
    const QString amount = fmt::exactSizes() ? tr("%1 megs").arg(fmt::number(megs)) : fmt::size(m_model->totalBytes());
    f << (n == 1 ? tr("1 file shared (%1)").arg(amount) : tr("%1 files shared (%2)").arg(n).arg(amount));
    if (!m_scan.isEmpty())
        f << m_scan;
    else if (n > 0 && m_model->hashedCount() < n)
        f << tr("%1 of %2 files hashed").arg(m_model->hashedCount()).arg(n);
    return f;
}

void SharedPage::saveState()
{
    m_list->saveState();
}

void SharedPage::shareFolder()
{
    Settings *st = core()->settings();
    QString start = st->s(kLastShareDir);
    if (start.isEmpty())
        start = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    auto *dlg = new ShareFolderDialog(start, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &QDialog::accepted, this, [this, dlg] {
        const QString path = QDir::toNativeSeparators(dlg->folder());
        core()->settings()->setValue(kLastShareDir, dlg->folder());
        core()->library()->addFolder(path, dlg->recursive());
    });
    dlg->open();
}

void SharedPage::showUnshareMenu()
{
    auto *menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("unshareMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    const QList<SharedFolder> folders = core()->library()->folders();
    for (const SharedFolder &f : folders) {
        const QString key = shared::dirKey(f.path);
        QString label = shared::displayDir(key);
        if (!f.recursive)
            label += tr("  (no sub folders)");
        menu->addAction(label, this, [this, key] { unshareFolderKey(key); });
    }
    if (folders.isEmpty())
        menu->addAction(tr("(no shared folders)"))->setEnabled(false);
    tips::explainMenu(menu);   // [ui-g] tool tips + status bar text of the entries
    menu->popup(m_buttons[1]->mapToGlobal(QPoint(0, m_buttons[1]->height())));
}

void SharedPage::unshareFolderKey(const QString &key)
{
    LibraryService *lib = core()->library();
    for (const SharedFolder &f : lib->folders())
        if (shared::dirKey(f.path) == key) {
            lib->removeFolder(f.path);
            return;
        }
    // a sub folder of a shared root: exclude the folder (persisted by the service)
    lib->excludeFolder(key, true);
}

void SharedPage::showFileTypes()
{
    auto *dlg = new SharedFileTypesDialog(core()->library(), core()->settings(), this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->open();
}

void SharedPage::scanForMedia()
{
    // NG meaning: share the standard media folders the core suggests (Music / Videos / Pictures /
    // Downloads that exist) and that are not shared yet
    QStringList candidates;
    for (const QString &p : core()->library()->suggestMediaFolders()) {
        if (p.isEmpty() || !QFileInfo(p).isDir() || QDir(p) == QDir::home())
            continue;
        const QString k = shared::dirKey(p);
        if (m_model->rootOf(k).isEmpty() && !candidates.contains(p))
            candidates << p;
    }
    if (candidates.isEmpty()) {
        QMessageBox::information(this, tr("Scan For Media"), tr("All your media folders are already shared."));
        return;
    }
    QStringList shown;
    for (const QString &c : std::as_const(candidates))
        shown << QDir::toNativeSeparators(c);
    if (QMessageBox::question(this, tr("Scan For Media"),
                              tr("Share these media folders (including sub folders)?\n\n%1").arg(shown.join(QLatin1Char('\n'))),
                              QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
        return;
    for (const QString &c : std::as_const(shown))
        core()->library()->addFolder(c, true);
}

void SharedPage::activate(const QModelIndex &vi)
{
    const QModelIndex si = m_list->mapToSource(vi);
    if (m_model->isFolderRow(si)) {
        const QModelIndex first = vi.sibling(vi.row(), 0);
        m_list->setExpanded(first, !m_list->isExpanded(first));
        return;
    }
    if (const SharedFile *f = m_model->fileAt(si))
        QDesktopServices::openUrl(QUrl::fromLocalFile(f->path));
}

void SharedPage::showContextMenu(const QModelIndex &vi, const QPoint &globalPos)
{
    auto *menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("sharedMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    const QModelIndex si = m_list->mapToSource(vi);
    if (!si.isValid()) {
        menu->addAction(tr("Share Folder..."), this, &SharedPage::shareFolder);
        menu->addAction(tr("Refresh"), core()->library(), &LibraryService::rescan);
    } else if (m_model->isFolderRow(si)) {
        const QModelIndex first = vi.sibling(vi.row(), 0);
        const bool exp = m_list->isExpanded(first);
        const QString key = m_model->folderKey(si);
        QAction *e = menu->addAction(exp ? tr("Collapse") : tr("Expand"), this, [this, first, exp] { m_list->setExpanded(first, !exp); });
        e->setEnabled(m_model->hasChildren(si));
        menu->setDefaultAction(e);
        menu->addSeparator();
        menu->addAction(tr("Open Folder"), this, [this, key] { launch(false, key); });
        menu->addSeparator();
        menu->addAction(tr("Un-share Folder"), this, [this, key] { unshareFolderKey(key); });
    } else if (const SharedFile *f = m_model->fileAt(si)) {
        const SharedFile file = *f;
        // [ui-e] Open File / Open Containing Folder (the file selected in the file manager)
        QAction *open = menu->addAction(tr("Open File"), this, [this, file] { launch(true, file.path); });
        open->setObjectName(QStringLiteral("actOpenFile"));
        menu->setDefaultAction(open);
        menu->addAction(tr("Open Containing Folder"), this, [this, file] { launch(false, file.path); })
            ->setObjectName(QStringLiteral("actShowInFolder"));
        menu->addSeparator();
        menu->addAction(tr("Copy Magnet Link"), this, [file] { QGuiApplication::clipboard()->setText(file.hashes.toMagnet(file.name, file.size)); })
            ->setEnabled(!file.hashes.isEmpty());
        menu->addAction(tr("Copy ed2k Link"), this, [file] { QGuiApplication::clipboard()->setText(ed2kLink(file.name, file.size, file.hashes)); })
            ->setEnabled(!file.hashes.ed2k.isEmpty());
        menu->addAction(tr("Copy Name"), this, [file] { QGuiApplication::clipboard()->setText(file.name); });
        menu->addSeparator();
        // every selected file
        QStringList paths;
        for (const QModelIndex &s : m_list->selectedSourceRows())
            if (const SharedFile *sf = m_model->fileAt(s))
                paths << sf->path;
        if (paths.isEmpty())
            paths << file.path;
        menu->addAction(paths.size() > 1 ? tr("Un-share %1 Files").arg(paths.size()) : tr("Un-share File"), this, [this, paths] {
            for (const QString &p : paths)
                core()->library()->unshareFile(p);
        });
    }
    tips::explainMenu(menu);   // [ui-g] tool tips + status bar text of the entries
    menu->popup(globalPos);
}

bool SharedPage::scriptCommand(const QString &verb, const QStringList &args, QString *error)
{
    // share-folder <path> [norecurse] | unshare-folder <path> | rescan
    if (verb == QLatin1String("share-folder")) {
        QStringList a = args;
        bool rec = true;
        if (!a.isEmpty() && a.last() == QLatin1String("norecurse")) {
            rec = false;
            a.removeLast();
        }
        if (a.isEmpty()) {
            *error = QStringLiteral("usage: share-folder <path> [norecurse]");
            return true;
        }
        core()->library()->addFolder(a.join(QLatin1Char(' ')), rec);
        return true;
    }
    if (verb == QLatin1String("unshare-folder")) {
        unshareFolderKey(shared::dirKey(args.join(QLatin1Char(' '))));
        return true;
    }
    if (verb == QLatin1String("rescan")) {
        core()->library()->rescan();
        return true;
    }
    return false;
}

void SharedPage::launch(bool open, const QString &path)
{
    QString why;
    const bool ok = open ? launcher::openFile(this, core()->settings(), path, &why) : launcher::showInFolder(path, &why);
    if (!ok && !why.isEmpty())
        mainWindow()->showNotice(why);
}

void SharedPage::resizeEvent(QResizeEvent *)
{
    // measured on web/shared_files_page_columns_353.png: row C at page y 5, list frame from y 29
    static const int xs[] = {2, 139, 277, 414, 551};
    static const int ws[] = {133, 134, 133, 133, 133};
    for (int i = 0; i < m_buttons.size(); ++i)
        m_buttons[i]->setGeometry(SR(xs[i], 5, ws[i], 21));
    m_list->setGeometry(0, S(29), width(), qMax(0, height() - S(29)));
}

} // namespace mx::ui

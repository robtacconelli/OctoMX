// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/dialogs/browse_window.h"

#include <QtGui/QShortcut>

#include <QtCore/QTimer>
#include <QtGui/QCloseEvent>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QScrollBar>

#include "mx/services.h"
#include "mx/settings_keys.h"
#include "ui/kit/format.h"
#include "ui/kit/icons.h"
#include "ui/kit/launcher.h"
#include "ui/kit/list_view.h"
#include "ui/kit/status_bar.h"
#include "ui/kit/widgets.h"
#include "ui/kit/scale.h"
#include "ui/kit/session.h"
#include "ui/main_window.h"
#include "ui/models/browse_model.h"

namespace mx::ui {

namespace {
const QLatin1StringView kAfsKey = keys::search::AutoFindSourcesMin;
const QLatin1StringView kAeqKey = keys::search::AutoEnterQueue;

QString fileKey(const QString &name, qint64 size)
{
    return name.toLower() + QLatin1Char('|') + QString::number(size);
}

QStringList hashKeys(const FileHashes &h)
{
    QStringList l;
    if (!h.ed2k.isEmpty())
        l << QStringLiteral("ed2k:") + QString::fromLatin1(h.ed2k.toHex());
    if (!h.sha1.isEmpty())
        l << QStringLiteral("sha1:") + QString::fromLatin1(h.sha1.toHex());
    if (!h.btih.isEmpty())
        l << QStringLiteral("btih:") + QString::fromLatin1(h.btih.toHex()) + QLatin1Char(':') + QString::number(h.btFileIndex);
    return l;
}

bool isActive(TransferState s)
{
    return s == TransferState::Transferring || s == TransferState::Connecting || s == TransferState::FindingSources ||
           s == TransferState::RemotelyQueued || s == TransferState::Verifying;
}
} // namespace

QString BrowseWindow::windowTitleFor(const UserRef &u)
{
    const QString name = u.name.isEmpty() ? u.id : u.name;
    const QString net = netName(u.net);
    return tr("%1 on %2").arg(name, net);
}

SearchHit BrowseWindow::hitFor(const UserRef &u, const BrowseEntry &e)
{
    SearchHit h;
    h.fileName = BrowseModel::fileNameOf(e.path);
    h.path = e.path;
    h.size = e.size;
    h.durationSec = e.durationSec;
    h.bitrateKbps = e.bitrateKbps;
    h.sampleRateHz = e.sampleRateHz;
    h.hashes = e.hashes;
    h.type = mediaTypeForFileName(h.fileName);
    const QStringList hk = hashKeys(e.hashes);
    h.key = hk.isEmpty() ? QStringLiteral("browse:%1:%2").arg(u.key(), e.path) : hk.first();
    SearchSource s;
    s.net = u.net;
    s.user = u;
    s.uri = e.path;
    h.sources << s;
    return h;
}

BrowseWindow::BrowseWindow(mx::Core *core, MainWindow *mw, const UserRef &user, QWidget *parent)
    : QWidget(parent, Qt::Window), m_core(core), m_main(mw), m_user(user)
{
    setObjectName(QStringLiteral("browseWindow"));
    setProperty("userKey", user.key());
    setAttribute(Qt::WA_DeleteOnClose);
    setAutoFillBackground(true);
    setWindowIcon(icon(IconId::Browse));
    setWindowTitle(windowTitleFor(user));
    setMinimumSize(SS(420, 240));

    auto mk = [this](const QString &t, const char *name) {
        auto *b = new PushButton(t, this);
        b->setObjectName(QString::fromLatin1(name));
        return b;
    };
    m_row1 << mk(tr("Refresh"), "browseRefresh") << mk(tr("Send Message"), "browseSendMessage") << mk(tr("Whois"), "browseWhois")
           << mk(tr("Add To Hotlist"), "browseAddHotlist") << mk(tr("Show Full Paths"), "browseFullPaths");
    m_row2 << mk(tr("Download"), "browseDownload") << mk(tr("Manually Resume..."), "browseManualResume");
    m_row1[4]->setCheckable(true);
    m_row1[0]->setToolTip(tr("Ask this user again for the list of files they share. Use it when the list is empty or old."));
    m_row1[1]->setToolTip(tr("Open a private message window to this user."));
    m_row1[2]->setToolTip(tr("Open a window with what this user tells about themselves: network, connection, files shared, upload slots and time online."));
    m_row1[3]->setToolTip(tr("Put this user on your Hotlist, so you see when they are online and can browse or message them later."));
    m_row1[4]->setToolTip(tr("Show every file with its whole folder path in one flat list instead of the folder tree."));
    m_row2[0]->setToolTip(tr("Download the selected files from this user. They appear in Transfers."));
    m_row2[1]->setToolTip(tr("Download the selected file. A partial copy already in Transfers is continued automatically, no file needs to be picked here."));
    Settings *st = core->settings();
    m_afs = new MenuButton(this);
    m_afs->setObjectName(QStringLiteral("browseAfs"));
    m_afs->addChoice(tr("Never"), 0, tr("Auto Find Sources - Never"));
    for (int m : {10, 15, 20, 25, 30, 40, 50, 60})
        m_afs->addChoice(tr("Every %1 min").arg(m), m, tr("Auto Find Sources - %1m").arg(m));
    m_afs->setCurrentData(st->i(kAfsKey));
    m_aeq = new MenuButton(this);
    m_aeq->setObjectName(QStringLiteral("browseAeq"));
    m_aeq->addChoice(tr("Never"), 0, tr("Auto Enter Queue - Never"));
    for (int n : {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 15, 20, 25, 30, 35, 40, 45, 50, 60, 70, 80, 90, 100})
        m_aeq->addChoice(QString::number(n), n, tr("Auto Enter Queue - %1").arg(n));
    m_aeq->setCurrentData(st->i(kAeqKey));
    m_afs->setToolTip(tr("For downloads started here, search again for more sources of the file at this interval while it is not complete. Click to change the interval."));
    m_aeq->setToolTip(tr("When a source puts your download in its waiting queue, wait only if your place is this number or better; "
                         "a longer queue is left while other sources work. Never leaves every queue. Click to change the number."));
    connect(m_afs, &MenuButton::choiceChanged, this, [this](const QVariant &v) {
        m_core->settings()->setValue(kAfsKey, v.toInt());
        if (v.toInt() > 0)
            m_core->settings()->setValue(keys::transfers::SourceSearchIntervalMin, v.toInt());
    });
    connect(m_aeq, &MenuButton::choiceChanged, this, [this](const QVariant &v) { m_core->settings()->setValue(kAeqKey, v.toInt()); });

    m_legend << new LegendItem(QString(keys::colors::MatchIncomplete), tr("Matches incomplete local file"), this)
             << new LegendItem(QString(keys::colors::CanJoinDownload), tr("Can be added to running multi-point download"), this)
             << new LegendItem(QString(keys::colors::CurrentlyDownloading), tr("Currently downloading"), this);

    m_model = new BrowseModel(this);
    m_model->setLocalMatcher([this](const BrowseEntry &e) {
        const QString fk = fileKey(BrowseModel::fileNameOf(e.path), e.size);
        QStringList keys = hashKeys(e.hashes);
        keys << fk;
        for (const QString &k : std::as_const(keys)) {
            if (m_activeKeys.contains(k + QStringLiteral("#mine")))
                return BrowseModel::Downloading;
            if (m_activeKeys.contains(k))
                return BrowseModel::CanJoin;
            if (m_incompleteKeys.contains(k))
                return BrowseModel::MatchesIncomplete;
        }
        return BrowseModel::NoMatch;
    });
    m_proxy = new BrowseProxy(this);
    m_proxy->setSourceModel(m_model);
    m_list = new MxListView(this);
    m_list->setFilterEnabled(true);   // [ui-e] Ctrl+F (a separate window: its own shortcut)
    connect(new QShortcut(QKeySequence::Find, this), &QShortcut::activated, m_list, &MxListView::showFilter);
    m_list->setObjectName(QStringLiteral("browseList"));
    m_list->setTreeMode(true);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setModel(m_proxy);
    m_list->setDefaultColumnWidths({430, 90, 45, 45, 45});
    m_list->setStateKey(st, QStringLiteral("browse"));
    m_status = new StatusBar(this);
    m_status->floatTab()->hide();

    connect(m_row1[0], &QPushButton::clicked, this, &BrowseWindow::refresh);
    connect(m_row1[1], &QPushButton::clicked, this, [this] { m_main->openPrivateMessage(m_user); });
    connect(m_row1[2], &QPushButton::clicked, this, [this] { m_main->openWhois(m_user); });
    connect(m_row1[3], &QPushButton::clicked, this, [this] { m_core->users()->addToHotlist(m_user); });
    connect(m_row1[4], &QPushButton::toggled, this, [this](bool on) {
        m_model->setShowFullPaths(on);
        if (!on)
            m_list->expandAll();
    });
    connect(m_row2[0], &QPushButton::clicked, this, &BrowseWindow::downloadSelected);
    connect(m_row2[1], &QPushButton::clicked, this, [this] {
        if (m_list->selectedSourceRows().isEmpty())
            return;
        QMessageBox::information(this, tr("File Download"),
                                 tr("Resuming from a partially downloaded file is done automatically when the same file is "
                                    "already in the Transfers list."));
        downloadSelected();
    });
    connect(m_list, &MxListView::rowActivated, this, [this](const QModelIndex &vi) {
        const QModelIndex s = m_list->mapToSource(vi);
        if (m_model->isFolderRow(s)) {
            const QModelIndex first = vi.sibling(vi.row(), 0);
            m_list->setExpanded(first, !m_list->isExpanded(first));
        } else {
            download(m_model->entriesOf(s));
        }
    });
    connect(m_list, &MxListView::contextMenuAt, this, &BrowseWindow::showContextMenu);

    UserService *us = core->users();
    connect(us, &UserService::browseResults, this, &BrowseWindow::onResults);
    connect(us, &UserService::browseFailed, this, &BrowseWindow::onFailed);
    m_localTimer = new QTimer(this);
    m_localTimer->setSingleShot(true);
    m_localTimer->setInterval(1000);
    connect(m_localTimer, &QTimer::timeout, this, [this] {
        rebuildLocalIndex();
        m_model->refreshLocal();
    });
    auto kick = [this] {
        if (!m_localTimer->isActive())
            m_localTimer->start();
    };
    connect(core->transfers(), &TransferService::transferAdded, this, kick);
    connect(core->transfers(), &TransferService::transferRemoved, this, kick);
    connect(core->transfers(), &TransferService::transferChanged, this, [kick](const TransferInfo &t) {
        if (!t.isUpload)
            kick();
    });
    rebuildLocalIndex();

    const QByteArray geo = st->value(keys::ui::dialogKey(QStringLiteral("browse"))).toByteArray();
    if (geo.isEmpty() || !restoreGeometry(geo))
        resize(SS(640, 600));
    refresh();
}

BrowseWindow::~BrowseWindow()
{
    if (m_request)
        m_core->users()->cancelBrowse(m_request);
}

void BrowseWindow::rebuildLocalIndex()
{
    m_activeKeys.clear();
    m_incompleteKeys.clear();
    const QString me = m_user.key();
    for (const TransferInfo &t : m_core->transfers()->downloads()) {
        if (t.state == TransferState::Completed || t.state == TransferState::Seeding || t.state == TransferState::Cancelled)
            continue;
        QStringList keys = hashKeys(t.hashes);
        keys << fileKey(t.name, t.size);
        bool fromThisUser = false;
        for (const TransferSource &s : t.sources)
            if (s.user.key() == me)
                fromThisUser = true;
        for (const QString &k : std::as_const(keys)) {
            if (isActive(t.state)) {
                m_activeKeys.insert(k);
                if (fromThisUser)
                    m_activeKeys.insert(k + QStringLiteral("#mine"));
            } else {
                m_incompleteKeys.insert(k);
            }
        }
    }
}

void BrowseWindow::refresh()
{
    if (m_request)
        m_core->users()->cancelBrowse(m_request);
    m_failure.clear();
    m_model->clear();
    m_request = m_core->users()->browse(m_user);
    updateStatus();
}

void BrowseWindow::onResults(quint32 id, const UserRef &, const QList<BrowseEntry> &files, bool finished)
{
    if (id != m_request || !m_request)
        return;
    // keep expansion (folders are expanded by default) and scroll position
    const int scroll = m_list->verticalScrollBar() ? m_list->verticalScrollBar()->value() : 0;
    m_model->append(files);
    if (!m_model->showFullPaths())
        m_list->expandAll();
    if (m_list->header()->sortIndicatorSection() >= 0)
        m_proxy->sort(m_list->header()->sortIndicatorSection(), m_list->header()->sortIndicatorOrder());
    m_list->verticalScrollBar()->setValue(scroll);
    if (finished)
        m_request = 0;
    updateStatus();
}

void BrowseWindow::onFailed(quint32 id, const UserRef &, const QString &reason)
{
    if (id != m_request || !m_request)
        return;
    m_request = 0;
    m_failure = reason.isEmpty() ? tr("Browse failed") : reason;
    updateStatus();
}

void BrowseWindow::updateStatus()
{
    QString s;
    const qint64 megs = m_model->totalBytes() / (1024 * 1024);
    const QString amount = fmt::exactSizes() ? tr("%1 megs").arg(fmt::number(megs)) : fmt::size(m_model->totalBytes());
    if (!m_failure.isEmpty())
        s = tr("Unable to browse> %1").arg(m_failure);
    else if (m_request)
        s = m_model->entryCount() ? tr("Downloading list> %1 files (%2) so far").arg(m_model->entryCount()).arg(amount)
                                  : tr("Downloading list...");
    else
        s = tr("Finished downloading list> %1 files (%2) total").arg(m_model->entryCount()).arg(amount);
    m_status->setFields({s});
}

void BrowseWindow::downloadSelected()
{
    QList<int> entries;
    for (const QModelIndex &s : m_list->selectedSourceRows())
        for (int e : m_model->entriesOf(s))
            if (!entries.contains(e))
                entries << e;
    download(entries);
}

std::function<int(const QString &)> BrowseWindow::s_askHook;

void BrowseWindow::download(const QList<int> &entries)
{
    if (entries.isEmpty())
        return;
    // [set-1] the dangerous file type warning, as for a download from the Search page (it was never
    // asked here): programs, scripts and archives only when the user says yes, unless the warning is
    // switched off (this session or for good, Settings > File Transfers)
    QList<int> wanted = entries;
    QStringList risky;
    for (int e : entries)
        if (launcher::isDangerousFile(m_model->entry(e).path.section(QLatin1Char('\\'), -1)))
            risky << m_model->entry(e).path.section(QLatin1Char('\\'), -1);
    if (!risky.isEmpty() && !Session::instance()->suppressDangerousWarning()
        && !m_core->settings()->b(keys::transfers::SuppressDangerousWarning)) {
        const QString text = tr("You are about to download\n\n    %1%2\n\nThis is a program or script, or a file that can hide one. "
                                "Such files may carry a virus or other harmful code, so it is safer not to download it.\n\n"
                                "Download it all the same?\n\n(This warning can be switched off for the rest of the session in "
                                "the program settings.)")
                                 .arg(risky.first(), risky.size() == 2 ? tr("\n    and 1 more of this kind")
                                    : risky.size() > 2 ? tr("\n    and %1 more of this kind").arg(risky.size() - 1) : QString());
        int answer;
        if (s_askHook) {
            answer = s_askHook(text);
        } else {
            QMessageBox box(QMessageBox::Warning, tr("OctoMX"), text, QMessageBox::Yes | QMessageBox::No, this);
            box.setObjectName(QStringLiteral("browseWarning"));
            box.setDefaultButton(QMessageBox::Yes);
            answer = box.exec();
        }
        if (answer != QMessageBox::Yes) {
            wanted.clear();
            for (int e : entries)
                if (!launcher::isDangerousFile(m_model->entry(e).path.section(QLatin1Char('\\'), -1)))
                    wanted << e;
            if (wanted.isEmpty())
                return;
        }
    }
    int n = 0;
    for (int e : wanted) {
        const QString id = m_core->transfers()->downloadHit(hitFor(m_user, m_model->entry(e)));
        if (!id.isEmpty())
            ++n;
    }
    m_status->setFields({n == 1 ? tr("Started 1 download") : tr("Started %1 downloads").arg(n)});
    QTimer::singleShot(3000, this, &BrowseWindow::updateStatus);
    if (!m_localTimer->isActive())
        m_localTimer->start();
}

void BrowseWindow::showContextMenu(const QModelIndex &vi, const QPoint &globalPos)
{
    const QModelIndex s = m_list->mapToSource(vi);
    if (!s.isValid())
        return;
    auto *menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("browseMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    if (m_model->isFolderRow(s)) {
        const QModelIndex first = vi.sibling(vi.row(), 0);
        const bool exp = m_list->isExpanded(first);
        QAction *e = menu->addAction(exp ? tr("Collapse") : tr("Expand"), this, [this, first, exp] { m_list->setExpanded(first, !exp); });
        menu->setDefaultAction(e);
        QAction *df = menu->addAction(tr("Download Folder"), this, [this, s] { download(m_model->entriesOf(s)); });
        df->setToolTip(tr("Download every file in this folder and its sub folders."));
        df->setStatusTip(df->toolTip());
    } else {
        QAction *d = menu->addAction(tr("Download"), this, &BrowseWindow::downloadSelected);
        menu->setDefaultAction(d);
        QAction *md = menu->addAction(tr("Manually Download"), this, [this] { m_row2[1]->click(); });
        md->setToolTip(m_row2[1]->toolTip());
        md->setStatusTip(md->toolTip());
    }
    menu->addSeparator();
    menu->addAction(tr("Send Message"), this, [this] { m_main->openPrivateMessage(m_user); });
    menu->addAction(tr("Whois"), this, [this] { m_main->openWhois(m_user); });
    QAction *hot = menu->addAction(tr("Add To Hotlist"), this, [this] { m_core->users()->addToHotlist(m_user); });
    hot->setToolTip(m_row1[3]->toolTip());
    hot->setStatusTip(hot->toolTip());
    menu->setToolTipsVisible(true);
    menu->popup(globalPos);
}

void BrowseWindow::resizeEvent(QResizeEvent *)
{
    // measured on web/browse_window_354b4.png (client coordinates): two rows of push buttons with
    // 6-px gaps between the groups, the legend, then the list
    m_row1[0]->setGeometry(SR(11, 1, 97, 19));
    m_row1[1]->setGeometry(SR(114, 1, 120, 19));
    m_row1[2]->setGeometry(SR(234, 1, 120, 19));
    m_row1[3]->setGeometry(SR(354, 1, 120, 19));
    m_row1[4]->setGeometry(SR(480, 1, 95, 19));
    m_row2[0]->setGeometry(SR(11, 22, 138, 19));
    m_row2[1]->setGeometry(SR(155, 22, 138, 19));
    m_afs->setGeometry(SR(299, 22, 138, 19));
    m_aeq->setGeometry(SR(437, 22, 138, 19));
    // the legend flows left to right (the kit's swatches are 30 px; the 3.54 browse window's 18)
    int lx = S(13);
    for (LegendItem *l : std::as_const(m_legend)) {
        const int lw = l->sizeHint().width();
        l->setGeometry(lx, S(42), lw, S(16));
        lx += lw + S(8);
    }
    const int sh = m_status->height();
    m_list->setGeometry(0, S(58), width(), qMax(0, height() - S(58) - sh));
    m_status->setGeometry(0, height() - sh, width(), sh);
}

void BrowseWindow::closeEvent(QCloseEvent *e)
{
    m_core->settings()->setValue(keys::ui::dialogKey(QStringLiteral("browse")), saveGeometry());
    m_list->saveState();
    QWidget::closeEvent(e);
}

} // namespace mx::ui

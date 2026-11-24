// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/pages/transfers_page.h"
#include "ui/dialogs/connection_test.h"

#include <functional>

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QLocale>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QClipboard>
#include <QtGui/QDesktopServices>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QPainter>
#include <QtGui/QActionGroup>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QVBoxLayout>

#include "mx/download_order.h"
#include "mx/services.h"
#include "mx/settings_keys.h"
#include "mx/trash.h"
#include "ui/dialogs/file_download_dialog.h"
#include "ui/dialogs/preview_dialog.h"
#include "ui/dialogs/remove_dialogs.h"
#include "ui/dialogs/simple_dialogs.h"
#include "ui/kit/tips.h"
#include "ui/kit/colors.h"
#include "ui/kit/draw.h"
#include "ui/kit/list_view.h"
#include "ui/kit/widgets.h"
#include "ui/kit/launcher.h"
#include "ui/kit/links.h"
#include "ui/kit/scale.h"
#include "ui/main_window.h"
#include "ui/models/transfer_model.h"
#include "ui/pages/search_page.h"

namespace mx::ui {

namespace {
const QLatin1StringView kAfsKey = keys::search::AutoFindSourcesMin;
const QLatin1StringView kAeqKey = keys::search::AutoEnterQueue;

// The original WinMX's "Bandwidth Priority" submenu (menu_transfers_upload_bandwidth_priority.png) on the
// three-level priority of the service API: x10 ... x1.5 = high, Normal, 90% ... 10% = low.
const char *const kPrioItems[] = {"x10", "x8", "x6", "x5", "x4", "x3", "x2", "x1.5", "Normal", "90%", "80%", "70%", "60%", "50%",
                                  "40%", "30%", "20%", "10%"};
int prioOfItem(const char *item)
{
    return item[0] == 'x' ? 1 : qstrcmp(item, "Normal") == 0 ? 0 : -1;
}
// the item shown checked for a priority (one representative per level)
const char *itemOfPrio(int p)
{
    return p > 0 ? "x2" : p < 0 ? "50%" : "Normal";
}
void addPriorityMenu(QMenu *bp, int current, const std::function<void(int)> &apply)
{
    auto *group = new QActionGroup(bp);
    for (const char *item : kPrioItems) {
        QAction *a = bp->addAction(QString::fromLatin1(item));
        a->setCheckable(true);
        a->setActionGroup(group);
        a->setChecked(qstrcmp(item, itemOfPrio(current)) == 0);
        const int p = prioOfItem(item);
        QObject::connect(a, &QAction::triggered, bp, [apply, p] { apply(p); });
    }
}

// Splitter whose handle is a plain face strip with a classic etched line on top (the bar the
// user drags between the download and the upload half).
class EtchedHandle : public QSplitterHandle {
public:
    using QSplitterHandle::QSplitterHandle;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), chrome::Face);
        draw::etchedHLine(&p, 0, width() - 1, 0);
    }
};

class EtchedSplitter : public ProportionalSplitter {   // [ui-e] keeps the halves' proportions
public:
    using ProportionalSplitter::ProportionalSplitter;

protected:
    QSplitterHandle *createHandle() override { return new EtchedHandle(orientation(), this); }
};

// lay out push buttons centred with the original WinMX's gaps (0 between "joined" buttons, 6 between groups)
void centreRow(const QList<PushButton *> &btns, const QList<int> &widths, const QList<int> &gapsBefore, int width, int y, int h)
{
    // widths / gaps / y / h in design pixels, `width` in device pixels
    int total = 0;
    for (int i = 0; i < widths.size(); ++i)
        total += widths[i] + gapsBefore.value(i);
    const int x0 = (width - S(total)) / 2;
    int x = 0;
    for (int i = 0; i < btns.size(); ++i) {
        x += gapsBefore.value(i);
        btns[i]->setGeometry(SR(x, y, widths[i], h).translated(x0, 0));
        x += widths[i];
    }
}
} // namespace

TransfersPage::TransfersPage(mx::Core *core, MainWindow *mw, QWidget *parent)
    : Page(QStringLiteral("transfers"), tr("Transfers"), core, mw, parent)
{
    m_split = new EtchedSplitter(Qt::Vertical, this);
    m_split->setObjectName(QStringLiteral("transfersSplitter"));
    m_split->setHandleWidth(S(5));
    m_split->setChildrenCollapsible(false);
    m_downHalf = buildDownloadHalf();
    m_upHalf = buildUploadHalf();
    m_split->addWidget(m_downHalf);
    m_split->addWidget(m_upHalf);
    m_downHalf->installEventFilter(this);
    m_upHalf->installEventFilter(this);
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(m_split);
    m_split->setStretchFactor(0, 3);
    m_split->setStretchFactor(1, 2);
    // the stored sizes are applied once the page has its final size (QSplitter would spread a later
    // resize over both halves by their stretch factors): see showEvent
    m_split->setSizes({S(330), S(200)});
    connect(m_split, &QSplitter::splitterMoved, this, [this] {
        this->core()->settings()->setValue(keys::ui::splitterKey(QStringLiteral("transfers")), m_split->saveState());
    });

    m_summaryTimer = new QTimer(this);
    m_summaryTimer->setInterval(500);
    m_summaryTimer->setSingleShot(true);
    connect(m_summaryTimer, &QTimer::timeout, this, &TransfersPage::updateSummary);
    auto kick = [this] {
        if (!m_summaryTimer->isActive())
            m_summaryTimer->start();
    };
    connect(core->transfers(), &TransferService::transferChanged, this, kick);
    connect(core->transfers(), &TransferService::transferAdded, this, [this, kick](const TransferInfo &t) {
        kick();
        // new downloads are expanded only when the box in Settings ▸ File Transfers is ticked [ui-h] (off by default)
        if (!t.isUpload && this->core()->settings()->b(keys::transfers::NewDownloadsExpanded)) {
            const int row = m_downModel->rowOfId(t.id);
            if (row >= 0)
                m_down->expand(m_down->mapFromSource(m_downModel->index(row, 0)));
        }
    });
    connect(core->transfers(), &TransferService::transferRemoved, this, kick);
    // [ui-j] what happened to the files of removed downloads
    m_deleteReport = new QTimer(this);
    m_deleteReport->setSingleShot(true);
    m_deleteReport->setInterval(300);
    connect(m_deleteReport, &QTimer::timeout, this, &TransfersPage::reportDeletes);
    connect(core->transfers(), &TransferService::downloadFilesDeleted, this, &TransfersPage::onFilesDeleted);
    // [ver-1] the outcome of Verify File / Restart Download; the answers for several downloads at once
    // arrive together and are shown together
    m_noticeBatchTimer = new QTimer(this);
    m_noticeBatchTimer->setSingleShot(true);
    m_noticeBatchTimer->setInterval(250);
    connect(m_noticeBatchTimer, &QTimer::timeout, this, [this] {
        const QStringList all = m_noticeBatch;
        m_noticeBatch.clear();
        if (!all.isEmpty())
            showNotice(all.join(QLatin1Char(' ')));
    });
    connect(core->transfers(), &TransferService::downloadNotice, this, [this](const QString &, const QString &text) {
        if (!m_noticeBatch.contains(text))
            m_noticeBatch << text;
        m_noticeBatchTimer->start();
    });
    updateSummary();
}

QWidget *TransfersPage::buildDownloadHalf()
{
    auto *w = new QWidget(this);
    w->setObjectName(QStringLiteral("downloadHalf"));
    auto mk = [w](const QString &t, const char *name) {
        auto *b = new PushButton(t, w);
        b->setObjectName(QString::fromLatin1(name));
        return b;
    };
    m_rowC << mk(tr("Clear Complete"), "dlClearComplete") << mk(tr("Load Incomplete"), "dlLoadIncomplete")
           << mk(tr("Purge Users"), "dlPurgeUsers") << mk(tr("Queuing..."), "dlQueuing") << mk(tr("Options..."), "dlOptions")
           << mk(tr("Open Incoming Files Folder"), "dlOpenIncoming");
    m_rowD << mk(tr("Cancel"), "dlCancel") << mk(tr("Clear"), "dlClear");
    m_retry = mk(tr("Retry"), "dlRetry");
    m_rowD << m_retry << mk(tr("Find Sources Now"), "dlFindSources");

    Settings *st = core()->settings();
    m_afs = new MenuButton(w);
    m_afs->setObjectName(QStringLiteral("dlAfs"));
    m_afs->addChoice(tr("Never"), 0, tr("Auto Find Sources - Never"));
    for (int m : {10, 15, 20, 25, 30, 40, 50, 60})
        m_afs->addChoice(tr("Every %1 min").arg(m), m, tr("Auto Find Sources - %1m").arg(m));
    m_afs->setCurrentData(st->i(kAfsKey));
    m_aeq = new MenuButton(w);
    m_aeq->setObjectName(QStringLiteral("dlAeq"));
    m_aeq->addChoice(tr("Never"), 0, tr("Auto Enter Queue - Never"));
    for (int n : {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 15, 20, 25, 30, 35, 40, 45, 50, 60, 70, 80, 90, 100})
        m_aeq->addChoice(QString::number(n), n, tr("Auto Enter Queue - %1").arg(n));
    m_aeq->setCurrentData(st->i(kAeqKey));
    // [qa-1] the row-D buttons set the default for new downloads and apply to the selected downloads
    connect(m_afs, &MenuButton::choiceChanged, this, [this](const QVariant &v) {
        core()->settings()->setValue(kAfsKey, v.toInt());
        if (v.toInt() > 0)
            core()->settings()->setValue(keys::transfers::SourceSearchIntervalMin, v.toInt());
        for (const TransferInfo &d : selectedDownloads())
            core()->transfers()->setAutoFindSources(d.id, v.toInt());
    });
    connect(m_aeq, &MenuButton::choiceChanged, this, [this](const QVariant &v) {
        core()->settings()->setValue(kAeqKey, v.toInt());
        for (const TransferInfo &d : selectedDownloads())
            core()->transfers()->setAutoEnterQueue(d.id, v.toInt());
    });
    connect(st, &Settings::changed, this, [this](const QString &k, const QVariant &v) {
        if (k == kAfsKey)
            m_afs->setCurrentData(v.toInt());
        else if (k == kAeqKey)
            m_aeq->setCurrentData(v.toInt());
    });

    // [ui-g] what each button does
    m_rowC[0]->setToolTip(tr("Remove the finished downloads from the list; their files stay on your disk (nothing is deleted, "
                             "no question)"));
    m_rowC[1]->setToolTip(tr("Continue a download from an earlier session: pick a partial file from the incomplete folder "
                             "and OctoMX looks for sources again."));
    m_rowC[2]->setToolTip(tr("Reconnect at once to every source of the downloads that failed, timed out or are queued "
                             "remotely, instead of waiting for the next try. Use it after your connection came back."));
    m_rowC[3]->setToolTip(tr("Settings for how many downloads and uploads run at the same time and how queues work"));
    m_rowC[4]->setToolTip(tr("Settings for downloads: folders, what happens when a download finishes, limits"));
    m_rowC[5]->setToolTip(tr("Open the folder where finished downloads are saved"));
    m_rowD[0]->setToolTip(tr("Stop the selected unfinished downloads and remove them from the list; you are asked whether the "
                             "partial file stays (Load Incomplete continues it) or is deleted (with a source row selected: drop "
                             "only that source)"));
    m_rowD[1]->setToolTip(tr("Remove the selected downloads from the list, finished or not; you are asked whether the file stays "
                             "or is deleted (Settings, File Transfers can remember the answer)"));
    m_retry->setToolTip(tr("Connect to the known sources of the selected downloads again now, without waiting for the next attempt"));
    m_rowD[3]->setToolTip(tr("Search all networks again for more users who have the selected downloads"));
    m_afs->setToolTip(tr("Search again for more sources of the selected downloads (and of new ones) every so many minutes "
                         "while parts are missing. Click to change the interval."));
    m_aeq->setToolTip(tr("Wait in a user's upload queue only when your place is at most this number; a longer queue is left "
                         "when the download has other sources. Applies to the selected downloads and new ones. Click to change it."));
    TransferService *ts = core()->transfers();
    connect(m_rowC[0], &QPushButton::clicked, ts, &TransferService::clearCompleted);
    connect(m_rowC[1], &QPushButton::clicked, this, &TransfersPage::loadIncomplete);
    connect(m_rowC[2], &QPushButton::clicked, this, [this] {
        // "Purge Users": reconnect every known source of the stalled / errored downloads now
        // (clears back-off timers; errored downloads are revived)
        for (const TransferInfo &t : core()->transfers()->downloads())
            if (t.state == TransferState::Error || t.state == TransferState::TimedOut || t.state == TransferState::RemotelyQueued)
                core()->transfers()->enterQueue(t.id);
    });
    connect(m_rowC[3], &QPushButton::clicked, this, [this] { mainWindow()->openSettings(QStringLiteral("Queuing")); });
    connect(m_rowC[4], &QPushButton::clicked, this, [this] { mainWindow()->openSettings(QStringLiteral("File Transfers")); });
    connect(m_rowC[5], &QPushButton::clicked, this, [this] {
        const QString dir = core()->transfers()->incomingDir();
        QDir().mkpath(dir);
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });
    connect(m_rowD[0], &QPushButton::clicked, this, &TransfersPage::cancelSelected);
    connect(m_rowD[1], &QPushButton::clicked, this, &TransfersPage::clearSelected);
    connect(m_retry, &QPushButton::clicked, this, &TransfersPage::retrySelected);
    connect(m_rowD[3], &QPushButton::clicked, this, &TransfersPage::findSourcesSelected);

    m_downModel = new TransferModel(ts, false, st, this);
    m_down = new MxListView(w);
    m_down->setObjectName(QStringLiteral("downloadList"));
    m_down->setRowHeight(14);
    m_down->setTreeMode(true);
    m_down->setModel(m_downModel);
    // [qa-1] Bytes / Status / K/s / Time Left and the File:User growth (5:1) as measured on
    // transfers_downloads_header_354b4.png at 1024 px (438 / 200 / 46 / 136 / 79 / 51 / 64)
    // [ui-e] Network (was Speed) and Size / Rate in KB / MB: the network and the status get room
    m_down->setDefaultColumnWidths({300, 170, 96, 110, 130, 62, 60});
    m_down->setFillWeights({5, 1, 2});
    m_down->setStateKey(st, QStringLiteral("downloads"));
    m_down->setFillColumns({0, 1, 4});   // File, User - AFS - AEQ, Status grow on a wide window
    // [ui-h] the rows of the last session open as "New downloads expanded by default" says
    if (st->b(keys::transfers::NewDownloadsExpanded))
        m_down->expandAll();
    m_down->installEventFilter(this);
    connect(m_down, &MxListView::contextMenuAt, this, &TransfersPage::showDownloadMenu);
    connect(m_down, &MxListView::rowActivated, this, [this](const QModelIndex &vi) {
        const QModelIndex i = m_down->mapToSource(vi);
        const TransferInfo t = m_downModel->transferFor(i);
        if (transfers::isFinished(t) && t.state != TransferState::Cancelled) {
            // [ui-e] double-click opens a completed download (dangerous types warn first);
            // [ui-h] Settings > Launch Options "Double-click on a file": open / containing folder / nothing
            const QString action = core()->settings()->s(keys::launch::DoubleClickAction);
            if (action == QLatin1String("openFolder"))
                showTransferInFolder(t);
            else if (action != QLatin1String("nothing"))
                openTransfer(t);
        }
        else if (!m_downModel->isSourceRow(i))
            m_down->setExpanded(vi, !m_down->isExpanded(vi));
    });
    connect(m_down->selectionModel(), &QItemSelectionModel::selectionChanged, this, &TransfersPage::updateButtons);
    connect(m_downModel, &QAbstractItemModel::modelReset, m_down, [this] {
        if (core()->settings()->b(keys::transfers::NewDownloadsExpanded))
            m_down->expandAll();
    });

    m_summary = new StaticText(QString(), w);
    m_summary->setObjectName(QStringLiteral("downloadSummary"));
    updateButtons();
    return w;
}

QWidget *TransfersPage::buildUploadHalf()
{
    auto *w = new QWidget(this);
    w->setObjectName(QStringLiteral("uploadHalf"));
    auto mk = [w](const QString &t, const char *name) {
        auto *b = new PushButton(t, w);
        b->setObjectName(QString::fromLatin1(name));
        return b;
    };
    m_upRow << mk(tr("Clear Complete"), "ulClearComplete") << mk(tr("Clear Incomplete"), "ulClearIncomplete")
            << mk(tr("Cancel"), "ulCancel") << mk(tr("Clear"), "ulClear") << mk(tr("Queuing..."), "ulQueuing")
            << mk(tr("Options..."), "ulOptions");
    TransferService *ts = core()->transfers();
    connect(m_upRow[0], &QPushButton::clicked, this, [this] {
        for (const TransferInfo &t : core()->transfers()->uploads())
            if (t.state == TransferState::Completed)
                core()->transfers()->clearUpload(t.id);
    });
    connect(m_upRow[1], &QPushButton::clicked, this, [this] {
        for (const TransferInfo &t : core()->transfers()->uploads())
            if (t.state == TransferState::Cancelled || t.state == TransferState::TimedOut || t.state == TransferState::Error)
                core()->transfers()->clearUpload(t.id);
    });
    connect(m_upRow[2], &QPushButton::clicked, this, [this] {
        for (const TransferInfo &t : selectedUploads())
            core()->transfers()->cancelUpload(t.id);
    });
    connect(m_upRow[3], &QPushButton::clicked, this, [this] {
        for (const TransferInfo &t : selectedUploads())
            core()->transfers()->clearUpload(t.id);
    });
    m_upRow[0]->setToolTip(tr("Remove the finished uploads from the list"));
    m_upRow[1]->setToolTip(tr("Remove the uploads that were cancelled, timed out or failed from the list"));
    m_upRow[2]->setToolTip(tr("Stop sending to the selected users"));
    m_upRow[3]->setToolTip(tr("Remove the selected uploads from the list (a running one is stopped)"));
    m_upRow[4]->setToolTip(tr("Settings for upload slots and how the queue of waiting users works"));
    m_upRow[5]->setToolTip(tr("Settings for sharing and uploads: speed limits, who may download"));
    connect(m_upRow[4], &QPushButton::clicked, this, [this] { mainWindow()->openSettings(QStringLiteral("Queuing")); });
    connect(m_upRow[5], &QPushButton::clicked, this, [this] { mainWindow()->openSettings(QStringLiteral("File Transfers")); });

    m_upModel = new TransferModel(ts, true, core()->settings(), this);
    m_up = new MxListView(w);
    m_up->setObjectName(QStringLiteral("uploadList"));
    m_up->setRowHeight(14);
    m_up->setModel(m_upModel);
    m_up->setDefaultColumnWidths({230, 110, 96, 104, 130, 62, 60});
    m_up->setStateKey(core()->settings(), QStringLiteral("uploads"));
    m_up->setFillColumns({0, 1, 4});   // File / User / Status grow; Network, Size, Rate, Time Left stay
    connect(m_up, &MxListView::deleteRequested, this, [this] {   // [qa-1] Del = Cancel
        for (const TransferInfo &t : selectedUploads())
            core()->transfers()->cancelUpload(t.id);
    });
    connect(m_up, &MxListView::contextMenuAt, this, &TransfersPage::showUploadMenu);
    return w;
}

bool TransfersPage::eventFilter(QObject *o, QEvent *e)
{
    if (o == m_down && e->type() == QEvent::KeyPress) {
        auto *k = static_cast<QKeyEvent *>(e);
        if (k->matches(QKeySequence::Paste)) {
            pasteLinks();
            return true;
        }
#ifdef Q_OS_MACOS
        const bool delKey = k->key() == Qt::Key_Delete || k->key() == Qt::Key_Backspace;   // [mac-1] the Mac's delete key
#else
        const bool delKey = k->key() == Qt::Key_Delete;
#endif
        if (delKey) {   // [ui-j] Del = Clear: the selected rows leave the list (the question)
            clearSelected();
            return true;
        }
    }
    if ((o == m_downHalf || o == m_upHalf) && e->type() == QEvent::Resize) {
        if (o == m_downHalf)
            layoutDownloadHalf();
        else
            layoutUploadHalf();
    }
    return Page::eventFilter(o, e);
}

void TransfersPage::startUpload(const TransferInfo &t)
{
    if (!core()->transfers()->startUploadNow(t.id))
        showNotice(tr("%1 cannot start this queued upload now; it starts when a slot is free.").arg(netName(t.peer.net)));
}

void TransfersPage::showNotice(const QString &text)
{
    m_notice = text;
    emit statusChanged();
    QTimer::singleShot(8000, this, [this, text] {
        if (m_notice == text) {
            m_notice.clear();
            emit statusChanged();
        }
    });
}

void TransfersPage::layoutDownloadHalf()
{
    const int w = m_downHalf->width();
    centreRow(m_rowC, {96, 93, 93, 60, 60, 169}, {0, 0, 0, 6, 6, 6}, w, 5, 19);
    QList<PushButton *> d = m_rowD;
    d << m_afs << m_aeq;
    centreRow(d, {60, 60, 60, 121, 135, 135}, {0, 0, 6, 6, 6, 0}, w, 27, 19);
    const int listTop = S(48);
    const int summaryH = S(20);
    m_down->setGeometry(0, listTop, w, qMax(S(40), m_downHalf->height() - listTop - summaryH));
    m_summary->setGeometry(S(5), m_downHalf->height() - summaryH, w - S(10), summaryH);
}

void TransfersPage::layoutUploadHalf()
{
    const int w = m_upHalf->width();
    centreRow(m_upRow, {96, 96, 130, 130, 60, 60}, {0, 0, 6, 0, 6, 6}, w, 3, 19);
    m_up->setGeometry(0, S(26), w, qMax(S(30), m_upHalf->height() - S(26)));
}

QStringList TransfersPage::statusFields() const
{
    QStringList out{m_upModel->summaryText()};
    if (!m_notice.isEmpty())
        out << m_notice;
    return out;
}

void TransfersPage::updateSummary()
{
    m_summary->setText(m_downModel->summaryText());
    emit statusChanged();
}

void TransfersPage::updateButtons()
{
    bool userRow = false;
    const QModelIndexList sel = m_down->selectedSourceRows();
    for (const QModelIndex &i : sel)
        if (m_downModel->isSourceRow(i))
            userRow = true;
    m_retry->setEnabled(userRow);
}

void TransfersPage::showEvent(QShowEvent *e)
{
    Page::showEvent(e);
    if (m_splitRestored)
        return;
    m_splitRestored = true;
    const QByteArray st = core()->settings()->value(keys::ui::splitterKey(QStringLiteral("transfers"))).toByteArray();
    if (!st.isEmpty())
        m_split->restoreState(st);
}

void TransfersPage::saveState()
{
    m_down->saveState();
    m_up->saveState();
    if (m_splitRestored)   // never overwrite the stored sizes with the defaults of a page never shown
        core()->settings()->setValue(keys::ui::splitterKey(QStringLiteral("transfers")), m_split->saveState());
}

QList<TransferInfo> TransfersPage::selectedDownloads() const
{
    QList<TransferInfo> out;
    QStringList seen;
    for (const QModelIndex &i : m_down->selectedSourceRows()) {
        const TransferInfo t = m_downModel->transferFor(i);
        if (!t.id.isEmpty() && !seen.contains(t.id)) {
            seen << t.id;
            out << t;
        }
    }
    return out;
}

QList<TransferInfo> TransfersPage::selectedUploads() const
{
    QList<TransferInfo> out;
    for (const QModelIndex &i : m_up->selectedSourceRows()) {
        const TransferInfo t = m_upModel->transferFor(i);
        if (!t.id.isEmpty())
            out << t;
    }
    return out;
}

bool TransfersPage::cancelSelectedSources()
{
    // user rows selected (and not their file row): Cancel / Clear drop just those sources
    const QModelIndexList rows = m_down->selectedSourceRows();
    QList<QPair<QString, QString>> victims;
    bool fileRow = false;
    for (const QModelIndex &i : rows) {
        if (!m_downModel->isSourceRow(i)) {
            fileRow = true;
            continue;
        }
        const TransferSource s = m_downModel->sourceFor(i);
        if (!s.id.isEmpty())
            victims << qMakePair(m_downModel->transferFor(i).id, s.id);
    }
    if (fileRow || rows.isEmpty())
        return false;
    for (const auto &v : victims)
        core()->transfers()->cancelSource(v.first, v.second);
    return true;
}

void TransfersPage::cancelSelected()
{
    if (cancelSelectedSources())
        return;
    // Cancel acts on the unfinished downloads of the selection (a finished one is left alone)
    QList<TransferInfo> sel;
    for (const TransferInfo &t : selectedDownloads())
        if (!transfers::isFinished(t))
            sel << t;
    removeDownloads(sel, RemoveWay::Cancel);
}

void TransfersPage::clearSelected()
{
    if (cancelSelectedSources())
        return;
    removeDownloads(selectedDownloads(), RemoveWay::Clear);
}

void TransfersPage::retrySelected()
{
    // "Retry" / "Enter Remote Queue": reconnect to the sources now (gets us into remote queues)
    for (const TransferInfo &t : selectedDownloads())
        core()->transfers()->enterQueue(t.id);
}

void TransfersPage::findSourcesSelected()
{
    for (const TransferInfo &t : selectedDownloads())
        if (!transfers::isFinished(t))
            core()->transfers()->findMoreSources(t.id);
}

int TransfersPage::ask(const QString &title, const QString &text, int icon)
{
    if (s_askHook)
        return s_askHook(title, text);
    QMessageBox box(QMessageBox::Icon(icon), title, text, QMessageBox::Yes | QMessageBox::No, this);
    box.setObjectName(QStringLiteral("transfersQuestion"));
    return box.exec();
}

std::function<int(const QString &, const QString &)> TransfersPage::s_askHook;

void TransfersPage::removeDownloads(const QList<TransferInfo> &sel, RemoveWay way)
{
    if (sel.isEmpty())
        return;
    TransferService *ts = core()->transfers();
    Settings *st = core()->settings();
    QList<RemoveItem> items;
    bool risky = false;   // something would be deleted for good, or a file the user chose is written into
    for (const TransferInfo &t : sel) {
        RemoveItem it{t, ts->diskInfo(t.id)};
        if (!it.disk.paths.isEmpty() && (!it.disk.trash || !it.disk.inPlaceFile.isEmpty()))
            risky = true;
        items << it;
    }
    const QString remembered = st->s(keys::transfers::RemoveAction);
    bool askNow = way == RemoveWay::DeleteFile || (remembered != QLatin1String("keep") && remembered != QLatin1String("delete"));
    if (remembered == QLatin1String("delete") && risky)
        askNow = true;   // "deleted for good" is never done without the question
    RemoveDownloadDialog::Choice choice = remembered == QLatin1String("delete") ? RemoveDownloadDialog::Delete : RemoveDownloadDialog::Keep;
    if (askNow) {
        RemoveDownloadDialog dlg(items, this);
        choice = dlg.ask();
        if (choice == RemoveDownloadDialog::Cancel)
            return;
        if (dlg.dontAskAgain())
            st->setValue(keys::transfers::RemoveAction, choice == RemoveDownloadDialog::Delete ? QStringLiteral("delete") : QStringLiteral("keep"));
    }
    // a manual resume used its file in place: the row is gone, so is the note about it
    QVariantMap resumed = st->value(QStringLiteral("ui/resumedFiles")).toMap();
    bool resumedChanged = false;
    for (const RemoveItem &it : std::as_const(items)) {
        FileRemoval mode = FileRemoval::Keep;
        if (choice == RemoveDownloadDialog::Delete && !it.disk.paths.isEmpty())
            mode = it.disk.trash ? FileRemoval::Trash : (askNow ? FileRemoval::TrashOrDelete : FileRemoval::Trash);
        ts->removeDownload(it.t.id, mode);
        resumedChanged = resumed.remove(it.t.id) > 0 || resumedChanged;
    }
    if (resumedChanged)
        st->setValue(QStringLiteral("ui/resumedFiles"), resumed);
    if (choice == RemoveDownloadDialog::Keep)
        showNotice(items.size() == 1 ? tr("Removed \"%1\" from the list; its file stays on the disk.").arg(items.first().t.name)
                                     : tr("Removed %1 downloads from the list; their files stay on the disk.").arg(items.size()));
}

void TransfersPage::onFilesDeleted(const QString &id, const QString &name, const QStringList &trashed, const QStringList &deleted,
                                   const QStringList &failed, const QStringList &errors)
{
    Q_UNUSED(id)
    if (!trashed.isEmpty())
        m_trashedNames << name;
    if (!deleted.isEmpty())
        m_deletedNames << name;
    m_failedPaths << failed;
    m_failedErrors << errors;
    m_deleteReport->start();
}

void TransfersPage::reportDeletes()
{
    const QString where = trash::placeName();
    QStringList parts;
    if (m_trashedNames.size() == 1)
        parts << tr("Moved \"%1\" to the %2.").arg(m_trashedNames.first(), where);
    else if (m_trashedNames.size() > 1)
        parts << tr("Moved the files of %1 downloads to the %2.").arg(m_trashedNames.size()).arg(where);
    if (m_deletedNames.size() == 1)
        parts << tr("Deleted \"%1\" for good.").arg(m_deletedNames.first());
    else if (m_deletedNames.size() > 1)
        parts << tr("Deleted the files of %1 downloads for good.").arg(m_deletedNames.size());
    const QStringList failed = m_failedPaths, errors = m_failedErrors;
    m_trashedNames.clear();
    m_deletedNames.clear();
    m_failedPaths.clear();
    m_failedErrors.clear();
    if (!failed.isEmpty()) {
        QStringList shown;
        for (int i = 0; i < failed.size() && i < 5; ++i)
            shown << QDir::toNativeSeparators(failed.at(i));
        if (failed.size() > 5)
            shown << tr("and %1 more").arg(failed.size() - 5);
        launchFailed(tr("These files could not be deleted and are still on the disk:\n\n%1\n\n%2")
                         .arg(shown.join(QLatin1Char('\n')), errors.value(0)));
        return;
    }
    if (!parts.isEmpty())
        showNotice(parts.join(QLatin1Char(' ')));
}

void TransfersPage::changeDestination(const TransferInfo &t)
{
    const QString dir = s_dirHook ? s_dirHook(this, QFileInfo(t.savePath).absolutePath())
                                  : QFileDialog::getExistingDirectory(this, tr("Change Destination"),
                                                                      QFileInfo(t.savePath).absolutePath());
    if (dir.isEmpty())
        return;
    if (!core()->transfers()->setDownloadTarget(t.id, dir))
        QMessageBox::information(this, tr("OctoMX"),
                                 tr("The destination of '%1' cannot be changed (finished downloads and running torrents "
                                    "keep their place).")
                                     .arg(t.name));
}

std::function<QString(QWidget *, const QString &)> TransfersPage::s_dirHook;

void TransfersPage::loadIncomplete()
{
    TransferService *ts = core()->transfers();
    LoadIncompleteDialog dlg(ts->partialFiles(), ts->incompleteDir(), this);
    if (s_loadIncompleteHook ? !s_loadIncompleteHook(&dlg) : dlg.exec() != QDialog::Accepted)
        return;
    const QString file = dlg.chosenFile();
    if (file.isEmpty())
        return;
    const QString link = dlg.link();
    if (!link.isEmpty()) {
        DownloadRequest req;
        req.uri = link;
        req.name = dlg.chosenName().isEmpty() ? QFileInfo(file).fileName() : dlg.chosenName();
        if (req.name.startsWith(QLatin1String("__INCOMPLETE___")))
            req.name = req.name.mid(15);
        const QString id = ts->resumeDownload(req, file);
        if (id.isEmpty())
            QMessageBox::warning(this, tr("OctoMX"),
                                 tr("Unable to resume from\n\n%1\n\nThe file is larger than the download, cannot be read, or "
                                    "is already used by another download.")
                                     .arg(QDir::toNativeSeparators(file)));
        else
            SearchTab::rememberResumedFile(core()->settings(), id, file);
        return;
    }
    // no link: search for the name; the double-clicked result resumes from the file
    QString name = QFileInfo(dlg.chosenName().isEmpty() ? file : dlg.chosenName()).completeBaseName();
    if (name.startsWith(QLatin1String("__INCOMPLETE___")))
        name = name.mid(15);
    mainWindow()->startSearch(name);
    if (auto *sp = qobject_cast<SearchPage *>(mainWindow()->page(QStringLiteral("search"))))
        if (SearchTab *tab = sp->currentTab())
            tab->setResumeFile(file);
}

std::function<bool(LoadIncompleteDialog *)> TransfersPage::s_loadIncompleteHook;

void TransfersPage::searchAlternates(const TransferInfo &t)
{
    QString hashLine;
    if (!t.hashes.ed2k.isEmpty())
        hashLine = QStringLiteral("HASH>%1 -%2").arg(QString::fromLatin1(t.hashes.ed2k.toHex())).arg(t.size);
    else if (!t.hashes.btih.isEmpty())
        hashLine = QStringLiteral("HASH>%1").arg(QString::fromLatin1(t.hashes.btih.toHex()));
    mainWindow()->startSearch(QFileInfo(t.name).completeBaseName(), hashLine);
}

void TransfersPage::selectBundleFiles(const TransferInfo &t)
{
    BundleFilesDialog dlg(t.name, t.files, {}, this);
    if (dlg.exec() == QDialog::Accepted)
        core()->transfers()->setWantedFiles(t.id, dlg.selectedIndexes());
}

void TransfersPage::pasteLinks()
{
    AddLinkDialog dlg(this);
    const QString clip = QGuiApplication::clipboard()->text().trimmed();
    if (clip.startsWith(QLatin1String("magnet:")) || clip.startsWith(QLatin1String("ed2k://")) ||
        clip.startsWith(QLatin1String("http")))
        dlg.setText(clip);
    // [dl-1] the order flags for the new downloads: the defaults, changeable here
    Settings *st = core()->settings();
    dlg.setOrder(st && st->b(keys::transfers::SequentialDefault), st && st->b(keys::transfers::FirstLastDefault));
    if (s_addLinkHook ? !s_addLinkHook(&dlg) : dlg.exec() != QDialog::Accepted)
        return;
    const bool seq = dlg.sequential(), fl = dlg.firstLast();
    const QMetaObject::Connection c = connect(mainWindow(), &MainWindow::linksAdded, this, [this, seq, fl](const QStringList &, const QStringList &ids) {
        for (const QString &id : ids) {
            core()->transfers()->setSequentialDownload(id, seq);
            core()->transfers()->setFirstLastPiecesFirst(id, fl);
        }
    });
    mainWindow()->addLinks(dlg.links());
    disconnect(c);
}

std::function<bool(AddLinkDialog *)> TransfersPage::s_addLinkHook;

void TransfersPage::openTorrentFile()
{
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Load Incomplete"), QString(),
                                                            tr("Torrent files (*.torrent);;All files (*)"));
    if (!files.isEmpty())
        mainWindow()->addLinks(files);
}

// [qa-1] per-download Auto Find Sources / Auto Enter Queue (engine-4: TransferService::
// setAutoFindSources / setAutoEnterQueue; TransferInfo carries the effective values, -1 = not
// reported -> the global setting). The submenus show the clicked file's value and apply to every
// selected download (menu_transfers_download_file_afs_submenu.png).
void TransfersPage::addAfsAeqMenus(QMenu *menu, const TransferInfo &t)
{
    Settings *st = core()->settings();
    const int afs = t.autoFindSourcesMin >= 0 ? t.autoFindSourcesMin : st->i(kAfsKey);
    const int aeq = t.autoEnterQueue >= 0 ? t.autoEnterQueue : st->i(kAeqKey);
    QMenu *a = menu->addMenu(afs > 0 ? tr("Auto Find Sources - %1m").arg(afs) : tr("Auto Find Sources - Never"));
    a->setObjectName(QStringLiteral("afsMenu"));
    auto *ga = new QActionGroup(a);
    auto addA = [&](const QString &text, int v) {
        QAction *x = a->addAction(text, this, [this, v] {
            for (const TransferInfo &d : selectedDownloads())
                core()->transfers()->setAutoFindSources(d.id, v);
        });
        x->setCheckable(true);
        x->setChecked(v == afs);
        ga->addAction(x);
    };
    addA(tr("Never"), 0);
    for (int m : {10, 15, 20, 25, 30, 40, 50, 60})
        addA(tr("Every %1 min").arg(m), m);
    QMenu *q = menu->addMenu(aeq > 0 ? tr("Auto Enter Queue - %1").arg(aeq) : tr("Auto Enter Queue - Never"));
    q->setObjectName(QStringLiteral("aeqMenu"));
    auto *gq = new QActionGroup(q);
    auto addQ = [&](const QString &text, int v) {
        QAction *x = q->addAction(text, this, [this, v] {
            for (const TransferInfo &d : selectedDownloads())
                core()->transfers()->setAutoEnterQueue(d.id, v);
        });
        x->setCheckable(true);
        x->setChecked(v == aeq);
        gq->addAction(x);
    };
    addQ(tr("Never"), 0);
    for (int n : {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 15, 20, 25, 30, 35, 40, 45, 50, 60, 70, 80, 90, 100})
        addQ(QString::number(n), n);
}

// [ver-1] Verify File / Restart Download... for the clicked download, or every selected one when the
// clicked one is in the selection
QList<TransferInfo> TransfersPage::actionTargets(const TransferInfo &clicked) const
{
    const QList<TransferInfo> sel = selectedDownloads();
    for (const TransferInfo &x : sel)
        if (x.id == clicked.id)
            return sel;
    return {clicked};
}

void TransfersPage::addVerifyEntries(QMenu *menu, const TransferInfo &t)
{
    TransferService *ts = core()->transfers();
    const QList<TransferInfo> targets = actionTargets(t);
    QStringList canVerify, canRestart;
    QString whyNotVerify, whyNotRestart;
    for (const TransferInfo &x : targets) {
        QString why;
        if (ts->canVerifyDownload(x.id, &why))
            canVerify << x.id;
        else if (whyNotVerify.isEmpty())
            whyNotVerify = why;
        why.clear();
        if (ts->canRestartDownload(x.id, &why))
            canRestart << x.id;
        else if (whyNotRestart.isEmpty())
            whyNotRestart = why;
    }
    QAction *v = menu->addAction(tr("Verify File"), this, [this, canVerify] { verifyDownloads(canVerify); });
    v->setObjectName(QStringLiteral("actVerifyFile"));
    v->setEnabled(!canVerify.isEmpty());
    v->setToolTip(canVerify.isEmpty() ? tr("Cannot be verified now: %1").arg(whyNotVerify)
                                      : tr("Read the file on the disk again and check every part against its checksum; only damaged "
                                           "parts are downloaded again (a running download pauses for the check and goes on by itself)"));
    QAction *r = menu->addAction(tr("Restart Download..."), this, [this, canRestart] { restartDownloads(canRestart); });
    r->setObjectName(QStringLiteral("actRestartDownload"));
    r->setEnabled(!canRestart.isEmpty());
    r->setToolTip(canRestart.isEmpty() ? tr("Cannot be restarted now: %1").arg(whyNotRestart)
                                       : tr("Throw away what was received (a finished file goes to the %1) and download the whole file "
                                            "again from the beginning; you are asked first")
                                             .arg(trash::placeName()));
}

void TransfersPage::verifyDownloads(const QStringList &ids)
{
    for (const QString &id : ids)
        core()->transfers()->verifyDownload(id);
}

QString TransfersPage::restartQuestion(const QList<TransferInfo> &items) const
{
    TransferService *ts = core()->transfers();
    const QString where = trash::placeName();
    QStringList lines;
    for (const TransferInfo &t : items) {
        const DownloadDiskInfo di = ts->diskInfo(t.id);
        const bool finished = transfers::isFinished(t);
        QString what;
        if (di.paths.isEmpty())
            what = finished ? tr("its file is not on the disk any more, nothing is deleted") : tr("nothing was received yet");
        else if (finished)
            what = di.trash ? tr("the finished file goes to the %1").arg(where)
                            : tr("the finished file is deleted for good (there is no %1 for it)").arg(where);
        else
            what = di.trash ? tr("the %1 received so far go to the %2").arg(QLocale::system().formattedDataSize(di.bytes), where)
                            : tr("the %1 received so far are deleted for good (there is no %2 for them)")
                                  .arg(QLocale::system().formattedDataSize(di.bytes), where);
        QString line = tr("\"%1\": %2").arg(t.name, what);
        if (!di.kept.isEmpty())
            line += tr("; %1 stays on the disk (%2)").arg(QDir::toNativeSeparators(di.kept.first()), di.keptWhy);
        lines << line;
    }
    const QString head = items.size() == 1 ? tr("Restart \"%1\" from the beginning?").arg(items.first().name)
                                           : tr("Restart these %1 downloads from the beginning?").arg(items.size());
    return tr("%1\n\n%2\n\nEverything is then downloaded again from zero, with the same sources, folder, name and options.")
        .arg(head, lines.join(QLatin1Char('\n')));
}

bool TransfersPage::restartDownloads(const QStringList &ids, int answer)
{
    if (ids.isEmpty())
        return false;
    QList<TransferInfo> items;
    for (const QString &id : ids) {
        const TransferInfo t = core()->transfers()->transfer(id);
        if (!t.id.isEmpty())
            items << t;
    }
    if (items.isEmpty())
        return false;
    const QString text = restartQuestion(items);
    int reply = answer;
    if (reply == 0) {
        if (s_askHook) {
            reply = s_askHook(tr("Restart Download"), text);
        } else {
            QMessageBox box(QMessageBox::Question, tr("Restart Download"), text, QMessageBox::Yes | QMessageBox::No, this);
            box.setObjectName(QStringLiteral("restartQuestion"));
            box.setDefaultButton(QMessageBox::No);   // nothing is thrown away by a hasty Enter
            reply = box.exec();
        }
    }
    if (reply != QMessageBox::Yes)
        return false;
    for (const TransferInfo &t : std::as_const(items))
        core()->transfers()->restartDownload(t.id);
    return true;
}

// [ver-1] script driver: verify-file <name part> / restart-download <name part> [yes|no]
bool TransfersPage::scriptCommand(const QString &verb, const QStringList &args, QString *error)
{
    if (verb != QLatin1String("verify-file") && verb != QLatin1String("restart-download"))
        return Page::scriptCommand(verb, args, error);
    QStringList words = args;
    int answer = 0;
    if (verb == QLatin1String("restart-download") && !words.isEmpty() &&
        (words.last() == QLatin1String("yes") || words.last() == QLatin1String("no")))
        answer = words.takeLast() == QLatin1String("yes") ? int(QMessageBox::Yes) : int(QMessageBox::No);
    const QString part = words.join(QLatin1Char(' '));
    QStringList ids;
    for (const TransferInfo &t : core()->transfers()->downloads())
        if (!part.isEmpty() && t.name.contains(part, Qt::CaseInsensitive))
            ids << t.id;
    if (ids.isEmpty()) {
        *error = QStringLiteral("no download named like '%1'").arg(part);
        return true;
    }
    if (verb == QLatin1String("verify-file")) {
        QStringList ok;
        for (const QString &id : ids)
            if (core()->transfers()->canVerifyDownload(id))
                ok << id;
        if (ok.isEmpty())
            *error = QStringLiteral("'%1' cannot be verified now").arg(part);
        verifyDownloads(ok);
        return true;
    }
    QStringList ok;
    for (const QString &id : ids)
        if (core()->transfers()->canRestartDownload(id))
            ok << id;
    if (ok.isEmpty())
        *error = QStringLiteral("'%1' cannot be restarted now").arg(part);
    else
        restartDownloads(ok, answer);
    return true;
}

void TransfersPage::showDownloadMenu(const QModelIndex &viewIndex, const QPoint &globalPos)
{
    auto *menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("downloadMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    TransferService *ts = core()->transfers();
    if (!viewIndex.isValid()) {
        menu->addAction(tr("Paste Link..."), this, &TransfersPage::pasteLinks);
        menu->addAction(tr("Open Torrent File..."), this, &TransfersPage::openTorrentFile);
        menu->addSeparator();
        menu->addAction(tr("Pause All"), ts, &TransferService::pauseAll);
        menu->addAction(tr("Resume All"), ts, &TransferService::resumeAll);
        tips::explainMenu(menu);   // [ui-g] tool tips + status bar text of the entries
        menu->popup(globalPos);
        return;
    }
    const QModelIndex i = m_down->mapToSource(viewIndex);
    const TransferInfo t = m_downModel->transferFor(i);
    const bool finished = transfers::isFinished(t);
    if (m_downModel->isSourceRow(i)) {
        const TransferSource s = m_downModel->sourceFor(i);
        const UserRef u = s.user;
        // a source without an id (BitTorrent peer) cannot be addressed: its actions are disabled
        const bool addressable = !s.id.isEmpty();
        menu->addAction(tr("Cancel"), this, &TransfersPage::cancelSelected)->setEnabled(addressable);
        menu->addAction(tr("Clear"), this, &TransfersPage::clearSelected)->setEnabled(addressable);
        // [qa-1] original WinMX user row (menu_transfers_user_row.png): Cancel / Clear / - / Bandwidth Priority ...;
        // "Retry" / "Enter Remote Queue" only appear when applicable (spec 3.6), i.e. a source that is not
        // moving data: Retry after a failure, Enter Remote Queue when it is busy / waiting
        if (!finished && s.rateBps == 0) {
            const QString st = s.statusText.toLower();
            const bool failed = st.contains(QLatin1String("timed out")) || st.contains(QLatin1String("refused")) ||
                                st.contains(QLatin1String("unavailable")) || st.contains(QLatin1String("disconnected")) ||
                                st.contains(QLatin1String("reset")) || st.contains(QLatin1String("firewalled"));
            if (failed)
                menu->addAction(tr("Retry"), this, &TransfersPage::retrySelected);
            else if (!st.contains(QLatin1String("queued")))
                menu->addAction(tr("Enter Remote Queue"), this, &TransfersPage::retrySelected);
        }
        menu->addSeparator();
        QMenu *bp = menu->addMenu(tr("Bandwidth Priority"));
        bp->setObjectName(QStringLiteral("sourcePriorityMenu"));
        addPriorityMenu(bp, s.priority, [ts, tid = t.id, sid = s.id](int p) { ts->setSourcePriority(tid, sid, p); });
        bp->setEnabled(addressable && !finished);
        menu->addSeparator();
        const bool hasUser = u.isValid() && s.net != Net::BitTorrent && s.net != Net::Http;
        menu->addAction(tr("Browse"), this, [this, u] { mainWindow()->openBrowse(u); })->setEnabled(hasUser);
        menu->addAction(tr("Send Message"), this, [this, u] { mainWindow()->openPrivateMessage(u); })->setEnabled(hasUser);
        menu->addAction(tr("Whois"), this, [this, u] { mainWindow()->openWhois(u); })->setEnabled(hasUser);
        menu->addAction(tr("Add To Hotlist"), this, [this, u] { core()->users()->addToHotlist(u); })->setEnabled(hasUser);
        menu->addAction(tr("Ping"), this, [this, u] { mainWindow()->pingUser(u); })->setEnabled(u.isValid());
        menu->addSeparator();
        menu->addAction(tr("Open"), this, [this, t] { openTransfer(t); })->setEnabled(finished);
        addLaunchEntries(menu, t, finished);   // [ui-h] Settings > File Launching (was Play / Enqueue = Open)
        tips::explainMenu(menu);   // [ui-g] tool tips + status bar text of the entries
        menu->popup(globalPos);
        return;
    }
    // [ui-j] Cancel, Clear and Delete File... remove the row(s) through the same question
    QAction *cancelAct = menu->addAction(tr("Cancel"), this, &TransfersPage::cancelSelected);
    cancelAct->setEnabled(!finished);
    cancelAct->setToolTip(tr("Stop the download and remove it from the list; you choose whether its partial file stays or is deleted"));
    menu->addAction(tr("Clear"), this, &TransfersPage::clearSelected)
        ->setToolTip(tr("Remove the download from the list; you choose whether its file stays or is deleted"));
    menu->addSeparator();
    QAction *delAct = menu->addAction(tr("Delete File..."), this, [this, t] {
        QList<TransferInfo> sel = selectedDownloads();
        bool inSel = false;
        for (const TransferInfo &x : std::as_const(sel))
            inSel = inSel || x.id == t.id;
        removeDownloads(inSel ? sel : QList<TransferInfo>{t}, RemoveWay::DeleteFile);
    });
    delAct->setObjectName(QStringLiteral("actDeleteFile"));
    delAct->setToolTip(tr("Remove the download from the list and choose to delete its file (finished or partial); you are "
                          "always asked first"));
    menu->addAction(tr("Trim 10K from end of file"))->setEnabled(false);
    menu->addSeparator();
    menu->addAction(tr("Find Sources Now"), this, &TransfersPage::findSourcesSelected)->setEnabled(!finished);
    // [diag-1] the Connection Test, scrolled to this download (UI_SPEC 8.11)
    menu->addAction(tr("Why Is This Slow?..."), this, [this, id = t.id] { ConnectionTestDialog::open(core(), this, id); })->setEnabled(!finished);
    // [qa-1] not in the original WinMX file-row menu: only offered for a failed download (revives it into the queues)
    if (t.state == TransferState::Error || t.state == TransferState::TimedOut)
        menu->addAction(tr("Enter Queue"), this, &TransfersPage::retrySelected);
    addAfsAeqMenus(menu, t);
    menu->addSeparator();
    menu->addAction(tr("Search For Alternates..."), this, [this, t] { searchAlternates(t); });
    menu->addAction(tr("Search For Similar..."), this, [this, t] { mainWindow()->startSearch(QFileInfo(t.name).completeBaseName()); });
    menu->addSeparator();
    // OctoMX additions: pause / resume / priority / bundles / links
    if (t.state == TransferState::Paused)
        menu->addAction(tr("Resume"), this, [ts, t] { ts->resume(t.id); });
    else
        menu->addAction(tr("Pause"), this, [this] {
            for (const TransferInfo &x : selectedDownloads())
                core()->transfers()->pause(x.id);
        })->setEnabled(!finished);
    addVerifyEntries(menu, t);   // [ver-1] Verify File / Restart Download...
    QMenu *prio = menu->addMenu(tr("Priority"));
    const struct { const char *t; int v; } prios[] = {{QT_TR_NOOP("High"), 1}, {QT_TR_NOOP("Normal"), 0}, {QT_TR_NOOP("Low"), -1}};
    for (const auto &p : prios) {
        QAction *a = prio->addAction(tr(p.t), this, [this, v = p.v] {
            for (const TransferInfo &x : selectedDownloads())
                core()->transfers()->setPriority(x.id, v);
        });
        a->setCheckable(true);
        a->setChecked(t.priority == p.v);
    }
    addOrderEntries(menu, t, finished);   // [dl-1] Download In Sequential Order / First And Last Pieces First
    if (t.isBundle)
        menu->addAction(tr("Select Files..."), this, [this, t] { selectBundleFiles(t); })->setEnabled(!t.files.isEmpty());
    menu->addAction(tr("Change Destination..."), this, [this, t] { changeDestination(t); })->setEnabled(!finished);
    // [ui-e] Copy Magnet Link / Copy ed2k Link / Copy Name
    const QString magnet = magnetLink(t.name, t.size, t.hashes), ed2k = ed2kLink(t.name, t.size, t.hashes);
    menu->addAction(tr("Copy Magnet Link"), this, [magnet] { QGuiApplication::clipboard()->setText(magnet); })->setEnabled(!magnet.isEmpty());
    menu->addAction(tr("Copy ed2k Link"), this, [ed2k] { QGuiApplication::clipboard()->setText(ed2k); })->setEnabled(!ed2k.isEmpty());
    menu->addAction(tr("Copy Name"), this, [n = t.name] { QGuiApplication::clipboard()->setText(n); });
    if (!finished) {
        // the classic bottom block of a download that is not complete yet: Open / Play stay greyed
        menu->addSeparator();
        const QString suffix = QFileInfo(t.name).suffix().toLower();
        const bool exe = suffix == QLatin1String("exe") || suffix == QLatin1String("msi");
        menu->addAction(tr("Open"))->setEnabled(false);
        menu->addAction(exe ? tr("Start") : tr("Play"))->setEnabled(false);
        addPreviewEntry(menu, t);   // [dl-1] Preview of the partial audio / video file
        menu->addAction(tr("Open Containing Folder"), this, [this, t] { showTransferInFolder(t); });
    } else {
        // [qa-2] complete: only the two entries at the top. The classic Open / Play did the same as
        // Open File, and a second Open Containing Folder was a duplicate.
        // [ui-e] a completed download: Open File (default, = double-click) / Open Containing Folder first
        QAction *first = menu->actions().value(0);
        QAction *openFile = new QAction(t.isBundle ? tr("Open Folder") : tr("Open File"), menu);
        openFile->setObjectName(QStringLiteral("actOpenFile"));
        connect(openFile, &QAction::triggered, this, [this, t] { openTransfer(t); });
        QAction *showIt = new QAction(tr("Open Containing Folder"), menu);
        showIt->setObjectName(QStringLiteral("actShowInFolder"));
        connect(showIt, &QAction::triggered, this, [this, t] { showTransferInFolder(t); });
        menu->insertAction(first, openFile);
        menu->insertAction(first, showIt);
        // [ui-h] the File Launching entries (Settings) that have a program, or a Windows verb
        if (!t.isBundle && !t.savePath.isEmpty()) {
            for (const QVariantMap &e : launcher::menuEntries(core()->settings(), t.savePath)) {
                QAction *a = new QAction(e.value(QStringLiteral("title")).toString(), menu);
                a->setObjectName(QStringLiteral("actLaunch_") + e.value(QStringLiteral("title")).toString());
                connect(a, &QAction::triggered, this, [this, e, path = t.savePath] {
                    QString why;
                    if (!launcher::runEntry(this, core()->settings(), e, path, &why) && !why.isEmpty())
                        launchFailed(why);
                });
                menu->insertAction(first, a);
            }
        }
        QAction *afterTop = menu->insertSeparator(first);
        menu->setDefaultAction(openFile);
        // [dl-1] a completed audio / video file: the preview player too (Open File keeps the system default)
        if (!t.isBundle && preview::isMediaFile(t.name) && !t.savePath.isEmpty()) {
            QAction *withPlayer = new QAction(tr("Open With Preview Player"), menu);
            withPlayer->setObjectName(QStringLiteral("actOpenWithPreviewPlayer"));
            withPlayer->setToolTip(tr("Play the file with the player chosen for previews (Settings, File Launching, Preview Player)."));
            connect(withPlayer, &QAction::triggered, this, [this, t] { previewTransfer(t, -1); });
            menu->insertAction(afterTop, withPlayer);   // after Open Containing Folder
        }
    }
    tips::explainMenu(menu);   // [ui-g] tool tips + status bar text of the entries
    menu->popup(globalPos);
}

// [dl-1] the two order flags of qBittorrent, for every selected download (checked = all of them
// have it; a click switches it for all). Priorities, never conditions (TransferService).
void TransfersPage::addOrderEntries(QMenu *menu, const TransferInfo &t, bool finished)
{
    QList<TransferInfo> sel;
    for (const TransferInfo &x : selectedDownloads())
        if (!transfers::isFinished(x))
            sel << x;
    if (sel.isEmpty() && !finished)
        sel << t;
    bool allSeq = !sel.isEmpty(), allFl = !sel.isEmpty();
    for (const TransferInfo &x : std::as_const(sel)) {
        allSeq = allSeq && x.sequential;
        allFl = allFl && x.firstLastFirst;
    }
    QStringList ids;
    for (const TransferInfo &x : std::as_const(sel))
        ids << x.id;
    QAction *seq = menu->addAction(tr("Download In Sequential Order"), this, [this, ids, allSeq] {
        for (const QString &id : ids)
            core()->transfers()->setSequentialDownload(id, !allSeq);
    });
    seq->setObjectName(QStringLiteral("actSequential"));
    seq->setCheckable(true);
    seq->setChecked(allSeq);
    seq->setEnabled(!ids.isEmpty());
    seq->setToolTip(tr("Fetch the file from its beginning to its end, as far as the sources allow: each source is asked for the "
                       "lowest part it has that is still missing. A priority only: parts no source has right now never hold the "
                       "download up."));
    QAction *fl = menu->addAction(tr("Download First And Last Pieces First"), this, [this, ids, allFl] {
        for (const QString &id : ids)
            core()->transfers()->setFirstLastPiecesFirst(id, !allFl);
    });
    fl->setObjectName(QStringLiteral("actFirstLast"));
    fl->setCheckable(true);
    fl->setChecked(allFl);
    fl->setEnabled(!ids.isEmpty());
    fl->setToolTip(tr("Fetch the beginning and the end of the file first (1 %, at least 4 MB each), where players and video "
                      "containers keep what they need to start, so the file can be previewed early."));
}

void TransfersPage::addPreviewEntry(QMenu *menu, const TransferInfo &t)
{
    const previewui::MenuEntry e = previewui::menuEntry(core(), t);
    if (!e.show)
        return;
    QAction *a = menu->addAction(e.text);
    a->setObjectName(QStringLiteral("actPreview"));
    a->setEnabled(e.enabled);
    a->setToolTip(e.tip);
    a->setStatusTip(e.tip);
    if (e.ready) {
        connect(a, &QAction::triggered, this, [this, t] { previewTransfer(t, -2); });
    } else {
        connect(a, &QAction::triggered, this, [this, t] {
            // not playable yet: fetch the beginning and the end first, so it soon is
            core()->transfers()->setSequentialDownload(t.id, true);
            core()->transfers()->setFirstLastPiecesFirst(t.id, true);
            showNotice(tr("\"%1\" now downloads its beginning and end first; Preview becomes possible when enough of it is on disk.")
                           .arg(t.name));
        });
    }
}

void TransfersPage::previewTransfer(const TransferInfo &t, int fileIndex)
{
    QString why;
    if (!previewui::run(this, core(), t, fileIndex, &why) && !why.isEmpty())
        launchFailed(why);
}

void TransfersPage::addLaunchEntries(QMenu *menu, const TransferInfo &t, bool finished)
{
    if (t.isBundle || t.savePath.isEmpty())
        return;
    for (const QVariantMap &e : launcher::menuEntries(core()->settings(), t.savePath)) {
        QAction *a = menu->addAction(e.value(QStringLiteral("title")).toString(), this, [this, e, path = t.savePath] {
            QString why;
            if (!launcher::runEntry(this, core()->settings(), e, path, &why) && !why.isEmpty())
                launchFailed(why);
        });
        a->setEnabled(finished);
    }
}

void TransfersPage::openTransfer(const TransferInfo &t)
{
    if (t.savePath.isEmpty()) {   // an engine that does not report the path: its own launcher
        core()->transfers()->openFile(t.id);
        return;
    }
    QString why;
    const bool ok = t.isBundle || QFileInfo(t.savePath).isDir() ? launcher::showInFolder(t.savePath, &why)
                                                                : launcher::openFile(this, core()->settings(), t.savePath, &why);
    if (!ok && !why.isEmpty())
        launchFailed(why);
}

void TransfersPage::showTransferInFolder(const TransferInfo &t)
{
    if (t.savePath.isEmpty()) {
        core()->transfers()->openFolder(t.id);
        return;
    }
    QString why;
    if (!launcher::showInFolder(t.savePath, &why))
        launchFailed(why);
}

void TransfersPage::launchFailed(const QString &why)
{
    // [ui-e] never silent: the status bar and a classic message box say why
    showNotice(why);
    if (s_messageHook) {
        s_messageHook(why);
        return;
    }
    QMessageBox box(QMessageBox::Information, tr("OctoMX"), why, QMessageBox::Ok, this);
    box.setObjectName(QStringLiteral("launchProblem"));
    box.exec();
}

std::function<void(const QString &)> TransfersPage::s_messageHook;

void TransfersPage::showUploadMenu(const QModelIndex &viewIndex, const QPoint &globalPos)
{
    if (!viewIndex.isValid())
        return;
    const TransferInfo t = m_upModel->transferFor(m_up->mapToSource(viewIndex));
    const UserRef u = t.peer;
    auto *menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("uploadMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    TransferService *ts = core()->transfers();
    const bool queued = t.state == TransferState::Queued || t.state == TransferState::RemotelyQueued;
    const bool active = !transfers::isFinished(t);
    menu->addAction(tr("Start Transfer"), this, [this, t] { startUpload(t); })->setEnabled(queued);
    menu->addAction(tr("Cancel"), this, [ts, t] { ts->cancelUpload(t.id); })->setEnabled(active);
    menu->addAction(tr("Clear"), this, [ts, t] { ts->clearUpload(t.id); });
    menu->addSeparator();
    QMenu *bp = menu->addMenu(tr("Bandwidth Priority"));
    bp->setObjectName(QStringLiteral("uploadPriorityMenu"));
    addPriorityMenu(bp, t.priority, [this, t](int p) {
        if (!core()->transfers()->setUploadPriority(t.id, p))
            showNotice(tr("The priority of this upload cannot be changed on %1.").arg(netName(t.peer.net)));
    });
    bp->setEnabled(active);
    menu->addSeparator();
    const bool hasUser = u.isValid();
    menu->addAction(tr("Browse"), this, [this, u] { mainWindow()->openBrowse(u); })->setEnabled(hasUser);
    menu->addAction(tr("Send Message"), this, [this, u] { mainWindow()->openPrivateMessage(u); })->setEnabled(hasUser);
    menu->addAction(tr("Whois"), this, [this, u] { mainWindow()->openWhois(u); })->setEnabled(hasUser);
    menu->addAction(tr("Add To Hotlist"), this, [this, u] { core()->users()->addToHotlist(u); })->setEnabled(hasUser);
    menu->addSeparator();
    menu->addAction(tr("Open"))->setEnabled(false);
    menu->addAction(tr("Play"))->setEnabled(false);
    menu->addAction(tr("Enqueue"))->setEnabled(false);   // [qa-1] menu_transfers_upload_bandwidth_priority.png
    tips::explainMenu(menu);   // [ui-g] tool tips + status bar text of the entries
    menu->popup(globalPos);
}

} // namespace mx::ui

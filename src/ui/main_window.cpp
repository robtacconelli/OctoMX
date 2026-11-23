// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/main_window.h"

#include <QtGui/QStatusTipEvent>

#include <memory>
#include "ui/update_ui.h"
#include "ui/os_integration.h"
#include "ui/tray.h"

#include <QtCore/QDateTime>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFileInfo>
#include <QtCore/QMimeData>
#include <QtCore/QTimer>
#include <QtWidgets/QMenu>
#include <QtCore/QUrl>
#include <QtGui/QCloseEvent>
#include <QtGui/QDragEnterEvent>
#include <QtGui/QDropEvent>
#include <QtGui/QKeyEvent>
#include <QtGui/QScreen>
#include <QtGui/QShortcut>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QSystemTrayIcon>
#include <QtWidgets/QVBoxLayout>

#include "mx/services.h"
#include "mx/settings_keys.h"
#include "ui/dialogs/about_dialog.h"
#include "ui/dialogs/legal_notice.h"
#include "ui/hotlist_pane.h"
#include "ui/kit/colors.h"
#include "ui/kit/format.h"
#include "ui/kit/icons.h"
#include "ui/kit/list_view.h"
#include "ui/kit/scale.h"
#include "ui/kit/session.h"
#include "ui/kit/theme.h"
#include "ui/kit/status_bar.h"
#include "ui/kit/widgets.h"
#include "ui/pages/networks_page.h"
#include "ui/pages/page.h"
#include "ui/pages/search_page.h"
#include "ui/pages/transfers_page.h"
#include "ui/pages/bandwidth_page.h"   // [ui-b]
#include "ui/pages/chat_page.h"        // [ui-b]
#include "ui/pages/shared_page.h"      // [ui-b]
#include "ui/speed_limits.h"
#include "ui/kit/sound.h"   // [ui-h]

namespace mx::ui {

namespace {
constexpr int kRowAY = 3;
constexpr int kRowBY = 27;
constexpr int kRowH = 21;
constexpr int kPageAreaY = 48;
constexpr int kStatusH = 20;

bool isLink(const QString &s)
{
    return s.startsWith(QLatin1String("magnet:"), Qt::CaseInsensitive) ||
           s.startsWith(QLatin1String("ed2k://"), Qt::CaseInsensitive) ||
           s.startsWith(QLatin1String("http://"), Qt::CaseInsensitive) ||
           s.startsWith(QLatin1String("https://"), Qt::CaseInsensitive) ||
           s.endsWith(QLatin1String(".torrent"), Qt::CaseInsensitive);
}
} // namespace

QStringList MainWindow::pageIds()
{
    // the order of the page buttons (owner's choice; WinMX 3.53 had Chat third and the
    // Hotlist button between Search and Transfers)
    return {QStringLiteral("networks"), QStringLiteral("shared"), QStringLiteral("search"), QStringLiteral("transfers"),
            QStringLiteral("bandwidth"), QStringLiteral("chat")};
}

void registerUieScriptVerbs();   // main_window_e.cpp

MainWindow::MainWindow(mx::Core *core, QWidget *parent) : QWidget(parent), m_core(core)
{
    registerUieScriptVerbs();
    setObjectName(QStringLiteral("mainWindow"));
    setWindowIcon(appIcon());
    setAutoFillBackground(true);
    // [qa-1] the original could be made narrower than its 682-px button band (networks_wpn_tab_354b3.png
    // is 643 px wide, the band is clipped): rows A/B and the pages simply clip below their natural size
    setMinimumSize(SS(400, 300));
    resize(SS(800, 600));
    if (isScaled()) {
        // [qa-2] above 100 % the window is never narrower than the button band: centred button rows
        // were cut on both sides (Transfers: "el" for Cancel, the right page buttons gone) at 175 %
        setMinimumWidth(S(682));
        // the first-run size scales too, but never beyond the screen's work area (1280x720 at
        // 125 % would put the status bar under the taskbar); a title bar and borders are left room for
        if (QScreen *sc = QGuiApplication::primaryScreen()) {
            const QSize avail = sc->availableGeometry().size() - SS(8, 31);
            if (width() > avail.width() || height() > avail.height())
                resize(size().boundedTo(avail).expandedTo(minimumSize()));
        }
    }
    m_hotlistWidth = S(200);
    setAcceptDrops(true);
    migrateLegacySettings(core->settings());   // idempotent; the app also calls it before the wizard
    Fonts::instance()->attach(core->settings());   // Settings ▸ Appearance fonts, live
    // [ui-e] transfer slots / queues follow the connection type (values the user set are kept)
    applyConnectionDefaults(core->settings());
    connect(core->settings(), &Settings::changed, this, [this](const QString &key) {
        if (key == QLatin1String(keys::identity::Connection)) {
            const QStringList changed = applyConnectionDefaults(m_core->settings());
            if (!changed.isEmpty())
                showNotice(tr("Upload slots, downloads and queue set for a %1 connection.").arg(m_core->settings()->value(keys::identity::Connection).toString()));
        }
    });
    // [ui-h] Settings > Interface and Logs "Always on top", live
    applyAlwaysOnTop();
    connect(core->settings(), &Settings::changed, this, [this](const QString &key) {
        if (key == QLatin1String(keys::misc::AlwaysOnTop))
            applyAlwaysOnTop();
    });
    // [ui-e] "Show exact byte counts" (classic sizes / rates / times), live in every list and status line
    fmt::setExactSizes(core->settings()->b(keys::ui::ExactSizes));
    connect(core->settings(), &Settings::changed, this, [this](const QString &key, const QVariant &v) {
        if (key != QLatin1String(keys::ui::ExactSizes))
            return;
        fmt::setExactSizes(v.toBool());
        for (QWidget *w : QApplication::allWidgets()) {
            if (auto *view = qobject_cast<QAbstractItemView *>(w)) {
                if (QAbstractItemModel *m = view->model())
                    if (m->columnCount() > 0)
                        emit m->headerDataChanged(Qt::Horizontal, 0, m->columnCount() - 1);
                view->viewport()->update();
            }
        }
        for (Page *p : std::as_const(m_pages))
            emit p->statusChanged();
        updateTitle();
    });
    createChrome();
    createPages();

    // [ui-g] a Hotlist user who comes online: status-bar note + tray notification (hotlist/notifyOnline,
    // "Notify when Hot List users come online"; the setting existed but nothing read it)
    {
        // per entry: last online state and when the entry appeared (a user just added, whose state is
        // first learned, did not "come online": no notice in the first 20 s)
        struct Seen { bool online = false; qint64 sinceMs = 0; };
        auto seen = std::make_shared<QHash<QString, Seen>>();
        for (const HotlistEntry &e : core->users()->hotlist())
            seen->insert(e.user.key(), Seen{e.online, 0});
        connect(core->users(), &UserService::hotlistChanged, this, [this, seen] {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            for (const HotlistEntry &e : m_core->users()->hotlist()) {
                const QString k = e.user.key();
                if (!seen->contains(k)) {
                    seen->insert(k, Seen{e.online, now});
                    continue;
                }
                Seen &s = (*seen)[k];
                const bool cameOnline = e.online && !s.online && now - s.sinceMs > 20000;
                s.online = e.online;
                if (cameOnline)
                    notifyHotlistOnline(e.user.name.isEmpty() ? e.user.id : e.user.name);
            }
        });
    }
    // [ui-e] a download that completes: tray notification (ui/notifyDownloadDone) + status-bar note
    for (const TransferInfo &t : core->transfers()->downloads())
        if (t.state == TransferState::Completed || t.state == TransferState::Seeding)
            m_completedIds.insert(t.id);
    auto onTransfer = [this](const TransferInfo &t) {
        if (t.isUpload || (t.state != TransferState::Completed && t.state != TransferState::Seeding) || m_completedIds.contains(t.id))
            return;
        m_completedIds.insert(t.id);
        notifyDownloadDone(t);
    };
    connect(core->transfers(), &TransferService::transferChanged, this, onTransfer);
    connect(core->transfers(), &TransferService::transferAdded, this, onTransfer);
    // [ui-h] Settings > File Transfers "Switch to Transfers window when starting downloads": a download
    // added while OctoMX runs (not the ones of the last session the core restores at start)
    m_startedAt.start();
    connect(core->transfers(), &TransferService::transferAdded, this, [this](const TransferInfo &t) {
        if (t.isUpload || m_startedAt.elapsed() < 2000 || !m_core->settings()->b(keys::transfers::SwitchToTransfers))
            return;
        if (currentPageId() != QLatin1String("transfers"))
            showPage(QStringLiteral("transfers"));
    });

    // [qa-1] a download request the core could not start is never silent
    connect(core->transfers(), &TransferService::downloadNotStarted, this, &MainWindow::reportDownloadProblem);

    NetworkService *net = core->network();
    connect(net, &NetworkService::overallChanged, this, [this] { updateTitle(); updateNews(); });
    connect(net, &NetworkService::networkChanged, this, [this] { updateTitle(); updateNews(); });
    connect(net, &NetworkService::summaryChanged, this, &MainWindow::updateTitle);
    connect(net, &NetworkService::userPinged, this, [this](const UserRef &u, int ms) {
        if (!m_pinging.remove(u.key()))
            return;   // somebody else's ping
        const QString who = u.name.isEmpty() ? u.id : u.name;
        showNotice(ms >= 0 ? tr("Ping reply from %1: %2 ms").arg(who).arg(ms) : tr("No ping reply from %1").arg(who));
    });
    connect(core->transfers(), &TransferService::transferChanged, this, [this](const TransferInfo &t) {
        if (t.isUpload)
            updateTitle();
    });
    m_titleTimer = new QTimer(this);
    m_titleTimer->setInterval(1000);
    connect(m_titleTimer, &QTimer::timeout, this, &MainWindow::updateTitle);
    m_titleTimer->start();

    restoreState();
    updateTitle();
    updateNews();
    setupTray();
    // [post-1] Check for Updates (the automatic one at most once a day, never in demo / test runs)
    (new UpdateController(this))->scheduleStartCheck();
}

MainWindow::~MainWindow() = default;

void MainWindow::createChrome()
{
    m_news = new NewsBar(this);
    m_ignoredBtn = new ThinButton(IconId::Ignored, tr("Ignored"), this);
    m_ignoredBtn->setObjectName(QStringLiteral("btnIgnored"));
    m_settingsBtn = new ThinButton(IconId::Settings, tr("Settings"), this);
    m_settingsBtn->setObjectName(QStringLiteral("btnSettings"));
    m_helpBtn = new ThinButton(IconId::Help, tr("Help"), this);
    m_helpBtn->setObjectName(QStringLiteral("btnHelp"));
    // [ui-g] what each button does
    m_ignoredBtn->setToolTip(tr("The users whose chat lines and messages you hide; add or remove them here"));
    m_settingsBtn->setToolTip(tr("All the options of OctoMX: connection, folders, sharing, chat, appearance. "
                                 "Right-click for the colour theme."));
    m_helpBtn->setToolTip(tr("About OctoMX (version, licence) and Check for Updates"));
    connect(m_ignoredBtn, &QAbstractButton::clicked, this, &MainWindow::openIgnored);
    connect(m_settingsBtn, &QAbstractButton::clicked, this, [this] { openSettings(); });
    // [post-1] right click on Settings: a small menu with the colour theme (the click stays the classic one)
    m_settingsBtn->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_settingsBtn, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QMenu *m = settingsButtonMenu();
        m->popup(m_settingsBtn->mapToGlobal(pos));
    });
    // [post-1] the Help button pops a small menu: About OctoMX... (bold default) / Check for Updates...
    // (F1 still opens About directly)
    connect(m_helpBtn, &QAbstractButton::clicked, this, [this] {
        // right-aligned under the button: Help is the last button of row A
        QMenu *m = helpMenu();
        m->popup(m_helpBtn->mapToGlobal(QPoint(m_helpBtn->width() - m->sizeHint().width(), m_helpBtn->height())));
    });

    struct B { const char *id; IconId icon; const char *text; const char *tip; };
    const B buttons[] = {
        {"networks", IconId::Networks, QT_TR_NOOP("Networks"),
         QT_TR_NOOP("The networks OctoMX is connected to (OctoMX, BitTorrent, eD2k, Kad, Gnutella, web indexes): connect, disconnect, status, Connection Test (Ctrl+1)")},
        {"shared", IconId::SharedFiles, QT_TR_NOOP("Shared Files"),
         QT_TR_NOOP("The folders and files other users may download from you (Ctrl+2)")},
        {"search", IconId::Search, QT_TR_NOOP("Search"), QT_TR_NOOP("Search every network for files and download them (Ctrl+3)")},
        {"transfers", IconId::Transfers, QT_TR_NOOP("Transfers"), QT_TR_NOOP("Your downloads and uploads with their progress and sources (Ctrl+4)")},
        {"bandwidth", IconId::Bandwidth, QT_TR_NOOP("Bandwidth"), QT_TR_NOOP("Graphs of the incoming and outgoing speed (Ctrl+5)")},
        {"chat", IconId::Chat, QT_TR_NOOP("Chat"), QT_TR_NOOP("Chat rooms of the OctoMX network: list the rooms, join, talk (Ctrl+6)")},
        {"hotlist", IconId::Hotlist, QT_TR_NOOP("Hotlist"),
         QT_TR_NOOP("Show or hide the Hotlist beside the page: your saved users, online or offline (Ctrl+7)")},
    };
    for (const B &b : buttons) {
        const QString id = QString::fromLatin1(b.id);
        auto *btn = new ThinButton(b.icon, tr(b.text), this);
        btn->setObjectName(QStringLiteral("btn_") + id);
        btn->setToolTip(tr(b.tip));
        m_pageBtnList << btn;
        m_pageBtns.insert(id, btn);
        if (id == QLatin1String("hotlist"))
            connect(btn, &QAbstractButton::clicked, this, [this] { setHotlistVisible(!isHotlistVisible()); });
        else
            connect(btn, &QAbstractButton::clicked, this, [this, id] { showPage(id); });
    }

    m_split = new QSplitter(Qt::Horizontal, this);
    m_split->setObjectName(QStringLiteral("hotlistSplitter"));
    m_split->setChildrenCollapsible(false);
    m_split->setHandleWidth(S(4));
    m_stack = new QStackedWidget(m_split);
    m_stack->setObjectName(QStringLiteral("pageStack"));
    m_hotlist = new HotlistPane(m_core, this, m_split);
    m_hotlist->hide();
    m_split->addWidget(m_stack);
    m_split->addWidget(m_hotlist);
    m_split->setStretchFactor(0, 1);
    m_split->setStretchFactor(1, 0);
    connect(m_split, &QSplitter::splitterMoved, this, [this] {
        if (m_hotlist->isVisible())
            m_hotlistWidth = m_hotlist->width();
    });

    m_status = new StatusBar(this);
    connect(m_status->floatTab(), &FloatTab::clicked, this, [this] { floatPage(m_current); });

    // keyboard shortcuts (documented in src/ui/README.md)
    const QStringList ids = pageIds();
    const QList<QString> order = {QStringLiteral("networks"), QStringLiteral("shared"), QStringLiteral("search"),
                                  QStringLiteral("transfers"), QStringLiteral("bandwidth"), QStringLiteral("chat"),
                                  QStringLiteral("hotlist")};
    for (int i = 0; i < order.size(); ++i) {
        auto *sc = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key(Qt::Key_1 + i)), this);
        const QString id = order.at(i);
        connect(sc, &QShortcut::activated, this, [this, id] {
            if (id == QLatin1String("hotlist"))
                setHotlistVisible(!isHotlistVisible());
            else
                showPage(id);
        });
    }
    auto *help = new QShortcut(QKeySequence(Qt::Key_F1), this);
    connect(help, &QShortcut::activated, this, &MainWindow::openHelp);
    auto *find = new QShortcut(QKeySequence::Find, this);
    // [ui-e] Ctrl+F: the type-to-filter box of the list in front (Search results, Shared Files, channel
    // list); on a page without one it goes to Search as before
    connect(find, &QShortcut::activated, this, [this] {
        MxListView *target = nullptr;
        for (QWidget *w = QApplication::focusWidget(); w && !target; w = w->parentWidget())
            if (auto *l = qobject_cast<MxListView *>(w))
                target = l->filterEnabled() ? l : nullptr;
        if (!target)
            if (Page *p = page(m_current))
                for (MxListView *l : p->findChildren<MxListView *>())
                    if (l->filterEnabled() && l->isVisibleTo(this) && l->isVisible()) {
                        target = l;
                        break;
                    }
        if (target)
            target->showFilter();
        else
            showPage(QStringLiteral("search"));
    });
    auto *paste = new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V), this);
    connect(paste, &QShortcut::activated, this, [this] {
        if (auto *t = qobject_cast<TransfersPage *>(page(QStringLiteral("transfers"))))
            t->pasteLinks();
    });
}

void MainWindow::createPages()
{
    // [ui-b] replace the placeholders for "shared", "chat" and "bandwidth" with the real pages.
    addPage(new NetworksPage(m_core, this), IconId::Networks, tr("Networks"));
    addPage(new SharedPage(m_core, this), IconId::SharedFiles, tr("Shared Files"));   // [ui-b]
    addPage(new ChatPage(m_core, this), IconId::Chat, tr("Chat"));                     // [ui-b]
    addPage(new SearchPage(m_core, this), IconId::Search, tr("Search"));
    addPage(new TransfersPage(m_core, this), IconId::Transfers, tr("Transfers"));
    addPage(new BandwidthPage(m_core, this), IconId::Bandwidth, tr("Bandwidth"));     // [ui-b]
}

void MainWindow::addPage(Page *p, IconId, const QString &)
{
    auto *slot = new QWidget(m_stack);
    slot->setObjectName(QStringLiteral("slot_") + p->pageId());
    auto *lay = new QVBoxLayout(slot);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    p->setParent(slot);
    lay->addWidget(p);
    m_stack->addWidget(slot);
    m_slots.insert(p->pageId(), slot);
    m_pages.insert(p->pageId(), p);
    connect(p, &Page::statusChanged, this, [this, p] {
        if (p->pageId() == m_current && !isFloating(m_current))
            updateStatus();
    });
}

Page *MainWindow::page(const QString &id) const
{
    return m_pages.value(id);
}

ThinButton *MainWindow::pageButton(const QString &id) const
{
    return m_pageBtns.value(id);
}

void MainWindow::showPage(const QString &id)
{
    if (id == QLatin1String("hotlist")) {
        setHotlistVisible(true);
        return;
    }
    if (!m_pages.contains(id))
        return;
    if (FloatWindow *fw = m_floats.value(id)) {
        fw->show();
        fw->raise();
        fw->activateWindow();
    }
    const bool changed = m_current != id;
    m_current = id;
    m_stack->setCurrentWidget(m_slots.value(id));
    updatePageButtons();
    updateStatus();
    m_status->floatTab()->setVisible(!isFloating(id));
    if (!isFloating(id))
        m_pages.value(id)->activated();
    if (changed) {
        if (!m_restoring && m_core->settings())
            m_core->settings()->setValue(keys::ui::LastPage, id);
        emit pageChanged(id);
    }
}

void MainWindow::updatePageButtons()
{
    for (auto it = m_pageBtns.begin(); it != m_pageBtns.end(); ++it) {
        if (it.key() == QLatin1String("hotlist"))
            it.value()->setSelected(isHotlistVisible());
        else
            it.value()->setSelected(it.key() == m_current);
    }
}

void MainWindow::setHotlistVisible(bool on)
{
    if (on == isHotlistVisible())
        return;
    if (on) {
        m_hotlist->show();
        const int total = m_split->width();
        m_split->setSizes({qMax(S(100), total - m_hotlistWidth - m_split->handleWidth()), m_hotlistWidth});
    } else {
        m_hotlistWidth = m_hotlist->width();
        m_hotlist->hide();
    }
    updatePageButtons();
    if (!m_restoring && m_core->settings())
        m_core->settings()->setValue(QStringLiteral("ui/hotlistVisible"), on);
}

bool MainWindow::isHotlistVisible() const
{
    return m_hotlist && !m_hotlist->isHidden();
}

void MainWindow::floatPage(const QString &id)
{
    Page *p = m_pages.value(id);
    if (!p || m_floats.contains(id))
        return;
    auto *fw = new FloatWindow(p, nullptr);
    fw->setObjectName(QStringLiteral("float_") + id);
    m_floats.insert(id, fw);
    connect(fw, &FloatWindow::dockRequested, this, [this, id] { dockPage(id); });
    if (!fw->restorePlacement()) {
        fw->resize(qMax(S(500), m_stack->width()), qMax(S(300), m_stack->height() + S(kStatusH)));
        fw->move(mapToGlobal(SP(40, 60)));
    }
    fw->show();
    p->show();
    p->activated();
    m_status->floatTab()->setVisible(id != m_current ? true : false);
    updateStatus();
}

void MainWindow::dockPage(const QString &id)
{
    FloatWindow *fw = m_floats.take(id);
    if (!fw)
        return;
    fw->savePlacement();
    Page *p = fw->takePage();
    QWidget *slot = m_slots.value(id);
    p->setParent(slot);
    slot->layout()->addWidget(p);
    p->show();
    fw->deleteLater();
    if (id == m_current) {
        m_status->floatTab()->setVisible(true);
        p->activated();
    }
    updateStatus();
}

QString MainWindow::titleText() const
{
    QString ul;
    const QList<TransferInfo> ups = m_core->transfers()->uploads();
    int n = 0;
    qint64 rate = 0;
    for (const TransferInfo &t : ups)
        if (t.state == TransferState::Transferring) {
            ++n;
            rate += t.rateUpBps;
        }
    if (n > 0) {
        QChar sep = fmt::groupSeparator();
        Q_UNUSED(sep);
        ul = fmt::exactSizes() ? tr(" - %1 UL @ %2 B/s").arg(n).arg(fmt::number(rate)) : tr(" - %1 UL @ %2").arg(n).arg(fmt::rate(rate));
    }
    // [bw-1] a speed limit in force is never a mystery: " - DL limit 500 KB/s, UL limit 50 KB/s"
    const QString limits = speed::statusText(m_core->settings());
    if (!limits.isEmpty())
        ul += QStringLiteral(" - ") + limits;
    return QStringLiteral("%1 v%2%3  <%4>")
        .arg(QStringLiteral(MX_APP_NAME), m_core->versionString(), ul, stateText());
}

QString MainWindow::stateText() const
{
    // UI_SPEC §1.1 grammar from NetworkService::summary():
    //   <Online P=<primary> S=<sources> spm=<searches/min>>  primary connection
    //   <Online h:mm:ss>                                     secondary (connected time)
    //   <Connecting> / <Offline>, then  <Networks: n nets online, m pending  u users f files>
    const NetworkSummary sum = m_core->network()->summary();
    QString state;
    if (sum.state == ConnState::Connected) {
        if (!m_onlineSince.isValid())
            m_onlineSince.start();
        if (m_core->settings()->b(keys::identity::PrimaryConnection))
            state = QStringLiteral("Online P=%1 S=%2 spm=%3").arg(sum.primary).arg(sum.secondary).arg(sum.searchesPerMinute);   // [ui-e] S = secondary connections
        else
            state = QStringLiteral("Online %1").arg(fmt::duration(int(m_onlineSince.elapsed() / 1000)));
    } else {
        m_onlineSince.invalidate();
        state = sum.state == ConnState::Connecting ? QStringLiteral("Connecting") : QStringLiteral("Offline");
    }
    if (sum.networksEnabled > 1 && sum.state != ConnState::Disabled) {
        QString nets = QStringLiteral("Networks: %1 nets online, %2 pending").arg(sum.networksOnline).arg(sum.networksEnabled - sum.networksOnline);
        if (sum.usersOnline > 0)
            nets += QStringLiteral(" %1 users").arg(sum.usersOnline);
        if (sum.filesOnline > 0)
            nets += QStringLiteral(" %1 files").arg(sum.filesOnline);
        state += QStringLiteral(">  <") + nets;
    }
    return state;
}

void MainWindow::pingUser(const UserRef &u)
{
    if (!u.isValid())
        return;
    m_pinging.insert(u.key());
    showNotice(tr("Pinging %1...").arg(u.name.isEmpty() ? u.id : u.name));
    m_core->network()->pingUser(u);
}

bool MainWindow::event(QEvent *e)
{
    // [ui-g] a hovered menu entry (or button) with a status tip explains itself in the status bar
    if (e->type() == QEvent::StatusTip) {
        const QString tip = static_cast<QStatusTipEvent *>(e)->tip();
        if (!tip.isEmpty())
            showNotice(tip);
        return true;
    }
    return QWidget::event(e);
}

void MainWindow::showNotice(const QString &text)
{
    m_notice = text;
    updateStatus();
    QTimer::singleShot(8000, this, [this, text] {
        if (m_notice == text) {
            m_notice.clear();
            updateStatus();
        }
    });
}

void MainWindow::reportDownloadProblem(const QString &what, const QString &reason)
{
    const QString line = what.isEmpty() ? reason : tr("%1: %2").arg(what, reason);
    // the same request reported twice (core signal + the page's own check) shows once
    if (m_problemBox && m_problemLines.contains(line))
        return;
    showNotice(tr("Download not started - %1").arg(line));
    m_problemLines << line;
    if (!m_problemBox) {
        m_problemBox = new QMessageBox(QMessageBox::Information, tr("OctoMX"), QString(), QMessageBox::Ok, this);
        m_problemBox->setObjectName(QStringLiteral("downloadProblem"));
        m_problemBox->setAttribute(Qt::WA_DeleteOnClose);
        m_problemBox->setWindowModality(Qt::NonModal);
        connect(m_problemBox, &QObject::destroyed, this, [this] { m_problemLines.clear(); });
    }
    m_problemBox->setText(tr("The download could not be started:\n\n%1").arg(m_problemLines.join(QLatin1Char('\n'))));
    m_problemBox->show();
}

void MainWindow::updateTitle()
{
    const QString t = titleText();
    if (windowTitle() != t)
        setWindowTitle(t);
}

void MainWindow::updateNews()
{
    // Our own status line in the news bar (UI_SPEC §8.1).
    const NetworkService *net = m_core->network();
    int online = 0;
    for (const NetworkStatus &s : net->networks())
        if (s.state == ConnState::Connected)
            ++online;
    QString text;
    switch (net->overallState()) {
    case ConnState::Connected:
        text = online == 1 ? tr("Ready - 1 network online") : tr("Ready - %1 networks online").arg(online);
        break;
    case ConnState::Connecting:
        text = tr("Connecting...");
        break;
    default:
        text = tr("Offline");
        break;
    }
    if (m_core->isMock())
        text += tr("  (demo mode)");
    m_news->setText(text);
}

void MainWindow::updateStatus()
{
    Page *p = m_pages.value(m_current);
    if (!p)
        return;
    QStringList f = isFloating(m_current) ? QStringList() : p->statusFields();
    if (!m_notice.isEmpty())
        f << m_notice;
    m_status->setFields(f);
}

void MainWindow::resizeEvent(QResizeEvent *)
{
    relayout();
}

void MainWindow::relayout()
{
    const int w = width(), h = height();
    // [ui-e] rows A and B follow the window: the seven page buttons share the width (94 px at the
    // classic 682-px band, up to 150 px each), the news bar fills row A up to the three buttons, which
    // end where row B ends. Below the classic band the rows clip as in the original WinMX.
    const int n = qMax(1, int(m_pageBtnList.size()));
    const int pitch = qBound(S(98), (w - S(2) - S(2)) / n, S(154));
    const int gap = S(4);
    for (int i = 0; i < m_pageBtnList.size(); ++i)
        m_pageBtnList[i]->setGeometry(S(2) + pitch * i, S(kRowBY), pitch - gap, S(kRowH));
    const int bandEnd = S(2) + pitch * n - gap;   // right edge of the last page button
    const int y = S(kRowAY), rh = S(kRowH);
    const int help = S(62), btn = S(61);
    const int helpX = bandEnd - help;
    const int settingsX = helpX - S(4) - btn;
    const int ignoredX = settingsX - S(4) - btn;
    m_news->setGeometry(S(3), y, ignoredX - S(4) - S(3), rh);
    m_ignoredBtn->setGeometry(ignoredX, y, btn, rh);
    m_settingsBtn->setGeometry(settingsX, y, btn, rh);
    m_helpBtn->setGeometry(helpX, y, help, rh);
    const int statusH = m_status->height();
    m_split->setGeometry(0, S(kPageAreaY), w, qMax(0, h - S(kPageAreaY) - statusH));
    m_status->setGeometry(0, h - statusH, w, statusH);
}

void MainWindow::saveState()
{
    Settings *s = m_core->settings();
    if (!s)
        return;
    s->setValue(keys::ui::Geometry, saveGeometry());
    s->setValue(keys::ui::Maximized, isMaximized());
    s->setValue(keys::ui::LastPage, m_current);
    if (isHotlistVisible())
        m_hotlistWidth = m_hotlist->width();
    s->setValue(keys::ui::splitterKey(QStringLiteral("hotlistWidth")), QByteArray::number(m_hotlistWidth));
    s->setValue(QStringLiteral("ui/hotlistVisible"), isHotlistVisible());
    for (Page *p : std::as_const(m_pages))
        p->saveState();
    for (FloatWindow *fw : std::as_const(m_floats))
        fw->savePlacement();
    s->sync();
}

void MainWindow::restoreState()
{
    m_restoring = true;
    Settings *s = m_core->settings();
    QString page = QStringLiteral("networks");
    if (s) {
        const QByteArray geo = s->value(keys::ui::Geometry).toByteArray();
        if (!geo.isEmpty())
            restoreGeometry(geo);
        const QString last = s->s(keys::ui::LastPage);
        if (s->b(keys::ui::RestoreLastPage) && m_pages.contains(last))
            page = last;
        const QByteArray hw = s->value(keys::ui::splitterKey(QStringLiteral("hotlistWidth"))).toByteArray();
        if (!hw.isEmpty())
            m_hotlistWidth = qBound(S(120), hw.toInt(), S(600));
    }
    showPage(page);
    if (s && s->value(QStringLiteral("ui/hotlistVisible")).toBool())
        setHotlistVisible(true);
    m_restoring = false;
}

// [ui-c] closeEvent / handleCloseRequest / setupTray / openSettings: see main_window_c.cpp

// [ui-b] openIgnored / openBrowse / openWhois / openPrivateMessage / openChat: see main_window_b.cpp

void MainWindow::openHelp()
{
    AboutDialog dlg(m_core, this);
    connect(&dlg, &AboutDialog::checkForUpdatesRequested, this, &MainWindow::checkForUpdates);
    dlg.exec();
}

QMenu *MainWindow::helpMenu()
{
    if (!m_helpMenu) {
        m_helpMenu = new QMenu(this);
        m_helpMenu->setObjectName(QStringLiteral("helpMenu"));
        QAction *about = m_helpMenu->addAction(tr("About OctoMX..."), this, [this] { QTimer::singleShot(0, this, &MainWindow::openHelp); });
        m_helpMenu->addAction(tr("Check for Updates..."), this, &MainWindow::checkForUpdates);
        QAction *terms = m_helpMenu->addAction(tr("Terms of Use..."), this, [this] {
            LegalNotice dlg(true, this);
            dlg.exec();
        });
        terms->setToolTip(tr("Show the terms you accepted at the first start: use the program legally, you are responsible for what you exchange."));
        terms->setStatusTip(terms->toolTip());
        m_helpMenu->setDefaultAction(about);
    }
    return m_helpMenu;
}

QMenu *MainWindow::settingsButtonMenu()
{
    if (!m_settingsMenu) {
        m_settingsMenu = new QMenu(this);
        m_settingsMenu->setObjectName(QStringLiteral("settingsButtonMenu"));
        QAction *open = m_settingsMenu->addAction(tr("Settings..."), this, [this] { openSettings(); });
        m_settingsMenu->setDefaultAction(open);
        m_settingsMenu->addSeparator();
        m_settingsMenu->addMenu(createColorThemeMenu(this, m_settingsMenu));
    }
    return m_settingsMenu;
}

void MainWindow::checkForUpdates()
{
    if (UpdateController *u = UpdateController::of(this))
        u->checkNow(true);
}

void MainWindow::startSearch(const QString &text, const QString &text2)
{
    showPage(QStringLiteral("search"));
    if (auto *s = qobject_cast<SearchPage *>(page(QStringLiteral("search"))))
        s->searchInNewTab(text, text2);
}

void MainWindow::notifyDownloadDone(const TransferInfo &t)
{
    showNotice(tr("Download complete: %1").arg(t.name));
    // [ui-h] Settings > Interface and Logs: "Play a sound when a download completes", "Flash the
    // taskbar button on events"; Appearance > Interface "Notify me ...", "Show notifications in the tray"
    sound::alert(m_core->settings(), sound::Event::DownloadDone);
    flashTaskbar();
    if (!m_core->settings()->b(keys::ui::NotifyDownloadDone) || !m_core->settings()->b(keys::misc::TrayBalloons))
        return;
    if (s_notifyHook) {
        s_notifyHook(t.name);
        return;
    }
    if (TrayController *tray = TrayController::of(this))
        if (QSystemTrayIcon *icon = tray->trayIcon())
            if (icon->isVisible() && QSystemTrayIcon::supportsMessages())
                icon->showMessage(tr("Download complete"), t.name, appIcon(), 6000);
}

std::function<void(const QString &)> MainWindow::s_notifyHook;
std::function<void()> MainWindow::s_flashHook;

void MainWindow::applyAlwaysOnTop()
{
    const bool on = m_core->settings()->b(keys::misc::AlwaysOnTop);
    if (bool(windowFlags() & Qt::WindowStaysOnTopHint) == on)
        return;
    const bool wasVisible = isVisible();
    setWindowFlag(Qt::WindowStaysOnTopHint, on);   // hides the window: show it again
    if (wasVisible)
        show();
}

// [ui-h] misc/flashTaskbar: the taskbar button flashes for an event while OctoMX is not the active window
void MainWindow::flashTaskbar()
{
    if (!m_core->settings()->b(keys::misc::FlashTaskbar) || isActiveWindow())
        return;
    if (s_flashHook) {
        s_flashHook();
        return;
    }
    QApplication::alert(this);
}

void MainWindow::notifyHotlistOnline(const QString &name)
{
    if (!m_core->settings()->b(keys::hotlist::NotifyOnline))
        return;
    showNotice(tr("%1 (Hotlist) is online").arg(name));
    sound::alert(m_core->settings(), sound::Event::HotlistOnline);   // [ui-h]
    flashTaskbar();
    if (!m_core->settings()->b(keys::misc::TrayBalloons))
        return;
    if (s_notifyHook) {
        s_notifyHook(tr("%1 is online").arg(name));
        return;
    }
    if (TrayController *tray = TrayController::of(this))
        if (QSystemTrayIcon *icon = tray->trayIcon())
            if (icon->isVisible() && QSystemTrayIcon::supportsMessages())
                icon->showMessage(tr("Hotlist"), tr("%1 is online").arg(name), appIcon(), 6000);
}

void MainWindow::addLinks(const QStringList &uris)
{
    QStringList ids, accepted, skipped;
    for (const QString &raw : uris) {
        QString u = raw.trimmed();
        if (u.isEmpty())
            continue;
        const QUrl url(u);
        if (url.isLocalFile())
            u = url.toLocalFile();
        // [ui-e] a dropped / pasted file must be a .torrent (other files are not downloads)
        if (!isLink(u) && !(QFileInfo(u).isFile() && u.endsWith(QLatin1String(".torrent"), Qt::CaseInsensitive))) {
            skipped << QFileInfo(u).fileName().left(60);
            continue;
        }
        const QString id = m_core->transfers()->downloadUri(u);
        if (!id.isEmpty()) {
            ids << id;
            accepted << u;
        }
        // an empty id: the core said why (downloadNotStarted -> reportDownloadProblem)
    }
    if (!ids.isEmpty()) {
        showPage(QStringLiteral("transfers"));
        emit linksAdded(accepted, ids);
    }
    // [ui-e] never silent
    if (!skipped.isEmpty())
        showNotice(tr("Not a link or .torrent file OctoMX can download: %1").arg(skipped.join(QStringLiteral(", "))));
    else if (ids.size() > 1)
        showNotice(tr("%1 downloads started.").arg(ids.size()));
}

void MainWindow::dragEnterEvent(QDragEnterEvent *e)
{
    const QMimeData *m = e->mimeData();
    if (m->hasUrls() || (m->hasText() && isLink(m->text().trimmed())))
        e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *e)
{
    const QMimeData *m = e->mimeData();
    QStringList links;
    if (m->hasUrls()) {
        for (const QUrl &u : m->urls())
            links << (u.isLocalFile() ? u.toLocalFile() : u.toString());
    } else if (m->hasText()) {
        links = m->text().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    }
    addLinks(links);
    e->acceptProposedAction();
}

void MainWindow::keyPressEvent(QKeyEvent *e)
{
    if (e->matches(QKeySequence::NextChild) || (e->key() == Qt::Key_Tab && (e->modifiers() & Qt::ControlModifier))) {
        const QStringList ids = pageIds();
        showPage(ids.at((ids.indexOf(m_current) + 1) % ids.size()));
        return;
    }
    QWidget::keyPressEvent(e);
}

// ---------------------------------------------------------------------------------- FloatWindow

FloatWindow::FloatWindow(Page *page, QWidget *parent) : QWidget(parent, Qt::Window), m_page(page), m_pageId(page->pageId())
{
    setWindowTitle(page->title());
    setWindowIcon(appIcon());
    setAutoFillBackground(true);
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, S(3), 0, 0);
    lay->setSpacing(0);
    page->setParent(this);
    lay->addWidget(page, 1);
    m_status = new StatusBar(this);
    m_status->floatTab()->setText(tr("Dock"));
    lay->addWidget(m_status);
    connect(m_status->floatTab(), &FloatTab::clicked, this, &FloatWindow::dockRequested);
    connect(page, &Page::statusChanged, this, &FloatWindow::updateStatus);
    updateStatus();
}

Page *FloatWindow::takePage()
{
    Page *p = m_page;
    if (p) {
        disconnect(p, nullptr, this, nullptr);
        layout()->removeWidget(p);
        p->setParent(nullptr);
    }
    m_page = nullptr;
    return p;
}

void FloatWindow::updateStatus()
{
    if (m_page)
        m_status->setFields(m_page->statusFields());
}

void FloatWindow::closeEvent(QCloseEvent *e)
{
    // closing a floating page docks it back (the original WinMX never loses a page)
    e->ignore();
    emit dockRequested();
}

} // namespace mx::ui

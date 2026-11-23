// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: MainWindow parts owned by ui-c: Settings dialog, Setup Wizard from Settings, tray,
// minimise / close behaviour, system menu, plus the script verbs for them. Kept out of
// main_window.cpp so ui-b and ui-c can work on the window at the same time.
#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QTimer>
#include <QtGui/QActionGroup>
#include <QtGui/QCloseEvent>
#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <QtGui/QWindowStateChangeEvent>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QSystemTrayIcon>
#include <QtWidgets/QTreeView>

#ifdef Q_OS_WIN
#include <QtCore/QAbstractNativeEventFilter>
#include <windows.h>
#endif

#include "mx/services.h"
#include "mx/settings_keys.h"
#include "ui/dialogs/close_prompt.h"
#include "ui/dialogs/settings_binding.h"
#include "ui/dialogs/settings_dialog.h"
#include "ui/dialogs/setup_wizard.h"
#include "ui/kit/colors.h"
#include "ui/kit/icons.h"
#include "ui/kit/scale.h"
#include "ui/main_window.h"
#include "ui/os_integration.h"
#include "ui/script_driver.h"
#include "ui/speed_limits.h"
#include "ui/tray.h"
#include "ui/update_ui.h"

namespace mx::ui {

using namespace mx::keys;

namespace {

int g_trayOverride = -1;
TrayController::PromptHook g_promptHook;
TrayController::ConfirmHook g_confirmHook;

bool isActive(TransferState s)
{
    switch (s) {
    case TransferState::Queued:
    case TransferState::FindingSources:
    case TransferState::Connecting:
    case TransferState::RemotelyQueued:
    case TransferState::Transferring:
    case TransferState::Verifying:
        return true;
    default:
        return false;
    }
}

SettingsDialog *settingsDialogOf(MainWindow *mw)
{
    return mw->findChild<SettingsDialog *>(QString(), Qt::FindDirectChildrenOnly);
}

// Settings ▸ "Setup Wizard...": "Clear All Current Settings Before Running Setup Wizard?"
void runWizardFromSettings(MainWindow *mw, QWidget *parent)
{
    Settings *s = mw->core()->settings();
    const int r = SettingsBinder::ask(parent, QObject::tr("OctoMX"), QObject::tr("Clear All Current Settings Before Running Setup Wizard?"),
                                      QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, QMessageBox::No);
    if (r == QMessageBox::Cancel)
        return;
    QHash<QString, QVariant> before;
    for (const SettingDef &d : settingsSchema())
        if (d.restartRequired)
            before.insert(d.key, s->value(d.key));
    if (r == QMessageBox::Yes) {
        // everything except the window layout and the bookkeeping
        for (const SettingDef &d : settingsSchema()) {
            if (d.key.startsWith(QLatin1String("ui/")) || d.key.startsWith(QLatin1String("internal/")) || d.key.endsWith(QLatin1String("/*")))
                continue;
            if (d.key == QLatin1String(identity::ScreenName) || d.key == QLatin1String(ed2k::UserHash))
                continue;
            s->setValue(d.key, QVariant());
        }
    }
    auto *wiz = new SetupWizard(mw->core(), parent);
    wiz->setAttribute(Qt::WA_DeleteOnClose);
    QObject::connect(wiz, &QDialog::accepted, mw, [mw, wiz, before, parent] {
        wiz->applyFolders();
        Settings *st = mw->core()->settings();
        QStringList names;
        for (auto it = before.constBegin(); it != before.constEnd(); ++it)
            if (st->value(it.key()) != it.value()) {
                const SettingDef *d = findSetting(it.key());
                names << QStringLiteral("    ") + (d ? d->label : it.key());
            }
        if (!names.isEmpty())
            SettingsBinder::message(parent, QObject::tr("OctoMX"),
                                    QObject::tr("THESE SETTINGS TAKE EFFECT WHEN OCTOMX STARTS AGAIN:\n\n%1").arg(names.join(QLatin1Char('\n'))));
    });
    wiz->open();
}

#ifdef Q_OS_WIN
// The extended system menu (UI_SPEC §6.1): items inserted into the native window menu.
enum : UINT { kSysHide = 0x0100, kSysExit = 0x0110, kSysExitDone = 0x0120, kSysShutDone = 0x0130, kSysAuto = 0x0140, kSysReset = 0x0150 };

class SystemMenuFilter : public QAbstractNativeEventFilter {
public:
    SystemMenuFilter(TrayController *t, HWND hwnd) : m_t(t), m_hwnd(hwnd) {}
    bool nativeEventFilter(const QByteArray &, void *message, qintptr *result) override
    {
        MSG *m = static_cast<MSG *>(message);
        if (m->hwnd != m_hwnd)
            return false;
        if (m->message == WM_INITMENUPOPUP && HIWORD(m->lParam)) {   // the window menu
            HMENU sm = GetSystemMenu(m_hwnd, FALSE);
            CheckMenuItem(sm, kSysExitDone, MF_BYCOMMAND | (m_t->exitWhenDone() ? MF_CHECKED : MF_UNCHECKED));
            CheckMenuItem(sm, kSysShutDone, MF_BYCOMMAND | (m_t->shutdownWhenDone() ? MF_CHECKED : MF_UNCHECKED));
            const bool autoOn = m_t->mainWindow()->core()->settings()->b(misc::StartWithOs);
            CheckMenuItem(sm, kSysAuto, MF_BYCOMMAND | (autoOn ? MF_CHECKED : MF_UNCHECKED));
            return false;
        }
        if (m->message != WM_SYSCOMMAND)
            return false;
        const UINT id = UINT(m->wParam & 0xFFF0);
        TrayController *t = m_t;
        switch (id) {
        case kSysHide: QTimer::singleShot(0, t, [t] { t->hideToTray(); }); break;
        case kSysExit: QTimer::singleShot(0, t, [t] { t->requestExit(); }); break;
        case kSysExitDone: t->setExitWhenDone(!t->exitWhenDone()); break;
        case kSysShutDone: t->setShutdownWhenDone(!t->shutdownWhenDone()); break;
        case kSysAuto: {
            Settings *s = t->mainWindow()->core()->settings();
            s->setValue(misc::StartWithOs, !s->b(misc::StartWithOs));
            break;
        }
        case kSysReset: QTimer::singleShot(0, t, [t] { t->resetAllWindows(); }); break;
        default: return false;
        }
        *result = 0;
        return true;
    }

private:
    TrayController *m_t;
    HWND m_hwnd;
};
#endif

} // namespace

// ================================================================================ MainWindow

void MainWindow::openSettings(const QString &settingsPage)
{
    SettingsDialog *dlg = settingsDialogOf(this);
    if (dlg && !dlg->isVisible()) {
        delete dlg;   // closed but not yet deleted (deleteLater pending)
        dlg = nullptr;
    }
    const bool fresh = !dlg;
    if (!dlg) {
        dlg = new SettingsDialog(m_core, this, this);
        connect(dlg, &SettingsDialog::pageShown, this, [this](const QString &id) {
            m_core->settings()->setValue(keys::ui::SettingsPage, id);
        });
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        const QByteArray geo = m_core->settings()->value(keys::ui::dialogKey(QStringLiteral("settings"))).toByteArray();
        if (!geo.isEmpty())
            dlg->restoreGeometry(geo);
        else
            dlg->move(geometry().center() - SP(SettingsDialog::kClientW / 2, SettingsDialog::kClientH / 2));
        connect(dlg, &SettingsDialog::wizardRequested, this, [this, dlg] { runWizardFromSettings(this, dlg); });
        connect(dlg, &QDialog::finished, this, [this, dlg] {
            m_core->settings()->setValue(keys::ui::dialogKey(QStringLiteral("settings")), dlg->saveGeometry());
            dlg->deleteLater();   // the next open builds the pages from the current settings
        });
    }
    if (!settingsPage.isEmpty() && !dlg->showPage(settingsPage))
        qWarning("openSettings: unknown settings page '%s'", qPrintable(settingsPage));
    // [ui-h] ui/settingsPage: a plain Settings click opens the page that was open last time
    if (settingsPage.isEmpty() && fresh) {
        const QString last = m_core->settings()->s(keys::ui::SettingsPage);
        if (!last.isEmpty())
            dlg->showPage(last);
    }
    dlg->show();
    dlg->raise();
    dlg->activateWindow();
}

void MainWindow::setupTray()
{
    new TrayController(this);
}

bool MainWindow::handleCloseRequest()
{
    TrayController *t = TrayController::of(this);
    return t ? t->handleClose() : true;
}

QMenu *createColorThemeMenu(MainWindow *mw, QWidget *parent)
{
    auto *m = new QMenu(QObject::tr("Color Theme"), parent);
    m->setObjectName(QStringLiteral("colorThemeMenu"));
    auto *group = new QActionGroup(m);
    group->setExclusive(true);
    const struct { const char *text; const char *id; } items[] = {
        {QT_TR_NOOP("Dark (classic)"), "dark"}, {QT_TR_NOOP("Light"), "light"}, {QT_TR_NOOP("Follow system"), "system"}};
    for (const auto &it : items) {
        QAction *a = m->addAction(QObject::tr(it.text));
        a->setCheckable(true);
        a->setData(QString::fromLatin1(it.id));
        group->addAction(a);
        QObject::connect(a, &QAction::triggered, mw, [mw, id = QString::fromLatin1(it.id)] {
            mw->core()->settings()->setValue(keys::ui::ColorTheme, id);
        });
    }
    auto sync = [m, mw] {
        const QString cur = mw->core()->settings()->s(keys::ui::ColorTheme);
        for (QAction *a : m->actions())
            a->setChecked(a->data().toString() == cur);
    };
    sync();
    QObject::connect(m, &QMenu::aboutToShow, m, sync);
    return m;
}

void saveFloatingPages(MainWindow *mw)
{
    QStringList ids;
    for (const QString &id : MainWindow::pageIds())
        if (mw->isFloating(id))
            ids << id;
    mw->core()->settings()->setValue(keys::ui::FloatingPages, ids);
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    if (handleCloseRequest()) {
        saveState();
        saveFloatingPages(this);
        for (FloatWindow *fw : std::as_const(m_floats))
            fw->hide();
        e->accept();
        emit quitRequested();
    } else {
        e->ignore();
    }
}

// ============================================================================ TrayController

TrayController::TrayController(MainWindow *mw) : QObject(mw), m_mw(mw)
{
    setObjectName(QStringLiteral("trayController"));
    buildMenus();
    m_icon = new QSystemTrayIcon(appIcon(), this);
    m_icon->setObjectName(QStringLiteral("trayIcon"));
    m_icon->setContextMenu(m_menu);
    m_icon->setToolTip(mw->windowTitle());
    connect(mw, &QWidget::windowTitleChanged, m_icon, [this](const QString &t) { m_icon->setToolTip(t); });
    connect(m_icon, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
        if (r == QSystemTrayIcon::Trigger) {
            restore();
        } else if (r == QSystemTrayIcon::DoubleClick) {
            // the first click of a double-click already restored the window
            if (!m_hidden && QDateTime::currentMSecsSinceEpoch() - m_restoredAt > 600)
                hideToTray();
            else
                restore();
        }
    });
    if (trayAvailable())
        m_icon->show();
    m_doneTimer = new QTimer(this);
    m_doneTimer->setInterval(1000);
    connect(m_doneTimer, &QTimer::timeout, this, &TrayController::checkDone);
    connect(mw->core()->settings(), &Settings::changed, this, [this](const QString &key) {
        if (key == QLatin1String(misc::StartWithOs))
            updateMenus();
    });
    mw->installEventFilter(this);
    installSystemMenu();
    updateMenus();
    // pages that were floating at the last exit float again
    const QStringList floating = mw->core()->settings()->sl(keys::ui::FloatingPages);
    if (!floating.isEmpty())
        QTimer::singleShot(0, this, [this, floating] {
            for (const QString &id : floating)
                if (MainWindow::pageIds().contains(id) && !m_mw->isFloating(id))
                    m_mw->floatPage(id);
        });
}

TrayController::~TrayController()
{
#ifdef Q_OS_WIN
    if (m_nativeFilter) {
        auto *f = static_cast<SystemMenuFilter *>(m_nativeFilter);
        qApp->removeNativeEventFilter(f);
        delete f;
    }
#endif
    if (m_icon)
        m_icon->hide();
}

TrayController *TrayController::of(const MainWindow *mw)
{
    return mw ? mw->findChild<TrayController *>(QStringLiteral("trayController"), Qt::FindDirectChildrenOnly) : nullptr;
}

bool TrayController::trayAvailable()
{
    if (g_trayOverride >= 0)
        return g_trayOverride == 1;
    if (qEnvironmentVariableIsSet("MX_FAKE_TRAY"))
        return true;
    return QSystemTrayIcon::isSystemTrayAvailable();
}

void TrayController::setTrayAvailableOverride(int v)
{
    g_trayOverride = v;
}

void TrayController::setPromptHook(const PromptHook &h)
{
    g_promptHook = h;
}

void TrayController::setConfirmHook(const ConfirmHook &h)
{
    g_confirmHook = h;
}

void TrayController::buildMenus()
{
    // tray menu [obs: web/menu_system_and_tray.png right]
    m_menu = new QMenu(m_mw);
    m_menu->setObjectName(QStringLiteral("trayMenu"));
    m_show = m_menu->addAction(tr("Show"), this, &TrayController::restore);
    m_hide = m_menu->addAction(tr("Hide"), this, &TrayController::hideToTray);
    m_menu->addSeparator();
    m_exitDone = m_menu->addAction(tr("Exit When Xfers Stop"), this, [this](bool on) { setExitWhenDone(on); });
    m_exitDone->setCheckable(true);
    m_shutDone = m_menu->addAction(tr("Shutdown When Xfers Stop"), this, [this](bool on) { setShutdownWhenDone(on); });
    m_shutDone->setCheckable(true);
    m_menu->addSeparator();
    m_menu->addAction(tr("Reset All Windows"), this, &TrayController::resetAllWindows);
    m_menu->addMenu(createColorThemeMenu(m_mw, m_menu));   // [post-1]
    m_menu->addMenu(createSpeedLimitsMenu(m_mw, m_menu));  // [bw-1]
    m_menu->addSeparator();
    m_auto = m_menu->addAction(tr("Auto - Startup"), this, [this](bool on) { m_mw->core()->settings()->setValue(misc::StartWithOs, on); });
    m_auto->setCheckable(true);
    m_menu->addSeparator();
    m_menu->addAction(tr("Exit"), this, [this] { QTimer::singleShot(0, this, &TrayController::requestExit); });
    connect(m_menu, &QMenu::aboutToShow, this, &TrayController::updateMenus);

    // system menu items [obs: same file, left]
    m_sysMenu = new QMenu(m_mw);
    m_sysMenu->setObjectName(QStringLiteral("systemMenu"));
    m_sysMenu->addAction(tr("Hide"), this, &TrayController::hideToTray);
    m_sysMenu->addAction(tr("Exit"), this, [this] { QTimer::singleShot(0, this, &TrayController::requestExit); });
    m_sysMenu->addSeparator();
    m_sysExitDone = m_sysMenu->addAction(tr("Exit When Transfers Finish"), this, [this](bool on) { setExitWhenDone(on); });
    m_sysExitDone->setCheckable(true);
    m_sysShutDone = m_sysMenu->addAction(tr("Shutdown When Transfers Finish"), this, [this](bool on) { setShutdownWhenDone(on); });
    m_sysShutDone->setCheckable(true);
    m_sysMenu->addSeparator();
    m_sysAuto = m_sysMenu->addAction(tr("Auto - Startup"), this, [this](bool on) { m_mw->core()->settings()->setValue(misc::StartWithOs, on); });
    m_sysAuto->setCheckable(true);
    m_sysMenu->addSeparator();
    m_sysMenu->addAction(tr("Reset All Windows"), this, &TrayController::resetAllWindows);
    connect(m_sysMenu, &QMenu::aboutToShow, this, &TrayController::updateMenus);
}

void TrayController::updateMenus()
{
    const bool autoOn = m_mw->core()->settings()->b(misc::StartWithOs);
    for (QAction *a : {m_auto, m_sysAuto}) {
        QSignalBlocker b(a);
        a->setChecked(autoOn);
    }
    for (QAction *a : {m_exitDone, m_sysExitDone}) {
        QSignalBlocker b(a);
        a->setChecked(m_exitWhenDone);
    }
    for (QAction *a : {m_shutDone, m_sysShutDone}) {
        QSignalBlocker b(a);
        a->setChecked(m_shutdownWhenDone);
    }
    // the default (bold) item is what a double-click does
    m_menu->setDefaultAction(m_mw->isVisible() && !m_hidden ? m_hide : m_show);
}

void TrayController::installSystemMenu()
{
#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(m_mw->winId());
    HMENU sm = GetSystemMenu(hwnd, FALSE);
    if (!sm)
        return;
    UINT pos = 0;
    auto item = [&](UINT id, const QString &text) {
        InsertMenuW(sm, pos++, MF_BYPOSITION | MF_STRING, id, reinterpret_cast<LPCWSTR>(text.utf16()));
    };
    auto sep = [&] { InsertMenuW(sm, pos++, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr); };
    item(kSysHide, tr("Hide"));
    item(kSysExit, tr("Exit"));
    sep();
    item(kSysExitDone, tr("Exit When Transfers Finish"));
    item(kSysShutDone, tr("Shutdown When Transfers Finish"));
    sep();
    item(kSysAuto, tr("Auto - Startup"));
    sep();
    item(kSysReset, tr("Reset All Windows"));
    sep();
    auto *f = new SystemMenuFilter(this, hwnd);
    qApp->installNativeEventFilter(f);
    m_nativeFilter = f;
#endif
}

bool TrayController::eventFilter(QObject *o, QEvent *e)
{
    // [post-1] the minimise button is an ordinary minimise to the taskbar, always (owner request):
    // misc/minimizeToTray is still read from old profiles but no longer hides the window
    if (o == m_mw && e->type() == QEvent::WindowStateChange && qEnvironmentVariableIsSet("MX_TRAY_DEBUG")) {
        auto *se = static_cast<QWindowStateChangeEvent *>(e);
        qInfo("tray: state %d -> %d hidden=%d", int(se->oldState()), int(m_mw->windowState()), int(m_hidden));
    }
    return QObject::eventFilter(o, e);
}

void TrayController::hideToTray()
{
    if (!trayAvailable()) {
        // no tray: the nearest thing is the task bar
        m_mw->showMinimized();
        return;
    }
    if (m_hidden)
        return;
    if (!m_mw->isMinimized())
        m_wasMaximized = m_mw->isMaximized();
    m_hiddenWindows.clear();
    // floating pages and our dialogs go with the main window
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (w == m_mw || !w->isVisible() || qobject_cast<QMenu *>(w))
            continue;
        if (qobject_cast<FloatWindow *>(w) || w->parentWidget() == m_mw) {
            m_hiddenWindows << w;
            w->hide();
        }
    }
    m_mw->saveState();
    saveFloatingPages(m_mw);
    m_mw->hide();
    m_hidden = true;
    m_icon->show();
    updateMenus();
    emit hiddenChanged(true);
}

void TrayController::restore()
{
    const bool wasHidden = m_hidden || !m_mw->isVisible() || m_mw->isMinimized();
    m_hidden = false;
    if (m_wasMaximized)
        m_mw->showMaximized();
    else
        m_mw->showNormal();
    for (const QPointer<QWidget> &w : std::as_const(m_hiddenWindows))
        if (w)
            w->show();
    m_hiddenWindows.clear();
    m_mw->raise();
    m_mw->activateWindow();
    if (wasHidden)
        m_restoredAt = QDateTime::currentMSecsSinceEpoch();
    if (!trayAvailable())
        m_icon->hide();
    updateMenus();
    if (wasHidden)
        emit hiddenChanged(false);
}

void TrayController::toggle()
{
    if (m_hidden || !m_mw->isVisible())
        restore();
    else
        hideToTray();
}

void TrayController::startHidden()
{
    if (trayAvailable()) {
        m_wasMaximized = m_mw->isMaximized() || m_mw->core()->settings()->b(keys::ui::Maximized);
        m_hidden = true;
        m_icon->show();
        updateMenus();
    } else {
        m_mw->showMinimized();
    }
}

int TrayController::activeDownloads() const
{
    int n = 0;
    for (const TransferInfo &t : m_mw->core()->transfers()->downloads())
        if (isActive(t.state))
            ++n;
    return n;
}

bool TrayController::confirmExit()
{
    const int n = activeDownloads();
    if (n == 0 || !m_mw->core()->settings()->b(misc::ConfirmExit))
        return true;
    // [qa-2] "%n ... download(s)" printed "(s)" literally (no translator is loaded)
    const QString text = (n == 1 ? tr("There is 1 active download.") : tr("There are %1 active downloads.").arg(n)) +
                         tr("\n\nAre you sure you want to exit OctoMX?");
    if (g_confirmHook)
        return g_confirmHook(text);
    return QMessageBox::question(m_mw->isVisible() ? m_mw : nullptr, tr("OctoMX"), text, QMessageBox::Yes | QMessageBox::No,
                                 QMessageBox::No) == QMessageBox::Yes;
}

bool TrayController::handleClose()
{
    if (m_quitting || qApp->isSavingSession())
        return true;
    Settings *s = m_mw->core()->settings();
    const QString action = s->s(misc::CloseAction);
    // [post-1] without a system tray the X exits: never an invisible program nobody can reach
    if (action == QLatin1String("exit") || !trayAvailable()) {
        if (!confirmExit())
            return false;
        m_quitting = true;
        return true;
    }
    if (action == QLatin1String("tray")) {
        hideToTray();
        return false;
    }
    // "Prompt" (UI_SPEC §5.5)
    const bool downloads = activeDownloads() > 0;
    bool dont = false;
    int choice = -1;
    if (g_promptHook) {
        choice = g_promptHook(downloads, &dont);
    } else {
        ClosePrompt dlg(downloads, m_mw);
        if (dlg.exec() == QDialog::Accepted) {
            choice = dlg.choice();
            dont = dlg.dontShowAgain();
        }
    }
    if (choice < 0)
        return false;
    if (dont)
        s->setValue(misc::CloseAction, choice == ClosePrompt::KeepRunningInTray ? QStringLiteral("tray") : QStringLiteral("exit"));
    switch (choice) {
    case ClosePrompt::ExitNow:
        m_quitting = true;
        return true;
    case ClosePrompt::ExitWhenDone:
        setExitWhenDone(true);
        hideToTray();
        return false;
    default:
        hideToTray();
        return false;
    }
}

void TrayController::requestExit()
{
    if (!confirmExit())
        return;
    quitNow();
}

void TrayController::quitNow()
{
    if (m_quitting && !m_mw->isVisible() && !m_hidden)
        return;
    m_quitting = true;
    m_doneTimer->stop();
    m_mw->saveState();
    saveFloatingPages(m_mw);
    m_icon->hide();
    emit m_mw->quitRequested();
}

void TrayController::setExitWhenDone(bool on)
{
    m_exitWhenDone = on;
    if (on || m_shutdownWhenDone)
        m_doneTimer->start();
    else
        m_doneTimer->stop();
    updateMenus();
}

void TrayController::setShutdownWhenDone(bool on)
{
    m_shutdownWhenDone = on;
    if (on || m_exitWhenDone)
        m_doneTimer->start();
    else
        m_doneTimer->stop();
    updateMenus();
}

void TrayController::checkDone()
{
    if ((!m_exitWhenDone && !m_shutdownWhenDone) || activeDownloads() > 0)
        return;
    const bool shutdown = m_shutdownWhenDone;
    m_exitWhenDone = m_shutdownWhenDone = false;
    qInfo("octomx: transfers finished - %s", shutdown ? "exit and shut down" : "exit");
    if (shutdown && m_os)
        m_os->shutdownComputer();
    quitNow();
}

void TrayController::resetAllWindows()
{
    for (const QString &id : MainWindow::pageIds())
        if (m_mw->isFloating(id))
            m_mw->dockPage(id);
    Settings *s = m_mw->core()->settings();
    for (const QString &k : s->keys(QString(keys::ui::DialogsPrefix)))
        s->setValue(k, QVariant());
    m_wasMaximized = false;
    restore();
    m_mw->showNormal();
    m_mw->resize(SS(800, 600));
    if (QScreen *sc = m_mw->screen()) {
        const QRect a = sc->availableGeometry();
        m_mw->move(a.center() - SP(400, 300));
    }
    if (SettingsDialog *d = settingsDialogOf(m_mw))
        d->move(m_mw->geometry().center() - SP(SettingsDialog::kClientW / 2, SettingsDialog::kClientH / 2));
    m_mw->saveState();
}

// ============================================================================== script verbs

namespace {

bool setWidgetValue(QWidget *w, const QString &value, QString *error)
{
    if (auto *bw = dynamic_cast<BoundWidget *>(w)) {
        if (qobject_cast<ColorButton *>(w))
            bw->setValueByUser(QColor(value).name());
        else if (w->metaObject()->className() == QByteArray("mx::ui::StringListEditor"))
            bw->setValueByUser(SettingsBinder::textToList(value));
        else
            bw->setValueByUser(value);
        return true;
    }
    if (auto *cb = qobject_cast<QComboBox *>(w)) {
        int i = cb->findData(value);
        if (i < 0)
            i = cb->findText(value);
        bool num = false;
        if (i < 0)
            i = value.toInt(&num);
        if (i < 0 || i >= cb->count()) {
            *error = QStringLiteral("no item '%1'").arg(value);
            return false;
        }
        cb->setCurrentIndex(i);
        emit cb->activated(i);
        return true;
    }
    if (auto *sb = qobject_cast<QSpinBox *>(w)) {
        sb->setValue(value.toInt());
        return true;
    }
    if (auto *ds = qobject_cast<QDoubleSpinBox *>(w)) {
        ds->setValue(value.toDouble());
        return true;
    }
    if (auto *le = qobject_cast<QLineEdit *>(w)) {
        le->setText(value);
        emit le->editingFinished();
        return true;
    }
    if (auto *mb = qobject_cast<MenuButton *>(w)) {
        for (QAction *a : mb->menu()->findChildren<QAction *>())
            if (a->data().toString() == value || a->text() == value) {
                a->trigger();
                return true;
            }
        *error = QStringLiteral("no choice '%1'").arg(value);
        return false;
    }
    if (auto *ab = qobject_cast<QAbstractButton *>(w); ab && ab->isCheckable()) {
        const bool on = value == QLatin1String("on") || value == QLatin1String("true") || value == QLatin1String("1");
        if (ab->isChecked() != on)
            ab->click();
        return true;
    }
    *error = QStringLiteral("cannot set a value on %1").arg(QString::fromLatin1(w->metaObject()->className()));
    return false;
}

QString settingText(const QVariant &v)
{
    if (v.typeId() == QMetaType::QStringList)
        return SettingsBinder::listToText(v.toStringList());
    if (v.typeId() == QMetaType::Bool)
        return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    return v.toString();
}

bool uicVerbs(MainWindow *mw, const QString &verb, const QString &rest, const QStringList &args, QString *error)
{
    TrayController *tray = TrayController::of(mw);
    if (verb == QLatin1String("settings")) {
        mw->openSettings(rest);
        SettingsDialog *d = settingsDialogOf(mw);
        if (!rest.isEmpty() && (!d || d->resolvePage(rest).isEmpty()))
            *error = QStringLiteral("unknown settings page '%1'").arg(rest);
        return true;
    }
    if (verb == QLatin1String("setvalue")) {
        QWidget *w = ScriptDriver::findWidget(mw, args.value(0));
        if (!w) {
            *error = QStringLiteral("no widget named %1").arg(args.value(0));
            return true;
        }
        setWidgetValue(w, rest.section(QLatin1Char(' '), 1), error);
        return true;
    }
    if (verb == QLatin1String("expect-setting")) {
        const QString key = args.value(0);
        const QString want = rest.section(QLatin1Char(' '), 1);
        const QString have = settingText(mw->core()->settings()->value(key));
        if (have != want)
            *error = QStringLiteral("%1 is '%2', expected '%3'").arg(key, have, want);
        return true;
    }
    if (verb == QLatin1String("wizard")) {
        auto *wiz = new SetupWizard(mw->core(), mw);
        wiz->setAttribute(Qt::WA_DeleteOnClose);
        if (!rest.isEmpty())
            for (int i = 0; i < rest.toInt(); ++i)
                wiz->next();
        wiz->open();
        return true;
    }
    // ---- assertions for the restart / persistence tests
    if (verb == QLatin1String("expect-page")) {
        if (mw->currentPageId() != rest)
            *error = QStringLiteral("current page is %1").arg(mw->currentPageId());
        return true;
    }
    if (verb == QLatin1String("expect-hotlist")) {
        if (mw->isHotlistVisible() != (rest == QLatin1String("on")))
            *error = QStringLiteral("hotlist visible=%1").arg(mw->isHotlistVisible());
        return true;
    }
    if (verb == QLatin1String("expect-size")) {
        const QSize want(args.value(0).toInt(), args.value(1).toInt());
        const QSize have = mw->isMaximized() ? mw->normalGeometry().size() : mw->size();
        if (have != want)
            *error = QStringLiteral("size is %1x%2").arg(have.width()).arg(have.height());
        return true;
    }
    if (verb == QLatin1String("expect-maximized")) {
        if (mw->isMaximized() != (rest == QLatin1String("on") || rest == QLatin1String("true")))
            *error = QStringLiteral("maximized=%1").arg(mw->isMaximized());
        return true;
    }
    if (verb == QLatin1String("expect-floating")) {
        if (!mw->isFloating(rest))
            *error = QStringLiteral("%1 is not floating").arg(rest);
        return true;
    }
    if (verb == QLatin1String("set-column") || verb == QLatin1String("expect-column")) {
        auto *l = qobject_cast<QTreeView *>(ScriptDriver::findWidget(mw, args.value(0)));
        if (!l) {
            *error = QStringLiteral("no list view named %1").arg(args.value(0));
            return true;
        }
        const int col = args.value(1).toInt(), width = args.value(2).toInt();
        if (verb == QLatin1String("set-column"))
            l->header()->resizeSection(col, width);
        else if (l->header()->sectionSize(col) != width)
            *error = QStringLiteral("column %1 is %2 px").arg(col).arg(l->header()->sectionSize(col));
        return true;
    }
    if (verb == QLatin1String("expect-widget")) {
        QWidget *w = ScriptDriver::findWidget(mw, args.value(0));
        const QString want = rest.section(QLatin1Char(' '), 1);
        QString have;
        if (!w) {
            *error = QStringLiteral("no widget named %1").arg(args.value(0));
            return true;
        }
        if (auto *bw = dynamic_cast<BoundWidget *>(w)) {
            const QVariant v = bw->boundValue();
            have = v.typeId() == QMetaType::QStringList ? SettingsBinder::listToText(v.toStringList()) : v.toString();
        } else if (auto *le = qobject_cast<QLineEdit *>(w)) {
            have = le->text();
        } else if (auto *sb = qobject_cast<QSpinBox *>(w)) {
            have = QString::number(sb->value());
        } else if (auto *cb = qobject_cast<QComboBox *>(w)) {
            have = cb->currentData().isValid() ? cb->currentData().toString() : cb->currentText();
        } else if (auto *mb = qobject_cast<MenuButton *>(w)) {
            have = mb->currentData().toString();
        } else if (auto *ab = qobject_cast<QAbstractButton *>(w)) {
            have = ab->isChecked() ? QStringLiteral("on") : QStringLiteral("off");
        }
        if (have != want)
            *error = QStringLiteral("%1 shows '%2', expected '%3'").arg(args.value(0), have, want);
        return true;
    }
    if (verb == QLatin1String("expect-settings-page")) {
        SettingsDialog *d = settingsDialogOf(mw);
        if (!d || d->currentPageId() != rest)
            *error = QStringLiteral("settings page is '%1'").arg(d ? d->currentPageId() : QStringLiteral("(closed)"));
        return true;
    }
    // [post-1] Help button menu, Settings button menu, Check for Updates..., the colour theme
    if (verb == QLatin1String("help-menu") || verb == QLatin1String("settings-menu")) {
        const bool help = verb == QLatin1String("help-menu");
        QWidget *btn = ScriptDriver::findWidget(mw, help ? QStringLiteral("btnHelp") : QStringLiteral("btnSettings"));
        QMenu *m = help ? mw->helpMenu() : mw->settingsButtonMenu();
        const int x = btn ? (help ? btn->width() - m->sizeHint().width() : 0) : 0;
        m->popup(btn ? btn->mapToGlobal(QPoint(x, btn->height())) : mw->mapToGlobal(QPoint(S(600), S(44))));
        return true;
    }
    if (verb == QLatin1String("check-updates")) {
        mw->checkForUpdates();
        return true;
    }
    if (verb == QLatin1String("update-show")) {   // screenshots: a release JSON file, no network
        QFile f(rest);
        ReleaseInfo r;
        QString why;
        if (!f.open(QIODevice::ReadOnly) || !parseLatestRelease(f.readAll(), &r, &why)) {
            *error = QStringLiteral("cannot read release %1 %2").arg(rest, why);
            return true;
        }
        if (UpdateController *u = UpdateController::of(mw))
            u->showResult(UpdateChecker::evaluate(r, u->currentVersion()), true);
        return true;
    }
    if (verb == QLatin1String("expect-theme")) {
        const QString have = Colors::themeId(Colors::instance()->theme());
        if (have != rest)
            *error = QStringLiteral("colour theme is %1").arg(have);
        return true;
    }
    if (!tray)
        return false;
    if (verb == QLatin1String("tray-menu") || verb == QLatin1String("system-menu")) {
        QMenu *m = verb == QLatin1String("tray-menu") ? tray->trayMenu() : tray->systemMenu();
        const QPoint at = verb == QLatin1String("tray-menu") ? mw->mapToGlobal(QPoint(mw->width() - S(190), mw->height() - S(200)))
                                                              : mw->mapToGlobal(QPoint(2, 2));
        m->popup(at);
        return true;
    }
    // [ui-d] "close-window <name>" of another window is ui-b's verb: only the main window here
    if (verb == QLatin1String("close-window") && (rest.isEmpty() || rest == mw->objectName())) {
        QTimer::singleShot(0, mw, [mw] { mw->close(); });
        return true;
    }
    if (verb == QLatin1String("hide-to-tray")) {
        tray->hideToTray();
        return true;
    }
    if (verb == QLatin1String("restore")) {
        tray->restore();
        return true;
    }
    if (verb == QLatin1String("minimize")) {
        mw->showMinimized();
        return true;
    }
    if (verb == QLatin1String("maximize")) {
        mw->showMaximized();
        return true;
    }
    // [post-1] minimise = taskbar, X = what misc/closeAction says
    if (verb == QLatin1String("expect-minimized")) {
        if (mw->isMinimized() != (rest == QLatin1String("on") || rest == QLatin1String("true")))
            *error = QStringLiteral("minimized=%1").arg(mw->isMinimized());
        return true;
    }
    if (verb == QLatin1String("expect-hidden-to-tray")) {
        if (tray->isHiddenToTray() != (rest == QLatin1String("on") || rest == QLatin1String("true")))
            *error = QStringLiteral("hidden to tray=%1").arg(tray->isHiddenToTray());
        return true;
    }
    if (verb == QLatin1String("expect-visible")) {
        const bool want = rest == QLatin1String("true") || rest == QLatin1String("on");
        if (mw->isVisible() != want)
            *error = QStringLiteral("main window visible=%1").arg(mw->isVisible());
        return true;
    }
    return false;
}

[[maybe_unused]] const bool g_registered = [] {
    ScriptDriver::addCommandHandler(&uicVerbs);
    return true;
}();

} // namespace

} // namespace mx::ui

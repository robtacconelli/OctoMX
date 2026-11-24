// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI [mac-1]: menu bar, Dock and open events on macOS (see mac_glue.h).
#include "ui/mac_glue.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QFileInfo>
#include <QtCore/QTimer>

#include <memory>
#include <QtCore/QUrl>
#include <QtGui/QFileOpenEvent>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeySequence>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMenuBar>

#include "mx/services.h"
#include "mx/settings_keys.h"
#include "ui/main_window.h"
#include "ui/os_integration.h"
#include "ui/tray.h"

namespace mx::ui {

OpenEventFilter::OpenEventFilter(QObject *parent) : QObject(parent)
{
    if (QCoreApplication *app = QCoreApplication::instance())
        app->installEventFilter(this);
}

QString OpenEventFilter::linkOf(const QString &file, const QString &url)
{
    if (!file.isEmpty())
        return file.endsWith(QLatin1String(".torrent"), Qt::CaseInsensitive) ? QFileInfo(file).absoluteFilePath() : QString();
    const QUrl u(url);
    const QString s = u.scheme().toLower();
    if (s == QLatin1String("magnet") || s == QLatin1String("ed2k"))
        return url;
    if (u.isLocalFile() && u.toLocalFile().endsWith(QLatin1String(".torrent"), Qt::CaseInsensitive))
        return u.toLocalFile();
    return QString();
}

bool OpenEventFilter::eventFilter(QObject *o, QEvent *e)
{
    if (e->type() == QEvent::FileOpen && o == QCoreApplication::instance()) {
        auto *fe = static_cast<QFileOpenEvent *>(e);
        const QString link = linkOf(fe->file(), fe->url().toString(QUrl::FullyEncoded));
        if (!link.isEmpty()) {
            qInfo("octomx: the system opened %s with OctoMX", link.startsWith(QLatin1Char('/')) ? "a .torrent file" : "a link");
            emit linksReceived({link});
            return true;
        }
    }
    return QObject::eventFilter(o, e);
}

#ifdef Q_OS_MACOS
namespace {

// Quit from the Dock, the application menu or the system: the close prompt decides (as the close
// button does on Windows); an accepted quit goes through MainWindow::close() -> quitRequested
class QuitFilter : public QObject {
public:
    explicit QuitFilter(MainWindow *mw) : QObject(mw), m_mw(mw) { qApp->installEventFilter(this); }

protected:
    bool eventFilter(QObject *o, QEvent *e) override
    {
        if (e->type() == QEvent::Quit && o == qApp) {
            TrayController *t = TrayController::of(m_mw);
            if (t && !t->isQuitting()) {
                e->ignore();
                QTimer::singleShot(0, m_mw, [mw = m_mw] { requestQuit(mw); });
                return true;
            }
        }
        return QObject::eventFilter(o, e);
    }

public:
    static void requestQuit(MainWindow *mw)
    {
        TrayController *t = TrayController::of(mw);
        if (!t) {
            mw->close();
            return;
        }
        // the prompt needs a visible window to sit on
        if (t->isHiddenToTray() && mw->core()->settings()->s(mx::keys::misc::CloseAction) == QLatin1String("ask"))
            t->restore();
        if (t->handleClose())
            t->quitNow();
    }

private:
    MainWindow *m_mw;
};

} // namespace
#endif

QMenuBar *installMacGlue(MainWindow *mw)
{
#ifdef Q_OS_MACOS
    if (!mw)
        return nullptr;
    auto *bar = new QMenuBar(nullptr);   // parentless: the one global menu bar of the application
    bar->setObjectName(QStringLiteral("macMenuBar"));
    QObject::connect(mw, &QObject::destroyed, bar, &QObject::deleteLater);

    // the application menu: Qt moves these actions there by their role
    QMenu *app = bar->addMenu(QObject::tr("OctoMX"));
    QAction *about = app->addAction(QObject::tr("About OctoMX"), mw, &MainWindow::openHelp);
    about->setMenuRole(QAction::AboutRole);
    QAction *upd = app->addAction(QObject::tr("Check for Updates..."), mw, &MainWindow::checkForUpdates);
    upd->setMenuRole(QAction::ApplicationSpecificRole);
    QAction *prefs = app->addAction(QObject::tr("Settings..."), mw, [mw] { mw->openSettings(); });
    prefs->setMenuRole(QAction::PreferencesRole);
    prefs->setShortcut(QKeySequence::Preferences);
    QAction *quit = app->addAction(QObject::tr("Quit OctoMX"), mw, [mw] { QuitFilter::requestQuit(mw); });
    quit->setMenuRole(QAction::QuitRole);
    quit->setShortcut(QKeySequence::Quit);

    QMenu *win = bar->addMenu(QObject::tr("Window"));
    QAction *mini = win->addAction(QObject::tr("Minimize"), mw, [mw] { mw->showMinimized(); });
    mini->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_M));   // Cmd+M
    QAction *close = win->addAction(QObject::tr("Close Window"), mw, [mw] {
        if (TrayController *t = TrayController::of(mw))
            t->hideToTray();
    });
    close->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_W));   // Cmd+W hides, as the red button
    win->addSeparator();
    win->addAction(QObject::tr("Show OctoMX"), mw, [mw] {
        if (TrayController *t = TrayController::of(mw))
            t->restore();
    });

    // the Dock icon's menu
    auto *dock = new QMenu(mw);
    dock->setObjectName(QStringLiteral("dockMenu"));
    dock->addAction(QObject::tr("Show OctoMX"), mw, [mw] {
        if (TrayController *t = TrayController::of(mw))
            t->restore();
    });
    dock->addAction(QObject::tr("Hide"), mw, [mw] {
        if (TrayController *t = TrayController::of(mw))
            t->hideToTray();
    });
    dock->setAsDockMenu();

    // a click on the Dock icon (the system makes the application active again, also when it is
    // active already) brings a hidden window back; not in the first moments after hiding (a start
    // hidden at login activates the application once)
    auto hiddenFor = std::make_shared<QElapsedTimer>();
    hiddenFor->start();
    if (TrayController *t = TrayController::of(mw))
        QObject::connect(t, &TrayController::hiddenChanged, mw, [hiddenFor](bool hidden) {
            if (hidden)
                hiddenFor->restart();
        });
    QObject::connect(qApp, &QGuiApplication::applicationStateChanged, mw, [mw, hiddenFor](Qt::ApplicationState st) {
        if (st != Qt::ApplicationActive)
            return;
        TrayController *t = TrayController::of(mw);
        if (t && t->isHiddenToTray() && !t->isQuitting() && hiddenFor->elapsed() > 1500)
            t->restore();
    });
    new QuitFilter(mw);
    return bar;
#else
    Q_UNUSED(mw);
    return nullptr;
#endif
}

} // namespace mx::ui

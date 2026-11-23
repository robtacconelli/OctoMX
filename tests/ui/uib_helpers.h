// SPDX-License-Identifier: GPL-3.0-or-later
// Helpers shared by the ui-b tests (tst_uib_*): a frozen mock core + main window in a temporary
// profile, restartable with the same profile to check persistence.
#pragma once

#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMenu>

#include "mock/mock_core.h"
#include "mx/services.h"
#include "ui/kit/colors.h"
#include "ui/kit/theme.h"
#include "ui/main_window.h"

namespace uib {

struct App {
    mx::Core *core = nullptr;
    mx::ui::MainWindow *mw = nullptr;

    void start(const QString &profile, bool frozen = true)
    {
        mx::MockOptions o;
        o.frozen = frozen;
        core = mx::createMockCoreWithOptions(profile, o);
        mx::ui::Colors::instance()->attach(core->settings());
        core->start();
        mw = new mx::ui::MainWindow(core);
        mw->resize(1024, 689);
        mw->show();
        QVERIFY(QTest::qWaitForWindowExposed(mw));
        QTest::qWait(30);
    }
    void stop()
    {
        if (mw) {
            mw->saveState();
            delete mw;
            mw = nullptr;
        }
        if (core) {
            core->settings()->sync();
            delete core;
            core = nullptr;
        }
        mx::ui::Colors::instance()->attach(nullptr);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
};

// the currently open popup menu (menus are created with WA_DeleteOnClose)
inline QMenu *openMenu()
{
    for (QWidget *w : QApplication::topLevelWidgets())
        if (auto *m = qobject_cast<QMenu *>(w); m && m->isVisible())
            return m;
    return nullptr;
}

inline QAction *menuAction(QMenu *m, const QString &text)
{
    if (!m)
        return nullptr;
    for (QAction *a : m->actions())
        if (a->text().remove(QLatin1Char('&')) == text)
            return a;
    return nullptr;
}

inline void closeMenus()
{
    for (QWidget *w : QApplication::topLevelWidgets())
        if (auto *m = qobject_cast<QMenu *>(w); m && m->isVisible())
            m->close();
    QTest::qWait(10);
}

inline int visibleTopLevels(const QWidget *except = nullptr)
{
    int n = 0;
    for (QWidget *w : QApplication::topLevelWidgets())
        if (w != except && w->isVisible() && !qobject_cast<QMenu *>(w))
            ++n;
    return n;
}

inline mx::UserRef user(const QString &name, mx::Net n = mx::Net::Overlay)
{
    mx::UserRef u;
    u.net = n;
    u.id = name;
    u.name = name;
    return u;
}

} // namespace uib

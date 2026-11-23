// SPDX-License-Identifier: GPL-3.0-or-later
// ui-b: the Chat page on the frozen demo core, channel list + filter, joining rooms (tabs,
// backlog, user list), sending / slash commands, the user menu, the warning, persistence of the
// open rooms and the splitter across a restart, a 1500-user room.
#include <QtCore/QElapsedTimer>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QSplitter>

#include "uib_helpers.h"
#include "mock/mock_data.h"
#include "ui/dialogs/user_windows.h"
#include "ui/kit/list_view.h"
#include "ui/kit/widgets.h"
#include "ui/models/chat_models.h"
#include "ui/pages/chat_page.h"
#include "ui/pages/chat_view.h"

using namespace mx;
using namespace mx::ui;

class TstUibChat : public QObject {
    Q_OBJECT
private:
    QTemporaryDir m_dir;
    uib::App m_app;
    ChatPage *page() const { return qobject_cast<ChatPage *>(m_app.mw->page(QStringLiteral("chat"))); }
    QString profile() const { return m_dir.filePath(QStringLiteral("profile")); }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_app.start(profile());
    }
    void cleanupTestCase() { m_app.stop(); }

    void warningOnFirstVisitAndCancelGoesBack()
    {
        m_app.mw->showPage(QStringLiteral("transfers"));
        m_app.mw->showPage(QStringLiteral("chat"));
        QDialog *dlg = nullptr;
        QTRY_VERIFY((dlg = m_app.mw->findChild<QDialog *>(QStringLiteral("chatWarningDialog"))) != nullptr);
        QTRY_VERIFY(dlg->isVisible());
        dlg->reject();
        QTRY_COMPARE(m_app.mw->currentPageId(), QStringLiteral("transfers"));
        // only once per session
        m_app.mw->showPage(QStringLiteral("chat"));
        QTest::qWait(50);
        QVERIFY(!m_app.mw->findChild<QDialog *>(QStringLiteral("chatWarningDialog")));
        QCOMPARE(m_app.mw->currentPageId(), QStringLiteral("chat"));
    }

    void channelListAndFilter()
    {
        ChannelListView *l = page()->channelList();
        QVERIFY(l);
        // [ui-d] the list shows the rooms of one network (the overlay by default)
        int overlayRooms = 0;
        for (const ChatRoomInfo &r : m_app.core->chat()->roomList())
            overlayRooms += r.net == Net::Overlay ? 1 : 0;
        QTRY_COMPARE(l->model()->roomCount(), overlayRooms);
        QCOMPARE(page()->statusFields().value(0), QStringLiteral("%1 channels").arg(l->model()->roomCount()));
        QCOMPARE(l->list()->model()->headerData(1, Qt::Horizontal).toString(), QStringLiteral("Users"));
        // Filter is a latched toggle: typing alone does not filter
        l->edit()->setText(QStringLiteral("jazz"));
        QCOMPARE(l->proxy()->rowCount(), l->model()->roomCount());
        auto *filter = l->findChild<QPushButton *>(QStringLiteral("chatFilter"));
        filter->setChecked(true);
        int expect = 0;
        for (const ChatRoomInfo &r : m_app.core->chat()->roomList())
            if (r.name.contains(QStringLiteral("jazz"), Qt::CaseInsensitive))
                ++expect;
        QVERIFY(expect > 0);
        QCOMPARE(l->proxy()->rowCount(), expect);
        filter->setChecked(false);
        l->edit()->clear();
        QCOMPARE(l->proxy()->rowCount(), l->model()->roomCount());
    }

    void joinRoomOpensTab()
    {
        ChatPage *p = page();
        const QString help = mock::helpRoomName();
        QSignalSpy joined(m_app.core->chat(), &ChatService::roomJoined);
        p->join(QStringLiteral("OctoMX Help"));
        QCOMPARE(joined.count(), 1);
        ChatRoomView *v = p->currentRoom();
        QVERIFY(v);
        QCOMPARE(v->roomId(), help);
        QCOMPARE(p->tabs()->count(), 2);
        QCOMPARE(v->userModel()->userCount(), m_app.core->chat()->roomUsers(help).size());
        QVERIFY(v->output()->lineCount() >= m_app.core->chat()->backlog(help).size() - 1);
        QVERIFY(v->output()->plainText().contains(QStringLiteral("Twirl836_24616 (DSL 178 files) has entered")));
        QCOMPARE(v->headerName(), QStringLiteral("%1 on OctoMX Peer Network").arg(help));
        // ops first in the user list
        QVERIFY(v->userList()->model()->index(0, 0).data().toString().startsWith(QLatin1Char('@')));
        QVERIFY(m_app.core->settings()->value(chatkeys::OpenRooms).toStringList().contains(help));
    }

    void sendAndCommands()
    {
        ChatRoomView *v = page()->currentRoom();
        QVERIFY(v);
        const int before = v->output()->lineCount();
        v->submit(QStringLiteral("hello room"));
        QCOMPARE(v->output()->lineCount(), before + 1);
        QVERIFY(v->output()->plainText().endsWith(QStringLiteral("<%1> hello room").arg(m_app.core->network()->screenName())));
        v->submit(QStringLiteral("/me waves"));
        QVERIFY(v->output()->plainText().endsWith(QStringLiteral("* %1 waves").arg(m_app.core->network()->screenName())));
        v->submit(QStringLiteral("/channelname"));
        QVERIFY(v->output()->plainText().endsWith(v->roomId()));
        // typed into the input line + Enter
        v->input()->setFocus();
        QTest::keyClicks(v->input(), QStringLiteral("/topic Testing topics"));
        QTest::keyClick(v->input(), Qt::Key_Return);
        QVERIFY(v->input()->text().isEmpty());
        QTRY_COMPARE(v->room().topic, QStringLiteral("Testing topics"));
        // input line history (Up)
        QTest::keyClick(v->input(), Qt::Key_Up);
        QCOMPARE(v->input()->text(), QStringLiteral("/topic Testing topics"));
        v->input()->clear();
        v->submit(QStringLiteral("/clr"));
        QCOMPARE(v->output()->lineCount(), 0);
    }

    void userMenuActions()
    {
        ChatRoomView *v = page()->currentRoom();
        const QModelIndex row = v->userList()->model()->index(3, 0);
        const QString key = row.data(IdRole).toString();
        v->showUserMenu(row, v->userList()->viewport()->mapToGlobal(QPoint(20, 20)));
        QMenu *m = uib::openMenu();
        QVERIFY(m);
        QCOMPARE(m->defaultAction()->text(), QStringLiteral("Send Message"));
        for (const char *t : {"Browse", "Whois", "Add To Hotlist", "Ignore", "Copy Name"})
            QVERIFY2(uib::menuAction(m, QString::fromLatin1(t)), t);
        uib::menuAction(m, QStringLiteral("Whois"))->trigger();
        uib::closeMenus();
        auto *w = m_app.mw->findChild<WhoisWindow *>();
        QVERIFY(w);
        QCOMPARE(w->user().key(), key);
        QTRY_VERIFY(w->hasResult());
        w->close();
        QTRY_VERIFY(!m_app.mw->findChild<WhoisWindow *>());
        // Ignore -> UserService
        v->showUserMenu(row, QPoint(10, 10));
        uib::menuAction(uib::openMenu(), QStringLiteral("Ignore"))->trigger();
        uib::closeMenus();
        bool ignored = false;
        for (const UserRef &u : m_app.core->users()->ignored())
            if (u.key() == key)
                ignored = true;
        QVERIFY(ignored);
        // double-click = private message
        emit v->userList()->rowActivated(row);
        QTRY_VERIFY(m_app.mw->findChild<PrivateMessageWindow *>());
        m_app.mw->findChild<PrivateMessageWindow *>()->close();
    }

    void commandsMenu()
    {
        ChatRoomView *v = page()->currentRoom();
        v->showCommandsMenu();
        QMenu *m = uib::openMenu();
        QVERIFY(m);
        QVERIFY(uib::menuAction(m, QStringLiteral("/action <text>")));
        QVERIFY(!uib::menuAction(m, QStringLiteral("Join Channel"))->isEnabled());
        QVERIFY(uib::menuAction(m, QStringLiteral("Part Channel"))->isEnabled());
        QVERIFY(uib::menuAction(m, QStringLiteral("Admin Channel Config Commands"))->menu());
        uib::menuAction(m, QStringLiteral("/action <text>"))->trigger();
        uib::closeMenus();
        QCOMPARE(v->input()->text(), QStringLiteral("/action "));
        v->input()->clear();
    }

    void secondRoomAndPart()
    {
        ChatPage *p = page();
        p->join(QStringLiteral("Linux Users"));
        QCOMPARE(p->rooms().size(), 2);
        QCOMPARE(p->tabs()->count(), 3);
        ChatRoomView *linux = p->currentRoom();
        QVERIFY(linux->room().name.startsWith(QStringLiteral("Linux Users_")));
        QCOMPARE(m_app.core->settings()->value(chatkeys::OpenRooms).toStringList().size(), 2);
        // Exit Channel parts and closes the tab
        linux->findChild<QPushButton *>(QStringLiteral("chatExit"))->click();
        QTRY_COMPARE(p->rooms().size(), 1);
        QCOMPARE(p->tabs()->count(), 2);
        QCOMPARE(m_app.core->settings()->value(chatkeys::OpenRooms).toStringList(), QStringList{mock::helpRoomName()});
    }

    void holdOpenAfterPart()
    {
        ChatPage *p = page();
        m_app.core->settings()->setValue(chatkeys::HoldOpenAfterPart, true);
        p->join(QStringLiteral("Blues Bar"));
        ChatRoomView *v = p->currentRoom();
        m_app.core->chat()->leaveRoom(v->roomId());
        QCOMPARE(p->rooms().size(), 2);
        QVERIFY(!v->isJoined());
        QVERIFY(!v->input()->isEnabled());
        m_app.core->settings()->setValue(chatkeys::HoldOpenAfterPart, false);
        emit v->closeRequested();
        QTRY_COMPARE(p->rooms().size(), 1);
    }

    void bigRoomResponsive()
    {
        ChatPage *p = page();
        QElapsedTimer t;
        t.start();
        p->join(QStringLiteral("Late Night Chat"));
        ChatRoomView *v = p->currentRoom();
        QCOMPARE(v->userModel()->userCount(), 1501);
        QElapsedTimer r;
        r.start();
        v->reloadUsers();
        v->userList()->viewport()->repaint();
        qInfo("join 1500-user room %lld ms, reload+paint %lld ms", t.elapsed(), r.elapsed());
        QVERIFY(r.elapsed() < 500);
        emit v->closeRequested();
        QTRY_COMPARE(p->rooms().size(), 1);
    }

    void persistsAcrossRestart()
    {
        ChatPage *p = page();
        ChatRoomView *v = p->rooms().value(0);
        QVERIFY(v);
        v->splitter()->setSizes({600, 300});
        emit v->splitter()->splitterMoved(600, 1);
        const int userListWidth = v->splitter()->sizes().value(1);
        m_app.stop();
        m_app.start(profile());
        p = page();
        m_app.mw->showPage(QStringLiteral("chat"));
        // the open room comes back (the demo network is up at once)
        QTRY_COMPARE(p->rooms().size(), 1);
        QCOMPARE(p->rooms().value(0)->roomId(), mock::helpRoomName());
        QVERIFY(p->selectSubTab(mock::helpRoomName()));
        QTest::qWait(50);
        QCOMPARE(p->rooms().value(0)->splitter()->sizes().value(1), userListWidth);
    }
};

QTEST_MAIN(TstUibChat)
#include "tst_uib_chat.moc"

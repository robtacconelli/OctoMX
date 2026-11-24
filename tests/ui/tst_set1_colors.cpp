// SPDX-License-Identifier: GPL-3.0-or-later
// [set-1] Every colour of Settings > Appearance, table-driven: the colour is set to a probe value that
// occurs nowhere else, the windows it belongs to are drawn (demo data, frozen) and the probe must be
// found in the pixels of the window the Appearance page names, and in no window of another kind; in
// the Dark and in the Light theme. MX_SET1_COLOR_MAP=1 prints, for every colour, every window that
// shows it (how the table below was made, then checked by hand against the Appearance pages).
#include <QtCore/QTemporaryDir>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtTest/QtTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QTreeView>

#include "mx/diagnostics.h"
#include "mx/services.h"
#include "mx/settings_keys.h"
#include "ui/dialogs/browse_window.h"
#include "ui/dialogs/connection_test.h"
#include "ui/dialogs/server_messages.h"
#include "ui/dialogs/user_windows.h"
#include "ui/hotlist_pane.h"
#include "ui/kit/colors.h"
#include "ui/kit/list_view.h"
#include "ui/kit/widgets.h"
#include "ui/main_window.h"
#include "ui/models/chat_models.h"
#include "ui/models/search_model.h"
#include "ui/models/transfer_model.h"
#include "ui/pages/bandwidth_page.h"
#include "ui/pages/chat_page.h"
#include "ui/pages/chat_view.h"
#include "ui/pages/networks_page.h"
#include "ui/pages/search_page.h"
#include "ui/pages/transfers_page.h"
#include "uib_helpers.h"

using namespace mx;
using namespace mx::ui;

namespace {

const QColor kProbe(0x13, 0x57, 0x9b);

int countOf(const QImage &img, const QColor &c)
{
    const QRgb want = c.rgb();
    int n = 0;
    for (int y = 0; y < img.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x)
            n += (line[x] | 0xff000000) == want;
    }
    return n;
}

// the windows, by name; the Appearance page that lists each colour names one of them
enum Scene { ScNewsBar, ScNetworks, ScOtherNets, ScSearch, ScTransfers, ScUploads, ScLibrary, ScHotlist, ScChatRoom, ScChatUsers, ScChatInput,
             ScChannels, ScPrivate, ScServerMsgs, ScIgnored, ScBrowse, ScBandwidth, ScConnTest,
             // rows in the states the demo does not show, drawn by the same models and lists
             ScCraftTransfers, ScCraftSearch, ScCraftUsers, ScCraftLed, ScSceneCount };
const char *const kSceneNames[] = {"news", "networks", "othernets", "search", "transfers", "uploads", "library", "hotlist", "chatroom",
                                   "chatusers", "chatinput", "channels", "private", "servermsgs", "ignored", "browse", "bandwidth",
                                   "conntest", "craft-transfers", "craft-search", "craft-users", "craft-led"};
// windows of another kind than a list: a list colour must not appear there
const QList<Scene> kNotALista = {ScBandwidth};

} // namespace

class TstSet1Colors : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    uib::App m_app;
    PrivateMessageWindow *m_pm = nullptr;
    ServerMessagesWindow *m_server = nullptr;
    BrowseWindow *m_browse = nullptr;
    QWidget *m_ignored = nullptr;

    // the crafted scenes
    TransferModel *m_craftTransfers = nullptr;
    MxListView *m_craftTransferList = nullptr;
    SearchModel *m_craftSearch = nullptr;
    MxListView *m_craftSearchList = nullptr;
    ChatUserModel *m_craftUsers = nullptr;
    MxListView *m_craftUserList = nullptr;
    QWidget *m_leds = nullptr;

    Settings *st() const { return m_app.core->settings(); }

    void buildCraftedScenes()
    {
        m_craftTransfers = new TransferModel(nullptr, false, st(), this);
        auto row = [](const QString &id, TransferState s, const QString &text) {
            TransferInfo t;
            t.id = id;
            t.name = id + QStringLiteral(".iso");
            t.state = s;
            t.statusText = text;
            t.size = 1000;
            return t;
        };
        m_craftTransfers->addTransfer(row(QStringLiteral("busy"), TransferState::Connecting, QStringLiteral("Busy (3 in queue)")));
        m_craftTransfers->addTransfer(row(QStringLiteral("nego"), TransferState::Connecting, QStringLiteral("Connected, negotiating...")));
        m_craftTransfers->addTransfer(row(QStringLiteral("gone"), TransferState::Cancelled, QStringLiteral("Cancelled")));
        TransferInfo bt = row(QStringLiteral("ubuntu-24.04-desktop-amd64"), TransferState::Transferring, QString());
        TransferSource src;
        src.net = Net::BitTorrent;
        src.connection = QStringLiteral("BitTorrent");
        src.statusText = QStringLiteral("Transferring");
        src.rateBps = 1000;
        bt.sources << src;
        m_craftTransfers->addTransfer(bt);
        m_craftTransferList = new MxListView;
        m_craftTransferList->setTreeMode(true);
        m_craftTransferList->setModel(m_craftTransfers);
        m_craftTransferList->resize(900, 160);
        m_craftTransferList->show();
        m_craftTransferList->expandAll();

        m_craftSearch = new SearchModel(this);
        m_craftSearch->setFlagSuspects(true);
        SearchHit fake;
        fake.key = QStringLiteral("k-fake");
        fake.fileName = QStringLiteral("Big Buck Bunny 1080p BluRay.avi.exe");
        fake.size = 1843200;
        fake.suspect = QStringLiteral("executable named like a media file");
        SearchSource fs;
        fs.net = Net::ED2K;
        fs.freeSlots = 0;
        fs.totalSlots = 3;
        fs.connection = QStringLiteral("DSL");
        fake.sources << fs;
        SearchHit full = fake;
        full.key = QStringLiteral("k-full");
        full.fileName = QStringLiteral("debian-12-netinst.iso");
        full.suspect.clear();
        SearchSource fs2 = fs;
        fs2.connection = QStringLiteral("Cable");
        full.sources << fs2;   // two sources: a group with its source rows ("0 of 3 available")
        m_craftSearch->applyHits({fake, full});
        m_craftSearchList = new MxListView;
        m_craftSearchList->setTreeMode(true);
        m_craftSearchList->setModel(m_craftSearch);
        m_craftSearchList->resize(900, 120);
        m_craftSearchList->show();
        m_craftSearchList->expandAll();

        m_craftUsers = new ChatUserModel(this);
        m_craftUsers->setUsers({ChatUser{uib::user(QStringLiteral("Voiced_70707")), 10, QStringLiteral("DSL"), 1}});
        m_craftUserList = new MxListView;
        m_craftUserList->setModel(m_craftUsers);
        m_craftUserList->resize(300, 80);
        m_craftUserList->show();

        m_leds = new QWidget;
        auto *red = new StatusIndicator(m_leds), *yellow = new StatusIndicator(m_leds);
        red->setState(0);
        yellow->setState(1);
        yellow->move(red->width() + 4, 0);
        m_leds->resize(red->width() * 2 + 8, red->height());
        m_leds->show();
    }
    template <class T> T *page(const char *id) const { return qobject_cast<T *>(m_app.mw->page(QString::fromLatin1(id))); }

    void say(const QString &room, ChatKind kind, const QString &from, const QString &text)
    {
        ChatMessage m;
        m.roomId = room;
        m.kind = kind;
        m.from = uib::user(from);
        m.text = text;
        m.time = QDateTime::currentDateTime();
        emit m_app.core->chat()->messageReceived(m);
    }

    // Every row of a list, also the ones below the visible part (a small screen makes the windows
    // short): the viewport page by page, stacked into one picture.
    static QImage stacked(const QList<QImage> &parts)
    {
        if (parts.size() == 1) return parts.first();
        int w = 0, h = 0;
        for (const QImage &p : parts) {
            w = qMax(w, p.width());
            h += p.height();
        }
        QImage all(w, h, QImage::Format_ARGB32);
        all.fill(parts.first().pixelColor(0, 0));
        QPainter pt(&all);
        int y = 0;
        for (const QImage &p : parts) {
            pt.drawImage(0, y, p);
            y += p.height();
        }
        return all;
    }
    static QImage grabAllRows(QAbstractItemView *lv)
    {
        QScrollBar *sb = lv->verticalScrollBar();
        const int keep = sb->value();
        QList<QImage> parts;
        for (int v = sb->minimum();; v += qMax(1, sb->pageStep())) {
            sb->setValue(qMin(v, sb->maximum()));
            QCoreApplication::processEvents();
            parts << lv->viewport()->grab().toImage();
            if (v >= sb->maximum()) break;
        }
        sb->setValue(keep);
        QCoreApplication::processEvents();
        return stacked(parts);
    }

    QImage render(Scene s)
    {
        QWidget *w = nullptr;
        switch (s) {
        case ScNewsBar:
            m_app.mw->newsBar()->setText(QStringLiteral("Big Buck Bunny is free to share"), QStringLiteral("www.bigbuckbunny.org"));
            w = m_app.mw->newsBar();
            break;
        case ScNetworks:
            m_app.mw->showPage(QStringLiteral("networks"));
            page<NetworksPage>("networks")->selectSubTab(QStringLiteral("wpn"));
            w = m_app.mw->page(QStringLiteral("networks"));
            break;
        case ScOtherNets:
            m_app.mw->showPage(QStringLiteral("networks"));
            page<NetworksPage>("networks")->selectSubTab(QStringLiteral("other"));
            w = m_app.mw->page(QStringLiteral("networks"));
            break;
        case ScSearch:
            m_app.mw->showPage(QStringLiteral("search"));
            w = page<SearchPage>("search")->currentTab()->list();
            break;
        case ScTransfers:
            m_app.mw->showPage(QStringLiteral("transfers"));
            w = m_app.mw->page(QStringLiteral("transfers"))->findChild<MxListView *>(QStringLiteral("downloadList"));
            break;
        case ScUploads:
            m_app.mw->showPage(QStringLiteral("transfers"));
            w = m_app.mw->page(QStringLiteral("transfers"))->findChild<MxListView *>(QStringLiteral("uploadList"));
            break;
        case ScLibrary:
            m_app.mw->showPage(QStringLiteral("shared"));
            w = m_app.mw->page(QStringLiteral("shared"));
            break;
        case ScHotlist: w = m_app.mw->hotlistPane(); break;
        case ScChatRoom:
        case ScChatUsers:
        case ScChatInput: {
            m_app.mw->showPage(QStringLiteral("chat"));
            page<ChatPage>("chat")->join(QStringLiteral("OctoMX Help"));   // back to the room tab
            ChatRoomView *r = page<ChatPage>("chat")->currentRoom();
            if (!r) return {};
            w = s == ScChatRoom ? static_cast<QWidget *>(r->output()) : s == ScChatUsers ? r->findChild<QWidget *>(QStringLiteral("chatUserList"))
                                                                                    : r->findChild<QWidget *>(QStringLiteral("chatInput"));
            break;
        }
        case ScChannels:
            m_app.mw->showPage(QStringLiteral("chat"));
            page<ChatPage>("chat")->showChannelList();
            w = page<ChatPage>("chat")->channelList();
            break;
        case ScPrivate: w = m_pm; break;
        case ScServerMsgs: w = m_server; break;
        case ScIgnored: w = m_ignored; break;
        case ScBrowse: w = m_browse; break;
        case ScBandwidth:
            m_app.mw->showPage(QStringLiteral("bandwidth"));
            w = m_app.mw->page(QStringLiteral("bandwidth"));
            break;
        case ScConnTest:
            w = ConnectionTestDialog::instance();
            w->resize(w->width(), 1000);   // every row in view
            for (QTreeView *v : w->findChildren<QTreeView *>()) v->expandAll();
            QCoreApplication::processEvents();
            break;
        case ScCraftTransfers: w = m_craftTransferList; break;
        case ScCraftSearch: w = m_craftSearchList; break;
        case ScCraftUsers: w = m_craftUserList; break;
        case ScCraftLed: w = m_leds; break;
        case ScSceneCount: break;
        }
        if (!w) return {};
        QCoreApplication::processEvents();
        if (auto *lv = qobject_cast<MxListView *>(w)) return grabAllRows(lv);
        // a window: itself, then every row of its lists (a screen lower than the window cuts them)
        QList<QImage> parts{w->grab().toImage()};
        if (s == ScConnTest)
            for (QTreeView *v : w->findChildren<QTreeView *>()) parts << grabAllRows(v);
        return stacked(parts);
    }

    // the scenes where `key` must show, and the ones where it must not; empty = nothing to check
    struct Row { const char *key; QList<Scene> in; };
    static QList<Row> table()
    {
        // where each colour shows in the demo situation (both themes), checked against the Appearance
        // page that lists it; the crafted scenes are rows in states the demo has not
        return {
            {"colors/newsBackground", {ScNewsBar}},
            {"colors/newsText", {ScNewsBar}},
            {"colors/newsLink", {ScNewsBar}},
            {"colors/listHeaderText", {ScLibrary, ScChannels, ScBrowse, ScConnTest}},
            {"colors/listGrid", {ScOtherNets, ScSearch, ScTransfers, ScUploads, ScLibrary, ScHotlist, ScChatUsers, ScChannels, ScIgnored, ScBrowse, ScConnTest, ScCraftTransfers, ScCraftSearch, ScCraftUsers}},
            {"colors/inactiveText", {ScBrowse, ScConnTest, ScCraftTransfers, ScCraftSearch}},
            {"colors/statusLedOn", {ScNetworks}},
            {"colors/statusLedConnecting", {ScCraftLed}},
            {"colors/statusLedError", {ScCraftLed}},
            {"colors/statusLedOff", {ScNetworks, ScCraftLed}},
            {"colors/listBackground", {ScOtherNets, ScSearch, ScTransfers, ScUploads, ScLibrary, ScHotlist, ScChannels, ScIgnored, ScBrowse, ScConnTest, ScCraftTransfers, ScCraftSearch, ScCraftUsers}},
            {"colors/listText", {ScOtherNets, ScSearch, ScTransfers, ScLibrary, ScBrowse, ScConnTest, ScCraftTransfers, ScCraftSearch}},
            {"colors/listSelectedBackground", {ScSearch, ScTransfers, ScConnTest}},
            {"colors/serverConnected", {ScOtherNets}},
            {"colors/serverConnecting", {ScOtherNets}},
            {"colors/serverDisconnected", {ScOtherNets}},
            {"colors/chatBackground", {ScChatRoom, ScPrivate, ScServerMsgs}},
            {"colors/chatText", {ScChatRoom, ScPrivate}},
            {"colors/chatSystem", {ScServerMsgs}},
            {"colors/chatError", {ScChatRoom}},
            {"colors/chatMotd", {ScChatRoom, ScServerMsgs}},
            {"colors/chatTimestamp", {ScChatRoom, ScPrivate, ScServerMsgs}},
            {"colors/chatOtherName", {ScChatRoom, ScPrivate}},
            {"colors/chatOwnName", {ScPrivate}},
            {"colors/chatAction", {ScChatRoom}},
            {"colors/chatJoin", {ScChatRoom}},
            {"colors/chatPart", {ScChatRoom}},
            {"colors/chatTopic", {ScChatRoom}},
            {"colors/chatLink", {ScChatRoom, ScPrivate}},
            {"colors/chatInputBackground", {ScChatInput, ScPrivate}},
            {"colors/chatInputText", {ScChatInput}},
            {"colors/chatUserListBackground", {ScChatUsers}},
            {"colors/chatUserListText", {ScChatUsers, ScCraftUsers}},
            {"colors/chatUserAdmin", {ScChatUsers}},
            {"colors/chatUserVoiced", {ScCraftUsers}},
            {"colors/listSelectedText", {ScSearch}},
            {"colors/bitrateLow", {ScSearch, ScBrowse}},
            {"colors/bitrateMedium", {ScSearch, ScBrowse}},
            {"colors/bitrateHigh", {ScSearch, ScBrowse}},
            {"colors/partialText", {ScSearch}},
            {"colors/speedSlow", {ScSearch, ScUploads, ScChatUsers, ScCraftSearch}},
            {"colors/speedMedium", {ScUploads, ScChatUsers, ScCraftSearch, ScCraftUsers}},
            {"colors/speedFast", {ScSearch, ScUploads, ScChatUsers, ScCraftSearch}},
            {"colors/suspectText", {ScCraftSearch}},
            {"colors/available", {ScSearch}},
            {"colors/slotOne", {ScSearch}},
            {"colors/unavailable", {ScCraftSearch}},
            {"colors/pingGood", {ScConnTest}},
            {"colors/pingMedium", {ScConnTest}},
            {"colors/pingBad", {ScConnTest}},
            {"colors/matchIncomplete", {ScBrowse}},
            {"colors/canJoinDownload", {ScSearch, ScBrowse}},
            {"colors/currentlyDownloading", {ScBrowse}},
            {"colors/networkLabel", {ScCraftTransfers}},
            {"colors/channelName", {ScChannels}},
            {"colors/channelUsers", {ScChannels}},
            {"colors/channelTopic", {ScChannels}},
            {"colors/hotlistHeader", {ScHotlist}},
            {"colors/hotlistTemporary", {ScHotlist}},
            {"colors/userOnline", {ScHotlist}},
            {"colors/userOffline", {ScHotlist}},
            {"colors/ignoredText", {ScIgnored}},
            {"colors/transferActive", {ScTransfers, ScUploads, ScCraftTransfers}},
            {"colors/transferQueued", {ScTransfers}},
            {"colors/transferComplete", {ScTransfers}},
            {"colors/transferError", {ScTransfers, ScUploads, ScCraftTransfers}},
            {"colors/transferPaused", {ScTransfers, ScCraftTransfers}},
            {"colors/transferConnecting", {ScTransfers, ScUploads}},
            {"colors/transferNegotiating", {ScCraftTransfers}},
            {"colors/transferRemoteQueued", {ScTransfers, ScUploads}},
            {"colors/transferBusy", {ScCraftTransfers}},
            {"colors/transferCancelled", {ScCraftTransfers}},
            {"colors/progressBar", {ScTransfers, ScUploads, ScCraftTransfers}},
            {"colors/progressBackground", {ScTransfers, ScUploads, ScCraftTransfers}},
            {"colors/progressActive", {ScTransfers}},
            {"colors/graphBackground", {ScBandwidth}},
            {"colors/graphGrid", {ScBandwidth}},
            {"colors/graphText", {ScBandwidth}},
            {"colors/graphUp", {ScBandwidth}},
            {"colors/graphDown", {ScBandwidth}},
            {"colors/graphOverlay", {ScBandwidth}},
            {"colors/graphPackets", {ScBandwidth}},
            {"colors/graphIncomingPeers", {ScBandwidth}},
            {"colors/graphOutgoingPeers", {ScBandwidth}},
            {"colors/graphServerIn", {ScBandwidth}},
            {"colors/graphServerOut", {ScBandwidth}},
            {"colors/graphOther", {ScBandwidth}},
        };
    }

    QString checkTheme(const QString &theme)
    {
        st()->setValue(keys::ui::ColorTheme, theme);
        QTest::qWait(30);
        const bool map = qEnvironmentVariableIsSet("MX_SET1_COLOR_MAP");
        QStringList errors;
        for (const Row &r : table()) {
            const QString key = QString::fromLatin1(r.key);
            const QString before = Colors::instance()->get(key).name();
            st()->setValue(key, kProbe.name());
            QTest::qWait(5);
            QStringList shows;
            for (int s = 0; s < ScSceneCount; ++s) {
                const QImage img = render(Scene(s));
                if (img.isNull()) continue;
                const int n = countOf(img, kProbe);
                if (n > 0) shows << QStringLiteral("%1(%2)").arg(QLatin1String(kSceneNames[s])).arg(n);
                const bool expected = r.in.contains(Scene(s));
                if (expected && n == 0)
                    errors << QStringLiteral("%1 [%2]: not in %3").arg(key, theme, QLatin1String(kSceneNames[s]));
                if (!expected && n > 0 && kNotALista.contains(Scene(s)) != r.in.contains(ScBandwidth))
                    errors << QStringLiteral("%1 [%2]: also in %3 (%4 pixels), a window of another kind").arg(key, theme, QLatin1String(kSceneNames[s])).arg(n);
            }
            if (map) qInfo("%s [%s]: %s", r.key, qPrintable(theme), qPrintable(shows.join(QLatin1Char(' '))));
            st()->setValue(key, before);
        }
        return errors.join(QLatin1Char('\n'));
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_app.start(m_dir.filePath(QStringLiteral("profile")));
        // the demo situation: every row state the pages colour
        st()->setValue(keys::ui::ShowGrid, true);
        st()->setValue(keys::chat::ShowTimestamps, true);
        m_app.mw->setHotlistVisible(true);
        m_app.mw->showPage(QStringLiteral("search"));
        SearchTab *t = page<SearchPage>("search")->currentTab();
        t->setTexts(QStringLiteral("velorine"), QString());
        t->startSearch();
        QTest::qWait(30);
        t->flushPending();
        t->list()->selectRow(t->list()->visibleRow(0));
        m_app.mw->showPage(QStringLiteral("transfers"));
        auto *dl = m_app.mw->page(QStringLiteral("transfers"))->findChild<MxListView *>(QStringLiteral("downloadList"));
        QVERIFY(dl);
        dl->selectRow(dl->visibleRow(0));
        // chat: a room with lines of every kind
        m_app.mw->showPage(QStringLiteral("chat"));
        page<ChatPage>("chat")->join(QStringLiteral("OctoMX Help"));
        QTRY_VERIFY(page<ChatPage>("chat")->currentRoom());
        const QString room = page<ChatPage>("chat")->currentRoom()->roomId();
        say(room, ChatKind::Message, QStringLiteral("Speaker_10101"), QStringLiteral("a line of text"));
        say(room, ChatKind::Action, QStringLiteral("Speaker_10101"), QStringLiteral("waves"));
        say(room, ChatKind::Join, QStringLiteral("Comer_20202"), QString());
        say(room, ChatKind::Part, QStringLiteral("Goer_30303"), QString());
        say(room, ChatKind::Topic, QStringLiteral("Speaker_10101"), QStringLiteral("Big Buck Bunny night"));
        say(room, ChatKind::Motd, QString(), QStringLiteral("Welcome to the room"));
        say(room, ChatKind::Error, QString(), QStringLiteral("No such user"));
        page<ChatPage>("chat")->currentRoom()->findChild<QLineEdit *>(QStringLiteral("chatInput"))->setText(QStringLiteral("typing"));
        // a private message window: my line, the other's line with a link, a system line
        const UserRef u = uib::user(QStringLiteral("Friend_40404"));
        m_app.mw->openPrivateMessage(u);
        QTRY_VERIFY((m_pm = PrivateChatHub::instance(m_app.mw)->window(u)) != nullptr);
        m_pm->submit(QStringLiteral("hello friend"));
        ChatMessage in;
        in.kind = ChatKind::Private;
        in.from = u;
        in.text = QStringLiteral("see https://www.bigbuckbunny.org");
        in.time = QDateTime::currentDateTime();
        emit m_app.core->chat()->messageReceived(in);
        QTest::qWait(30);
        m_pm->resize(500, 300);
        // server messages, ignored users, browse, connection test
        m_server = new ServerMessagesWindow(m_app.core, Net::ED2K, QString());
        m_server->resize(500, 300);
        m_server->show();
        m_app.core->users()->ignore(uib::user(QStringLiteral("Noisy_50505")), true);
        m_app.mw->openIgnored();
        QTRY_VERIFY((m_ignored = [] { for (QWidget *w : QApplication::allWidgets()) if (qobject_cast<IgnoredWindow *>(w) && w->isVisible()) return w; return static_cast<QWidget *>(nullptr); }()) != nullptr);
        m_app.mw->openBrowse(uib::user(QStringLiteral("Sharer_60606")));
        QTRY_VERIFY((m_browse = [] { for (QWidget *w : QApplication::allWidgets()) if (auto *b = qobject_cast<BrowseWindow *>(w); b && b->isVisible()) return b; return static_cast<BrowseWindow *>(nullptr); }()) != nullptr);
        QTRY_VERIFY(!m_browse->isLoading());
        ConnectionTestDialog::open(m_app.core, m_app.mw);
        m_app.mw->newsBar()->setText(QStringLiteral("Big Buck Bunny is free to share"), QStringLiteral("https://www.bigbuckbunny.org"));
        m_app.core->network()->disconnectNetwork(Net::Kad);   // a network row "Not connected"
        buildCraftedScenes();
        QTest::qWait(100);
    }
    void cleanupTestCase()
    {
        delete m_craftTransferList;
        delete m_craftSearchList;
        delete m_craftUserList;
        delete m_leds;
        delete m_server;
        m_app.stop();
    }

    void everyColourInItsWindowDark() { const QString e = checkTheme(QStringLiteral("dark")); QVERIFY2(e.isEmpty(), qPrintable(e)); }
    void everyColourInItsWindowLight() { const QString e = checkTheme(QStringLiteral("light")); QVERIFY2(e.isEmpty(), qPrintable(e)); }

    // every colour of the schema is in the table (or explained there with an empty list)
    void tableCoversEveryColour()
    {
        QStringList listed, missing;
        for (const Row &r : table()) listed << QString::fromLatin1(r.key);
        for (const SettingDef &d : settingsSchema())
            if (d.type == SettingType::Color && !d.hidden && !listed.contains(d.key)) missing << d.key;
        QVERIFY2(missing.isEmpty(), qPrintable(missing.join(QLatin1Char(' '))));
    }
};

QTEST_MAIN(TstSet1Colors)
#include "tst_set1_colors.moc"

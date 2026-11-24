// SPDX-License-Identifier: GPL-3.0-or-later
// [set-1] Behaviour tests of the settings the windows read: each test changes the setting as the
// Settings dialog does and checks what the user sees (a window that opens or not, the text and font of
// a chat line, which files a search tab shows, the program a file is opened with, the page shown at
// start, the warning before a risky download). Plus the dialog's own promises: the restart message,
// the defaults the tool tips name, Reset Defaults, and the wizard showing the same state as the pages.
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QRegularExpression>
#include <QtCore/QTemporaryDir>
#include <QtGui/QFontDatabase>
#include <QtTest/QSignalSpy>
#include <QtTest/QtTest>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QCompleter>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QSpinBox>

#include "mock/mock_core.h"
#include "mx/services.h"
#include "mx/settings_keys.h"
#include "ui/dialogs/browse_window.h"
#include "ui/dialogs/settings_binding.h"
#include "ui/dialogs/settings_dialog.h"
#include "ui/dialogs/settings_pages.h"
#include "ui/dialogs/setup_wizard.h"
#include "ui/dialogs/user_windows.h"
#include "ui/hotlist_pane.h"
#include "ui/kit/launcher.h"
#include "ui/kit/list_view.h"
#include "ui/kit/scale.h"
#include "ui/kit/session.h"
#include "ui/kit/theme.h"
#include "ui/kit/widgets.h"
#include "ui/main_window.h"
#include "ui/os_integration.h"
#include "ui/models/browse_model.h"
#include "ui/models/chat_models.h"
#include "ui/models/hotlist_model.h"
#include "ui/models/search_model.h"
#include "ui/pages/chat_page.h"
#include "ui/pages/chat_view.h"
#include "ui/pages/search_page.h"
#include "uib_helpers.h"

using namespace mx;
using namespace mx::ui;

namespace {

ChatMessage pm(const UserRef &from, const QString &text)
{
    ChatMessage m;
    m.kind = ChatKind::Private;
    m.from = from;
    m.text = text;
    m.time = QDateTime::currentDateTime();
    return m;
}

QList<PrivateMessageWindow *> pmWindows()
{
    QList<PrivateMessageWindow *> out;
    for (QWidget *w : QApplication::topLevelWidgets())
        if (auto *p = qobject_cast<PrivateMessageWindow *>(w); p && p->isVisible()) out << p;
    for (QWidget *w : QApplication::allWidgets())
        if (auto *p = qobject_cast<PrivateMessageWindow *>(w); p && p->isVisible() && !out.contains(p)) out << p;
    return out;
}

template <class T> T *visibleWindow()
{
    for (QWidget *w : QApplication::allWidgets())
        if (auto *p = qobject_cast<T *>(w); p && p->isVisible()) return p;
    return nullptr;
}

} // namespace

class TstSet1 : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    uib::App m_app;
    int m_flashes = 0;

    Settings *st() const { return m_app.core->settings(); }
    ChatPage *chat() const { return qobject_cast<ChatPage *>(m_app.mw->page(QStringLiteral("chat"))); }
    SearchPage *search() const { return qobject_cast<SearchPage *>(m_app.mw->page(QStringLiteral("search"))); }
    QString file(const QString &name)
    {
        const QString p = m_dir.filePath(name);
        QFile f(p);
        if (f.open(QIODevice::WriteOnly)) f.write("x");
        return p;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_app.start(m_dir.filePath(QStringLiteral("profile")));
        MainWindow::s_flashHook = [this] { ++m_flashes; };
    }
    void cleanupTestCase()
    {
        MainWindow::s_flashHook = {};
        launcher::setHook({});
        m_app.stop();
    }

    // ------------------------------------------------------------------ File Launching
    // launch/videoCommand, launch/imageCommand, launch/documentCommand, launch/otherCommand (with
    // launch/useOsDefault off): each type opens with its own program, %1 = the file, the system's
    // program when the box is on or the field empty
    void programPerFileType()
    {
        QStringList calls;
        launcher::setHook([&calls](const QString &action, const QString &path) {
            calls << action + QLatin1Char(' ') + path;
            return true;
        });
        const struct { QLatin1StringView key; const char *file; const char *program; } rows[] = {
            {keys::launch::VideoCommand, "film.mkv", "vplayer --fullscreen %1"},
            {keys::launch::ImageCommand, "photo.png", "viewer %1"},
            {keys::launch::DocumentCommand, "notes.pdf", "reader --page 1 %1"},
            {keys::launch::OtherCommand, "data.iso", "mounter %1"},
        };
        st()->setValue(keys::launch::UseOsDefault, false);
        for (const auto &r : rows)
            st()->setValue(r.key, QString::fromLatin1(r.program));
        for (const auto &r : rows) {
            const QString path = file(QString::fromLatin1(r.file));
            calls.clear();
            QVERIFY(launcher::openFile(nullptr, st(), path));
            QString want = QString::fromLatin1(r.program);
            want.replace(QStringLiteral("%1"), QDir::toNativeSeparators(path));
            QCOMPARE(calls, QStringList{QStringLiteral("command ") + want});
        }
        // the system's program: with the box on, and for a type whose field is empty
        st()->setValue(keys::launch::UseOsDefault, true);
        calls.clear();
        QVERIFY(launcher::openFile(nullptr, st(), file(QStringLiteral("film.mkv"))));
        QCOMPARE(calls.size(), 1);
        QVERIFY2(calls.first().startsWith(QStringLiteral("open ")), qPrintable(calls.first()));
        st()->setValue(keys::launch::UseOsDefault, false);
        st()->setValue(keys::launch::ImageCommand, QString());
        calls.clear();
        QVERIFY(launcher::openFile(nullptr, st(), file(QStringLiteral("photo.png"))));
        QVERIFY2(calls.first().startsWith(QStringLiteral("open ")), qPrintable(calls.first()));
        launcher::setHook({});
        st()->resetToDefaults(QStringLiteral("launch/"));
    }

    // ------------------------------------------------------------------ private messages
    // messages/openWindowOnMessage, messages/flashOnMessage, chat/showTimestamps,
    // chat/timestampFormat, chat/fontFamily, chat/fontSize (the private message window)
    void privateMessageWindows()
    {
        const UserRef a = uib::user(QStringLiteral("Quiet_10001")), b = uib::user(QStringLiteral("Loud_10002"));
        st()->setValue(keys::messages::OpenWindowOnMessage, false);
        st()->setValue(keys::messages::FlashOnMessage, false);
        const int windows = int(pmWindows().size());
        m_flashes = 0;
        emit m_app.core->chat()->messageReceived(pm(a, QStringLiteral("no window please")));
        QTest::qWait(50);
        QCOMPARE(int(pmWindows().size()), windows);
        QCOMPARE(m_flashes, 0);
        // on: a window opens and the window flashes
        st()->setValue(keys::messages::OpenWindowOnMessage, true);
        st()->setValue(keys::messages::FlashOnMessage, true);
        st()->setValue(keys::chat::ShowTimestamps, true);
        st()->setValue(keys::chat::TimestampFormat, QStringLiteral("hh:mm:ss"));
        emit m_app.core->chat()->messageReceived(pm(b, QStringLiteral("hello there")));
        PrivateMessageWindow *w = nullptr;
        QTRY_VERIFY((w = PrivateChatHub::instance(m_app.mw)->window(b)) != nullptr);
        QCOMPARE(m_flashes, 1);
        QTRY_VERIFY(w->output()->plainText().contains(QStringLiteral("hello there")));
        static const QRegularExpression stamp(QStringLiteral("\\[\\d\\d:\\d\\d:\\d\\d\\] [^\\n]*hello there"));
        QVERIFY2(stamp.match(w->output()->plainText()).hasMatch(), qPrintable(w->output()->plainText()));
        // the window keeps showing later messages; a timestamp off shows none
        st()->setValue(keys::chat::ShowTimestamps, false);
        PrivateChatHub::instance(m_app.mw)->window(b)->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        emit m_app.core->chat()->messageReceived(pm(b, QStringLiteral("second line")));
        QTRY_VERIFY((w = PrivateChatHub::instance(m_app.mw)->window(b)) != nullptr);
        QTRY_VERIFY(w->output()->plainText().contains(QStringLiteral("second line")));
        static const QRegularExpression anyStamp(QStringLiteral("\\[\\d\\d:\\d\\d(:\\d\\d)?\\] [^\\n]*second line"));
        QVERIFY(!anyStamp.match(w->output()->plainText()).hasMatch());
        // the chat font and size
        const int before = QFontInfo(w->output()->font()).pixelSize();
        st()->setValue(keys::chat::FontSize, 20);                // an open window follows at once
        QTRY_VERIFY2(QFontInfo(w->output()->font()).pixelSize() > before, qPrintable(QString::number(QFontInfo(w->output()->font()).pixelSize())));
        QString other;
        for (const QString &f : QFontDatabase::families())
            if (f.compare(w->output()->font().family(), Qt::CaseInsensitive) != 0 && !QFontDatabase::isPrivateFamily(f)) other = f;
        QVERIFY(!other.isEmpty());
        st()->setValue(keys::chat::FontFamily, other);
        QCOMPARE(chatFont(st()).family(), other);
        st()->setValue(keys::chat::FontFamily, QStringLiteral("No Such Font Family"));
        QVERIFY(chatFont(st()).family() != QLatin1String("No Such Font Family"));   // not installed: the standard font
        w->close();
        st()->resetToDefaults(QStringLiteral("chat/"));
        st()->resetToDefaults(QStringLiteral("messages/"));
    }

    // chat/showTimestamps, chat/timestampFormat, chat/fontSize in a chat room (live)
    void chatRoomLines()
    {
        m_app.mw->showPage(QStringLiteral("chat"));
        QTest::qWait(50);
        chat()->join(QStringLiteral("OctoMX Help"));
        ChatRoomView *room = nullptr;
        QTRY_VERIFY((room = chat()->currentRoom()) != nullptr);
        const QString roomId = room->roomId();
        auto say = [&](const QString &text) {
            ChatMessage m;
            m.roomId = roomId;
            m.kind = ChatKind::Message;
            m.from = uib::user(QStringLiteral("Talker_12121"));
            m.text = text;
            m.time = QDateTime::currentDateTime();
            emit m_app.core->chat()->messageReceived(m);
        };
        st()->setValue(keys::chat::ShowTimestamps, true);
        st()->setValue(keys::chat::TimestampFormat, QStringLiteral("hh.mm.ss"));
        say(QStringLiteral("stamped line"));
        QTRY_VERIFY(room->output()->plainText().contains(QStringLiteral("stamped line")));
        static const QRegularExpression stamp(QStringLiteral("\\[\\d\\d\\.\\d\\d\\.\\d\\d\\] [^\\n]*stamped line"));
        QVERIFY2(stamp.match(room->output()->plainText()).hasMatch(), qPrintable(room->output()->plainText().right(200)));
        st()->setValue(keys::chat::ShowTimestamps, false);
        say(QStringLiteral("plain line"));
        QTRY_VERIFY(room->output()->plainText().contains(QStringLiteral("plain line")));
        static const QRegularExpression stamped(QStringLiteral("\\] [^\\n]*plain line"));
        QVERIFY(!stamped.match(room->output()->plainText()).hasMatch());
        const int before = QFontInfo(room->output()->font()).pixelSize();
        st()->setValue(keys::chat::FontSize, 22);
        QTRY_VERIFY(QFontInfo(room->output()->font()).pixelSize() > before);   // at once
        st()->resetToDefaults(QStringLiteral("chat/"));
    }

    // chat/fetchListImmediately, chat/topicInFilter (the channel list)
    void channelList()
    {
        m_app.mw->showPage(QStringLiteral("chat"));
        QSignalSpy fetched(m_app.core->chat(), &ChatService::roomListChanged);
        auto reopen = [&] {
            if (ChannelListView *l = chat()->channelList()) emit l->closeRequested();
            QTest::qWait(20);
            chat()->showChannelList();
            QTest::qWait(50);
        };
        st()->setValue(keys::chat::FetchListImmediately, false);
        reopen();
        QCOMPARE(fetched.count(), 0);                       // waits for Refresh
        st()->setValue(keys::chat::FetchListImmediately, true);
        reopen();
        QTRY_VERIFY(fetched.count() >= 1);
        // the filter with and without the topics
        ChannelListView *l = chat()->channelList();
        QVERIFY(l);
        QString word;
        const QList<ChatRoomInfo> rooms = m_app.core->chat()->roomList();
        for (const ChatRoomInfo &r : rooms) {
            for (const QString &w : r.topic.split(QRegularExpression(QStringLiteral("\\W+")), Qt::SkipEmptyParts)) {
                bool inName = false;
                for (const ChatRoomInfo &x : rooms) inName |= x.name.contains(w, Qt::CaseInsensitive);
                if (w.size() >= 4 && !inName) { word = w; break; }
            }
            if (!word.isEmpty()) break;
        }
        QVERIFY2(!word.isEmpty(), "a topic word that no room name has");
        l->edit()->setText(word);
        auto *filter = l->findChild<QAbstractButton *>(QStringLiteral("chatFilter"));
        QVERIFY(filter);
        filter->setChecked(true);
        st()->setValue(keys::chat::TopicInFilter, false);
        QCOMPARE(l->proxy()->rowCount(), 0);
        st()->setValue(keys::chat::TopicInFilter, true);
        QTRY_VERIFY(l->proxy()->rowCount() > 0);
        filter->setChecked(false);
        l->edit()->clear();
        st()->resetToDefaults(QStringLiteral("chat/"));
    }

    // hotlist/autoBrowse: a double-click on a Hotlist user opens his files or a message window
    void hotlistDoubleClick()
    {
        m_app.mw->setHotlistVisible(true);
        HotlistPane *pane = m_app.mw->hotlistPane();
        QVERIFY(pane);
        int row = -1;
        for (int r = 0; r < pane->model()->rowCount(); ++r)
            if (pane->model()->kind(r) != HotlistModel::SectionRow) { row = r; break; }
        QVERIFY(row >= 0);
        const UserRef u = pane->model()->user(row);
        st()->setValue(keys::hotlist::AutoBrowse, true);
        emit pane->list()->rowActivated(pane->list()->model()->index(row, 0));
        BrowseWindow *bw = nullptr;
        QTRY_VERIFY((bw = visibleWindow<BrowseWindow>()) != nullptr);
        QCOMPARE(bw->user().key(), u.key());
        bw->close();
        QTest::qWait(20);
        st()->setValue(keys::hotlist::AutoBrowse, false);
        emit pane->list()->rowActivated(pane->list()->model()->index(row, 0));
        QTRY_VERIFY(PrivateChatHub::instance(m_app.mw)->window(u) != nullptr);
        PrivateChatHub::instance(m_app.mw)->window(u)->close();
        st()->resetToDefaults(QStringLiteral("hotlist/"));
    }

    // ------------------------------------------------------------------ Search
    // search/defaultType, search/defaultConnection, search/typeButton* and their captions,
    // search/autoFindSourcesMin and search/autoEnterQueue (the new tab's buttons), search/showFullPaths,
    // search/autoComplete, ui/searchHistoryMenu
    void searchTabSettings()
    {
        st()->setValue(keys::search::DefaultType, QStringLiteral("video"));
        st()->setValue(keys::search::DefaultConnection, connectionTextForRank(6));
        st()->setValue(keys::search::TypeButtonAudio, QStringLiteral("lossless"));
        st()->setValue(keys::search::TypeButtonAudioLabel, QStringLiteral("FLAC"));
        st()->setValue(keys::search::TypeButtonVideo, QStringLiteral("torrents"));
        st()->setValue(keys::search::TypeButtonVideoLabel, QStringLiteral("Torrents"));
        st()->setValue(keys::search::TypeButtonPictures, QStringLiteral("custom:iso,img"));
        st()->setValue(keys::search::TypeButtonPicturesLabel, QStringLiteral("Images"));
        st()->setValue(keys::search::AutoFindSourcesMin, 25);
        st()->setValue(keys::search::AutoEnterQueue, 15);
        st()->setValue(keys::search::ShowFullPaths, true);
        m_app.mw->showPage(QStringLiteral("search"));
        SearchTab *t = search()->newTab(true);
        QVERIFY(t);
        QCOMPARE(t->fileTypeCode(), QStringLiteral("video"));
        auto *conn = t->findChild<MenuButton *>(QStringLiteral("searchConnection"));
        QVERIFY(conn);
        QCOMPARE(conn->currentData().toInt(), connectionRankForText(connectionTextForRank(6)));
        auto *audio = t->findChild<QAbstractButton *>(QStringLiteral("searchAudio"));
        auto *pictures = t->findChild<QAbstractButton *>(QStringLiteral("searchPictures"));
        QCOMPARE(audio->text(), QStringLiteral("FLAC"));
        QCOMPARE(pictures->text(), QStringLiteral("Images"));
        auto *video = t->findChild<QAbstractButton *>(QStringLiteral("searchVideo"));
        QCOMPARE(video->text(), QStringLiteral("Torrents"));
        video->click();
        QCOMPARE(t->fileTypeCode(), QStringLiteral("torrents"));
        audio->click();
        QCOMPARE(t->fileTypeCode(), QStringLiteral("lossless"));
        pictures->click();
        QCOMPARE(t->fileTypeCode(), QStringLiteral("custom:iso,img"));
        QCOMPARE(t->findChild<MenuButton *>(QStringLiteral("searchAfs"))->currentData().toInt(), 25);
        QCOMPARE(t->findChild<MenuButton *>(QStringLiteral("searchAeq"))->currentData().toInt(), 15);
        // results with their folder (a new tab used to latch the button but show names only)
        t->setFileTypeCode(QStringLiteral("any"));
        t->setTexts(QStringLiteral("velorine"), QString());
        t->startSearch();
        QTest::qWait(20);
        t->flushPending();
        int withPath = -1;
        for (int r = 0; r < t->model()->hitCount(); ++r)
            if (!t->model()->hitAt(r).path.isEmpty()) { withPath = r; break; }
        QVERIFY2(withPath >= 0, "a demo hit with a path");
        const QModelIndex cell = t->model()->index(withPath, 0);
        QCOMPARE(cell.data().toString(), t->model()->hitAt(withPath).path);
        st()->setValue(keys::search::ShowFullPaths, false);   // from the Settings dialog: live
        QCOMPARE(t->model()->index(withPath, 0).data().toString(), t->model()->hitAt(withPath).fileName);
        // completion from earlier searches
        st()->setValue(keys::search::AutoComplete, true);
        QVERIFY(t->keywords()->completer());
        QVERIFY(t->keywords()->completer()->model()->rowCount() > 0);
        st()->setValue(keys::search::AutoComplete, false);
        QVERIFY(!t->keywords()->completer());
        // the recent searches arrow
        auto *hist = t->findChild<QAction *>(QStringLiteral("searchHistoryButton"));
        QVERIFY(hist);
        QVERIFY(hist->isVisible());
        st()->setValue(keys::ui::SearchHistoryMenu, false);
        QVERIFY(!hist->isVisible());
        search()->closeTab(t);
        st()->resetToDefaults(QStringLiteral("search/"));
        st()->resetToDefaults(QStringLiteral("ui/searchHistoryMenu"));
    }

    // search/customExtensions: the last list typed in Custom Extensions... is offered again
    void customExtensions()
    {
        m_app.mw->showPage(QStringLiteral("search"));
        st()->setValue(keys::search::CustomExtensions, QStringLiteral("mkv webm"));
        SearchTab *t = search()->newTab(true);
        QString offered;
        SearchTab::s_customExtensionsHook = [&offered](QString *text) {
            offered = *text;
            *text = QStringLiteral("srt sub");
            return true;
        };
        emit t->findChild<MenuButton *>(QStringLiteral("searchFileType"))->choiceChanged(QStringLiteral("custom"));   // Custom Extensions...
        QTRY_VERIFY(!offered.isEmpty());
        QVERIFY2(offered.contains(QStringLiteral("mkv")) && offered.contains(QStringLiteral("webm")), qPrintable(offered));
        QTRY_COMPARE(t->fileTypeCode(), QStringLiteral("custom:srt,sub"));
        QCOMPARE(st()->s(keys::search::CustomExtensions), QStringLiteral("srt sub"));
        SearchTab::s_customExtensionsHook = {};
        search()->closeTab(t);
        st()->resetToDefaults(QStringLiteral("search/"));
    }

    // transfers/suppressDangerousWarning in the Browse window (it never warned there)
    void browseWarnsBeforeRiskyFiles()
    {
        QStringList asked;
        int answer = QMessageBox::No;
        BrowseWindow::s_askHook = [&](const QString &text) {
            asked << text;
            return answer;
        };
        const UserRef u = uib::user(QStringLiteral("Sharer_31313"));
        m_app.mw->openBrowse(u);
        BrowseWindow *bw = nullptr;
        QTRY_VERIFY((bw = visibleWindow<BrowseWindow>()) != nullptr);
        QTRY_VERIFY(!bw->isLoading() && bw->model()->entryCount() > 0);
        BrowseEntry exe, clip;
        exe.path = QStringLiteral("D:\\Shared\\Tools\\bunny-player-setup.exe");
        exe.size = 3000000;
        exe.hashes.ed2k = QByteArray(16, 'e');
        clip.path = QStringLiteral("D:\\Shared\\Video\\Big Buck Bunny trailer.mkv");
        clip.size = 4000000;
        clip.hashes.ed2k = QByteArray(16, 'c');
        bw->model()->append({exe, clip});
        int risky = -1, plain = -1;
        for (int e = 0; e < bw->model()->entryCount(); ++e) {
            if (bw->model()->entry(e).path == exe.path) risky = e;
            if (bw->model()->entry(e).path == clip.path) plain = e;
        }
        QVERIFY(risky >= 0 && plain >= 0);
        QSignalSpy added(m_app.core->transfers(), &TransferService::transferAdded);
        bw->downloadEntries({risky, plain});
        QCOMPARE(asked.size(), 1);
        QVERIFY(asked.first().contains(QStringLiteral("This is a program or script")));
        QCOMPARE(added.count(), 1);                  // No: only the harmless file
        st()->setValue(keys::transfers::SuppressDangerousWarning, true);
        asked.clear();
        bw->downloadEntries({risky});
        QCOMPARE(asked.size(), 0);
        st()->setValue(keys::transfers::SuppressDangerousWarning, false);
        Session::instance()->setSuppressDangerousWarning(true);   // "this session only"
        bw->downloadEntries({risky});
        QCOMPARE(asked.size(), 0);
        Session::instance()->setSuppressDangerousWarning(false);
        BrowseWindow::s_askHook = {};
        bw->close();
    }

    // ------------------------------------------------------------------ the Settings dialog itself
    // the restart message on closing: once, with the settings that need a restart; every such
    // control says so in its tool tip
    void restartMessageAndTips()
    {
        QStringList shown;
        SettingsBinder::setMessageHook([&shown](QWidget *, const QString &, const QString &text) { shown << text; });
        {
            SettingsDialog d(m_app.core, m_app.mw);
            for (const char *k : {"ui/scale", "ui/smoothFonts", "misc/languageFile"}) {
                const SettingDef *def = findSetting(QString::fromLatin1(k));
                QVERIFY2(def && def->restartRequired, k);
            }
            QStringList restartKeys;
            for (QWidget *w : d.findChildren<QWidget *>()) {
                const QString keysOf = w->property(kKeyProp).toString();
                if (keysOf.isEmpty() || w->property(kAdvancedProp).toBool()) continue;
                for (const QString &k : keysOf.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                    const SettingDef *def = findSetting(k);
                    if (!def || !def->restartRequired) continue;
                    restartKeys << k;
                    QVERIFY2(w->toolTip().contains(QStringLiteral("restart"), Qt::CaseInsensitive),
                             qPrintable(k + QStringLiteral(": ") + w->toolTip()));
                }
            }
            QVERIFY(restartKeys.contains(QStringLiteral("ui/scale")));
            QVERIFY(d.binder()->commit(QString(keys::ui::Scale), QStringLiteral("150")));
            QVERIFY(d.binder()->commit(QString(keys::ui::SmoothFonts), QStringLiteral("on")));
            QVERIFY(d.binder()->commit(QString(keys::chat::FontSize), 12));   // applies at once: not listed
            d.close();
        }
        QCOMPARE(shown.size(), 1);
        QVERIFY(shown.first().contains(settingLabel(QStringLiteral("ui/scale"))));
        QVERIFY(shown.first().contains(findSetting(QStringLiteral("ui/smoothFonts"))->label));
        QVERIFY(!shown.first().contains(findSetting(QStringLiteral("chat/fontSize"))->label));
        SettingsBinder::setMessageHook({});
        st()->resetToDefaults(QStringLiteral("ui/scale"));
        st()->resetToDefaults(QStringLiteral("ui/smoothFonts"));
        st()->resetToDefaults(QStringLiteral("chat/"));
    }

    // the "Default: ..." of every tool tip is the default the program really uses, and Reset
    // Defaults gives exactly those values back
    void defaultsTheTipsName()
    {
        SettingsDialog d(m_app.core, m_app.mw);
        QStringList errors;
        for (const QString &k : d.pageKeys()) {
            const SettingDef *def = findSetting(k);
            if (!def) continue;
            const QString tip = settingTip(k);
            static const QRegularExpression rx(QStringLiteral("Default: ([^.]+)\\."));
            const auto m = rx.match(tip);
            if (!m.hasMatch()) continue;
            const QString said = m.captured(1).trimmed();
            QString real;
            switch (def->type) {
            case SettingType::Bool: real = def->defaultValue.toBool() ? QStringLiteral("on") : QStringLiteral("off"); break;
            case SettingType::Int:
            case SettingType::Double: real = def->defaultValue.toString(); break;
            case SettingType::Enum: {
                const int i = int(def->choices.indexOf(def->defaultValue.toString()));
                real = i >= 0 && i < def->choiceLabels.size() ? def->choiceLabels.at(i) : def->defaultValue.toString();
                break;
            }
            default: continue;
            }
            if (said.compare(real, Qt::CaseInsensitive) != 0 && !said.startsWith(real, Qt::CaseInsensitive)
                && !real.startsWith(said, Qt::CaseInsensitive))
                errors << QStringLiteral("%1: the tip says \"%2\", the default is \"%3\"").arg(k, said, real);
        }
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char('\n'))));

        // Reset Defaults (File Transfers page) and the binder's reset of every page key
        QVERIFY(d.showPage(QStringLiteral("transfers")));
        const QStringList keysOfPage = d.binder()->keysOf(d.pageWidget(QStringLiteral("transfers")));
        for (const QString &k : keysOfPage) {
            const SettingDef *def = findSetting(k);
            if (def->type == SettingType::Bool) st()->setValue(k, !def->defaultValue.toBool());
            else if (def->type == SettingType::Int) st()->setValue(k, def->min.toLongLong() + 1);
            else if (def->type == SettingType::Enum) st()->setValue(k, def->choices.last());
        }
        d.findChild<QAbstractButton *>(QStringLiteral("transfersResetDefaults"))->click();
        for (const QString &k : keysOfPage)
            if (k != QLatin1String(keys::transfers::IncomingDir))
                QVERIFY2(st()->value(k) == findSetting(k)->defaultValue, qPrintable(k));
        // every visible setting: reset gives the schema default back
        QStringList all = d.pageKeys();
        for (const QString &k : std::as_const(all)) {
            const SettingDef *def = findSetting(k);
            if (def && def->type == SettingType::Bool) st()->setValue(k, !def->defaultValue.toBool());
        }
        d.binder()->resetKeys(all);
        QStringList wrong;
        for (const QString &k : std::as_const(all)) {
            const SettingDef *def = findSetting(k);
            if (def && def->type == SettingType::Bool && st()->b(k) != def->defaultValue.toBool()) wrong << k;
        }
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join(QLatin1Char(' '))));
        d.close();
    }

    // the wizard shows what the Settings pages show (it used to tick every network and the
    // associations whatever was set)
    void wizardShowsTheSettings()
    {
        st()->setValue(keys::gnutella::Enabled, false);
        st()->setValue(keys::rats::Enabled, false);
        st()->setValue(keys::misc::AssociateTorrent, false);
        st()->setValue(keys::misc::StartWithOs, true);
        st()->setValue(keys::network::ConnectOnStart, false);
        st()->setValue(keys::identity::Connection, QStringLiteral("FTTB"));
        {
            SetupWizard w(m_app.core, nullptr);
            QVERIFY(!w.findChild<QCheckBox *>(QStringLiteral("wizNet_gnutella"))->isChecked());
            QVERIFY(!w.findChild<QCheckBox *>(QStringLiteral("wizNet_rats"))->isChecked());
            QVERIFY(w.findChild<QCheckBox *>(QStringLiteral("wizNet_bt"))->isChecked());
            QCOMPARE(w.findChild<QCheckBox *>(QStringLiteral("wizAssoc"))->checkState(), Qt::PartiallyChecked);
            QVERIFY(w.findChild<QCheckBox *>(QStringLiteral("wizAutostart"))->isChecked());
            QVERIFY(!w.findChild<QCheckBox *>(QStringLiteral("wizConnectNow"))->isChecked());
            bool conn = false;
            for (QRadioButton *r : w.findChildren<QRadioButton *>())
                if (r->property("mxValue").toString() == QLatin1String("FTTB")) conn = r->isChecked();
            QVERIFY(conn);
            // "Let me choose": finishing without a change keeps every value
            w.findChild<QRadioButton *>(QStringLiteral("wizCustom"))->setChecked(true);
            w.applySettings();
        }
        QVERIFY(!st()->b(keys::gnutella::Enabled));
        QVERIFY(!st()->b(keys::rats::Enabled));
        QVERIFY(!st()->b(keys::misc::AssociateTorrent));
        QVERIFY(st()->b(keys::misc::AssociateMagnet));
        QVERIFY(st()->b(keys::misc::StartWithOs));
        QCOMPARE(st()->s(keys::identity::Connection), QStringLiteral("FTTB"));
        // the Settings pages show the same
        SettingsDialog d(m_app.core, m_app.mw);
        QVERIFY(!d.findChild<QCheckBox *>(QStringLiteral("set_gnutella_enabled"))->isChecked());
        QVERIFY(!d.findChild<QCheckBox *>(QStringLiteral("set_misc_associateTorrent"))->isChecked());
        d.close();
        for (const char *k : {"gnutella/enabled", "rats/enabled", "misc/associateTorrent", "misc/startWithOs", "network/connectOnStart",
                              "identity/connection"})
            st()->resetToDefaults(QString::fromLatin1(k));
    }

    // identity/primaryConnection: what it changes (the title's status and the Host button)
    void primaryConnection()
    {
        st()->setValue(keys::identity::PrimaryConnection, true);
        QTRY_VERIFY(m_app.mw->titleText().contains(QStringLiteral("Online P=")));
        m_app.mw->showPage(QStringLiteral("chat"));
        chat()->showChannelList();
        auto *host = chat()->channelList()->findChild<QWidget *>(QStringLiteral("chatHost"));
        QVERIFY(host);
        QVERIFY(host->isVisibleTo(chat()->channelList()));
        st()->setValue(keys::identity::PrimaryConnection, false);
        QTRY_VERIFY(!m_app.mw->titleText().contains(QStringLiteral("Online P=")));
        QVERIFY(m_app.mw->titleText().contains(QStringLiteral("Online ")));
        QVERIFY(!host->isVisibleTo(chat()->channelList()));
        st()->resetToDefaults(QStringLiteral("identity/primaryConnection"));
    }

    // misc/associateTorrent, misc/checkAssociations (the OS seam: a fake registry)
    void linkAndFileAssociations()
    {
        MockOptions mo;
        mo.frozen = true;
        std::unique_ptr<Core> core(createMockCoreWithOptions(QString(), mo));
        Settings *s = core->settings();
        {
            std::unique_ptr<FakeOsIntegration> os(static_cast<FakeOsIntegration *>(OsIntegration::createFake(nullptr)));
            s->setValue(keys::misc::AssociateTorrent, false);
            os->attach(s);
            QVERIFY(os->isHandler(OsIntegration::Assoc::Magnet));
            QVERIFY(!os->isHandler(OsIntegration::Assoc::Torrent));   // .torrent files left alone
            s->setValue(keys::misc::AssociateTorrent, true);
            QVERIFY(os->isHandler(OsIntegration::Assoc::Torrent));    // at once
        }
        // another program took magnet: links; the check at start takes them back only when ticked
        for (const bool check : {false, true}) {
            std::unique_ptr<FakeOsIntegration> os(static_cast<FakeOsIntegration *>(OsIntegration::createFake(nullptr)));
            os->setHandler(OsIntegration::Assoc::Magnet, false);
            s->setValue(keys::misc::CheckAssociations, check);
            os->attach(s);
            QCOMPARE(os->isHandler(OsIntegration::Assoc::Magnet), check);
        }
    }

    // MX_SET1_SHOTS=<dir>: every Settings page (100 %, the theme set) and the Networks page as PNG
    void settingsShots()
    {
        const QString dir = qEnvironmentVariable("MX_SET1_SHOTS");
        if (dir.isEmpty())
            QSKIP("MX_SET1_SHOTS not set");
        QDir().mkpath(dir);
        SettingsDialog d(m_app.core, m_app.mw);
        d.show();
        QVERIFY(QTest::qWaitForWindowExposed(&d));
        int n = 0;
        for (const QString &id : d.pageIds()) {
            QVERIFY(d.showPage(id));
            QTest::qWait(20);
            QVERIFY(d.grab().save(dir + QStringLiteral("/settings_") + QString(id).replace(QLatin1Char('/'), QLatin1Char('_')) + QStringLiteral(".png")));
            ++n;
        }
        d.close();
        m_app.mw->showPage(QStringLiteral("networks"));
        QTest::qWait(50);
        QVERIFY(m_app.mw->grab().save(dir + QStringLiteral("/networks_page.png")));
        QVERIFY(n > 40);
    }

    // ------------------------------------------------------------------ start of the program
    // ui/restoreLastPage, ui/smoothFonts (read at start)
    void startPageAndFonts()
    {
        const QString profile = m_dir.filePath(QStringLiteral("profile"));
        m_app.mw->showPage(QStringLiteral("search"));
        st()->setValue(keys::ui::RestoreLastPage, true);
        m_app.stop();
        m_app.start(profile);
        MainWindow::s_flashHook = [this] { ++m_flashes; };
        QCOMPARE(m_app.mw->currentPageId(), QStringLiteral("search"));
        st()->setValue(keys::ui::RestoreLastPage, false);
        m_app.stop();
        m_app.start(profile);
        MainWindow::s_flashHook = [this] { ++m_flashes; };
        QCOMPARE(m_app.mw->currentPageId(), QStringLiteral("networks"));
        // smooth font edges: read when the interface size is applied (at start)
        for (const char *v : {"on", "off"}) {
            st()->setValue(keys::ui::SmoothFonts, QString::fromLatin1(v));
            applyUiScale(st());
            QCOMPARE(smoothFonts(), qstrcmp(v, "on") == 0);
            const QFont f = classicFont(11);
            QCOMPARE(bool(f.styleStrategy() & QFont::NoAntialias), qstrcmp(v, "off") == 0);
        }
        st()->setValue(keys::ui::SmoothFonts, QStringLiteral("auto"));
        applyUiScale(st());
        QCOMPARE(smoothFonts(), uiScale() > 1.0);   // Automatic: on above 100 %
        st()->resetToDefaults(QStringLiteral("ui/restoreLastPage"));
    }
};

QTEST_MAIN(TstSet1)
#include "tst_set1.moc"

// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: MainWindow's secondary-window entry points (worker ui-b): Ignored Users, Browse,
// Whois, private messages and "Chat" with a user. Kept out of main_window.cpp so the two UI
// workers do not edit the same file. Every window is a top-level window owned by the main window
// and deleted when closed; one window per user (an open one is raised).
#include "ui/main_window.h"

#include "mx/services.h"
#include "ui/dialogs/browse_window.h"
#include "ui/dialogs/user_windows.h"
#include "ui/pages/chat_page.h"
#include "ui/script_driver.h"

#include <QtCore/QElapsedTimer>
#include <QtWidgets/QApplication>
#include "ui/kit/scale.h"
#include "ui/kit/status_bar.h"
#include "ui/models/browse_model.h"
#include "ui/pages/chat_view.h"

#include <functional>
#include <memory>

namespace mx::ui {

namespace {
template <class W>
W *findUserWindow(const QWidget *owner, const UserRef &u)
{
    const QList<W *> all = owner->findChildren<W *>(QString(), Qt::FindDirectChildrenOnly);
    for (W *w : all)
        if (w->property("userKey").toString() == u.key())
            return w;
    return nullptr;
}

void bringUp(QWidget *w)
{
    w->show();
    w->raise();
    w->activateWindow();
}

UserRef scriptUser(const QStringList &args)
{
    // "<name> [network]": the network by name ("eD2k", "Gnutella"...), default the OctoMX network
    UserRef u;
    u.net = Net::Overlay;
    QStringList a = args;
    if (a.size() > 1) {
        const Net n = netFromName(a.last());
        if (n != Net::Unknown) {
            u.net = n;
            a.removeLast();
        }
    }
    u.name = a.join(QLatin1Char(' '));
    u.id = u.name;
    return u;
}

// script verbs of the ui-b windows (see src/ui/README.md):
//   open-ignored | open-browse <user> [net] | open-whois <user> [net] | open-pm <user> [net] |
//   open-chat <user> | pm-say <user> <text> | close-window <objectName>
bool uibVerbs(MainWindow *mw, const QString &verb, const QString &rest, const QStringList &args, QString *error)
{
    if (verb == QLatin1String("open-ignored")) {
        mw->openIgnored();
        return true;
    }
    if (verb == QLatin1String("open-browse") || verb == QLatin1String("open-whois") || verb == QLatin1String("open-pm") ||
        verb == QLatin1String("open-chat")) {
        const UserRef u = scriptUser(args);
        if (!u.isValid()) {
            *error = QStringLiteral("usage: %1 <user> [network]").arg(verb);
            return true;
        }
        if (verb == QLatin1String("open-browse"))
            mw->openBrowse(u);
        else if (verb == QLatin1String("open-whois"))
            mw->openWhois(u);
        else if (verb == QLatin1String("open-pm"))
            mw->openPrivateMessage(u);
        else
            mw->openChat(u);
        return true;
    }
    if (verb == QLatin1String("pm-say")) {
        // [ui-g] an open message window whose user name starts with the text (real overlay users have a
        // node id, not their name, as id), else a window for that name
        PrivateMessageWindow *w = nullptr;
        for (QWidget *tl : QApplication::topLevelWidgets())
            if (auto *pm = qobject_cast<PrivateMessageWindow *>(tl); pm && pm->isVisible() && pm->user().name.startsWith(args.value(0)))
                w = pm;
        UserRef u;
        u.net = Net::Overlay;
        u.name = u.id = args.value(0);
        if (!w)
            w = PrivateChatHub::instance(mw)->open(u, true);
        if (!w) {
            *error = QStringLiteral("usage: pm-say <user> <text>");
            return true;
        }
        w->submit(rest.section(QLatin1Char(' '), 1));
        return true;
    }
    if (verb == QLatin1String("expect-net")) {
        // [ovl-2] expect-net <label> <kind> <user> <seconds> [text]: wait until it holds, then print
        // "CHECK PASS <label>" (on timeout the driver prints "... CHECK FAIL <label>: ..."). <user> is a
        // name prefix. Kinds: pm (an open message window shows text), pm-status (its delivery state reads
        // text: Delivered / Not delivered / Refused / Sent), no-pm (for <seconds> no window shows text:
        // the check passes at the end), browse (a browse window lists >= text files), browse-fail (its
        // status says text), whois (a whois window answered with text in a line), online / offline (the
        // Hotlist entry), transfer (a source row of a download from the user reads text), room (<user> is
        // a room name prefix: its view shows text)
        const QString label = args.value(0), kind = args.value(1), who = args.value(2);
        const int secs = args.value(3).toInt();
        const QString text = rest.section(QLatin1Char(' '), 4);
        ScriptDriver *d = ScriptDriver::current();
        if (!d || label.isEmpty() || who.isEmpty() || secs <= 0) {
            *error = QStringLiteral("usage: expect-net <label> <kind> <user> <seconds> [text]");
            return true;
        }
        auto pmWindow = [who]() -> PrivateMessageWindow * {
            for (QWidget *tl : QApplication::topLevelWidgets())
                if (auto *pm = qobject_cast<PrivateMessageWindow *>(tl); pm && pm->isVisible() && pm->user().name.startsWith(who))
                    return pm;
            return nullptr;
        };
        auto clock = std::make_shared<QElapsedTimer>();
        clock->start();
        std::function<bool()> ready;
        QString what;
        if (kind == QLatin1String("pm")) {
            what = QStringLiteral("no message window for %1 shows '%2'").arg(who, text);
            ready = [pmWindow, text] { auto *w = pmWindow(); return w && w->output()->toPlainText().contains(text); };
        } else if (kind == QLatin1String("pm-status")) {
            what = QStringLiteral("the message window for %1 never read '%2'").arg(who, text);
            ready = [pmWindow, text] { auto *w = pmWindow(); return w && w->deliveryText().startsWith(text); };
        } else if (kind == QLatin1String("no-pm")) {
            what = QStringLiteral("a message window for %1 shows '%2'").arg(who, text);
            const int ms = secs * 1000;
            ready = [pmWindow, text, clock, ms] {
                auto *w = pmWindow();
                if (w && w->output()->toPlainText().contains(text)) return false;
                return clock->elapsed() >= ms - 400;
            };
        } else if (kind == QLatin1String("browse") || kind == QLatin1String("browse-fail")) {
            const bool fail = kind == QLatin1String("browse-fail");
            what = fail ? QStringLiteral("the browse of %1 did not fail with '%2'").arg(who, text)
                        : QStringLiteral("the browse of %1 listed fewer than %2 files").arg(who, text);
            ready = [mw, who, text, fail] {
                for (BrowseWindow *b : mw->findChildren<BrowseWindow *>(QString(), Qt::FindDirectChildrenOnly)) {
                    if (!b->user().name.startsWith(who) || b->isLoading()) continue;
                    if (fail) {
                        if (b->statusBar()->text().contains(text)) return true;
                        continue;
                    }
                    // files = leaves of the folder tree
                    std::function<int(const QModelIndex &)> leaves = [&](const QModelIndex &p) {
                        const int n = b->model()->rowCount(p);
                        if (n == 0) return p.isValid() ? 1 : 0;
                        int c = 0;
                        for (int i = 0; i < n; ++i) c += leaves(b->model()->index(i, 0, p));
                        return c;
                    };
                    if (leaves(QModelIndex()) >= text.toInt()) return true;
                }
                return false;
            };
        } else if (kind == QLatin1String("whois")) {
            what = QStringLiteral("no whois answer from %1 with '%2'").arg(who, text);
            ready = [mw, who, text] {
                for (WhoisWindow *w : mw->findChildren<WhoisWindow *>(QString(), Qt::FindDirectChildrenOnly)) {
                    if (!w->user().name.startsWith(who) || !w->hasResult()) continue;
                    for (const auto &l : w->view()->lines())
                        if ((l.first + QLatin1Char(' ') + l.second).contains(text)) return true;
                }
                return false;
            };
        } else if (kind == QLatin1String("online") || kind == QLatin1String("offline")) {
            const bool want = kind == QLatin1String("online");
            what = QStringLiteral("%1 is not %2 in the Hotlist").arg(who, kind);
            ready = [mw, who, want] {
                for (const HotlistEntry &e : mw->core()->users()->hotlist())
                    if (e.user.name.startsWith(who)) return e.online == want;
                return false;
            };
        } else if (kind == QLatin1String("room")) {
            what = QStringLiteral("room %1 never showed '%2'").arg(who, text);
            ready = [mw, who, text] {
                for (ChatRoomView *v : mw->findChildren<ChatRoomView *>())
                    if (v->roomId().startsWith(who.toLower()) && v->output()->toPlainText().contains(text)) return true;
                return false;
            };
        } else if (kind == QLatin1String("own-name")) {
            // <user> is a room name prefix: our own row there (recognised by id) has exactly the name the
            // Networks page shows
            what = QStringLiteral("the own row in %1 does not read the screen name").arg(who);
            ready = [mw, who] {
                for (const ChatRoomInfo &r : mw->core()->chat()->joinedRooms()) {
                    if (!r.id.startsWith(who.toLower())) continue;
                    for (const ChatUser &u : mw->core()->chat()->roomUsers(r.id))
                        if (mw->core()->users()->isSelf(u.user)) return u.user.name == mw->core()->network()->screenName();
                }
                return false;
            };
        } else if (kind == QLatin1String("transfer")) {
            what = QStringLiteral("no download source from %1 reads '%2'").arg(who, text);
            ready = [mw, who, text] {
                for (const TransferInfo &x : mw->core()->transfers()->downloads())
                    for (const TransferSource &s : x.sources)
                        if (s.user.name.startsWith(who) && s.statusText.contains(text)) return true;
                return false;
            };
        } else {
            *error = QStringLiteral("expect-net: unknown kind %1").arg(kind);
            return true;
        }
        // a failed check is reported and the script goes on (the driver would stop at a failed hold)
        const qint64 limitMs = qint64(secs) * 1000;
        const QString failText = QStringLiteral("CHECK FAIL %1: %2 after %3 s").arg(label, what).arg(secs);
        d->holdUntil([ready, label, clock, limitMs, failText] {
            if (ready()) {
                qInfo("CHECK PASS %s (%lld ms)", qPrintable(label), clock->elapsed());
                return true;
            }
            if (clock->elapsed() < limitMs) return false;
            qWarning("%s", qPrintable(failText));
            return true;
        }, int(limitMs) + 5000, failText);
        return true;
    }
    if (verb == QLatin1String("close-window")) {
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w != mw && w->isVisible() && w->objectName() == rest) {
                w->close();
                return true;
            }
        *error = QStringLiteral("no open window named %1").arg(rest);
        return true;
    }
    return false;
}
} // namespace

void registerUiBScriptVerbs()
{
    static bool done = false;
    if (!done) {
        done = true;
        ScriptDriver::addCommandHandler(&uibVerbs);
    }
}

void MainWindow::openIgnored()
{
    // Ignored Users tool window (UI_SPEC §5.1), a single instance
    auto *w = findChild<IgnoredWindow *>(QString(), Qt::FindDirectChildrenOnly);
    if (!w) {
        w = new IgnoredWindow(m_core, this, this);
        w->move(mapToGlobal(QPoint(qMax(0, width() - S(380)), S(60))));
    }
    bringUp(w);
}

void MainWindow::openBrowse(const mx::UserRef &u)
{
    // Browse window (UI_SPEC §5.2)
    if (!u.isValid())
        return;
    if (auto *w = findUserWindow<BrowseWindow>(this, u)) {
        // [ovl-2] a window that shows a failed (or empty) browse asks again: the reason may be gone
        // (added to the Hotlist, the user came back, a link is up now)
        if (!w->isLoading() && w->model()->rowCount() == 0)
            w->refresh();
        bringUp(w);
        return;
    }
    auto *w = new BrowseWindow(m_core, this, u, this);
    bringUp(w);
}

void MainWindow::openWhois(const mx::UserRef &u)
{
    // Whois window (UI_SPEC §5.3)
    if (!u.isValid())
        return;
    if (auto *w = findUserWindow<WhoisWindow>(this, u)) {
        w->request();
        bringUp(w);
        return;
    }
    auto *w = new WhoisWindow(m_core, this, u, this);
    bringUp(w);
}

void MainWindow::openPrivateMessage(const mx::UserRef &u)
{
    // private message window (UI_SPEC §5.4)
    PrivateChatHub::instance(this)->open(u, true);
}

void MainWindow::openChat(const mx::UserRef &u)
{
    // a room we share with the user, else a private conversation with them
    if (auto *chat = qobject_cast<ChatPage *>(page(QStringLiteral("chat")))) {
        for (ChatRoomView *v : chat->rooms())
            if (v->userModel() && v->isJoined()) {
                bool there = false;
                for (const ChatUser &cu : m_core->chat()->roomUsers(v->roomId()))
                    if (cu.user.key() == u.key())
                        there = true;
                if (there) {
                    showPage(QStringLiteral("chat"));
                    chat->selectSubTab(v->room().name);
                    return;
                }
            }
    }
    openPrivateMessage(u);
}

} // namespace mx::ui

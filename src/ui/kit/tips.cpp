// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/kit/tips.h"

#include "ui/kit/scale.h"

#include <QtCore/QCoreApplication>
#include <QtGui/QHelpEvent>
#include <QtGui/QTextDocument>
#include <QtWidgets/QToolTip>
#include <QtWidgets/QWidget>
#include <QtGui/QAction>
#include <QtGui/QStatusTipEvent>
#include <QtWidgets/QMenu>

namespace mx::ui::tips {

namespace {
const char *const kCtx = "MenuTips";

struct Entry { const char *text; const char *tip; };
// read from the code behind each entry (Transfers, Search, Shared Files, Hotlist, Chat, Networks)
const Entry kEntries[] = {
    // transfers
    {"Paste Link...", QT_TRANSLATE_NOOP("MenuTips", "Download from a magnet, ed2k or http link copied to the clipboard")},
    {"Open Torrent File...", QT_TRANSLATE_NOOP("MenuTips", "Choose a .torrent file on this computer and download what it describes")},
    {"Pause All", QT_TRANSLATE_NOOP("MenuTips", "Stop every download for now; nothing is lost and Resume All continues them")},
    {"Resume All", QT_TRANSLATE_NOOP("MenuTips", "Continue every paused download")},
    {"Retry", QT_TRANSLATE_NOOP("MenuTips", "Connect to the known sources of the selected downloads again now, without waiting for the next attempt")},
    {"Enter Remote Queue", QT_TRANSLATE_NOOP("MenuTips", "Connect to the user now and take a place in their upload queue")},
    {"Enter Queue", QT_TRANSLATE_NOOP("MenuTips", "Wait in the users' queues for this download even when the place is beyond the Auto Enter Queue limit")},
    {"Bandwidth Priority", QT_TRANSLATE_NOOP("MenuTips", "Give this transfer a larger or smaller share of the bandwidth than the others")},
    {"Priority", QT_TRANSLATE_NOOP("MenuTips", "Which downloads get sources and bandwidth first when there is not enough for all")},
    {"Delete File", QT_TRANSLATE_NOOP("MenuTips", "Cancel the download and delete its file (the partial or the finished file) from the disk; you are asked first")},
    {"Trim 10K from end of file", QT_TRANSLATE_NOOP("MenuTips", "In the original WinMX this cut the last 10 KB of a partial file that ended with damaged data, so a resume fetched them again. "
                                                               "Not needed here: every network checks the parts by their hashes. Not available.")},
    {"Find Sources Now", QT_TRANSLATE_NOOP("MenuTips", "Search all networks again for more users who have the selected downloads")},
    {"Why Is This Slow?...", QT_TRANSLATE_NOOP("MenuTips", "Open the Connection Test on this download: ports, sources and what limits its speed, in plain words")},
    {"Search For Alternates...", QT_TRANSLATE_NOOP("MenuTips", "Search for this exact file by its hash, to find more sources even under other names")},
    {"Search For Similar...", QT_TRANSLATE_NOOP("MenuTips", "Start a new search with the words of this file's name")},
    {"Select Files...", QT_TRANSLATE_NOOP("MenuTips", "Choose which files of this torrent or folder to download")},
    {"Change Destination...", QT_TRANSLATE_NOOP("MenuTips", "Choose another folder where the finished file is saved")},
    {"Start Transfer", QT_TRANSLATE_NOOP("MenuTips", "Let this queued user start downloading at once, before the others in the queue (where the network allows it)")},
    {"Enqueue", QT_TRANSLATE_NOOP("MenuTips", "Here the same as Open: the finished file opens with the program your system uses for it")},
    {"Play", QT_TRANSLATE_NOOP("MenuTips", "Open the finished file with the program your system uses for it")},
    {"Open", QT_TRANSLATE_NOOP("MenuTips", "Open the finished file with the program your system uses for it")},
    {"Open File", QT_TRANSLATE_NOOP("MenuTips", "Open the file with the program your system uses for it (double-click does the same)")},
    {"Open Containing Folder", QT_TRANSLATE_NOOP("MenuTips", "Show the file in its folder in the file manager")},
    {"Open Folder", QT_TRANSLATE_NOOP("MenuTips", "Show this folder in the file manager")},
    {"Copy Magnet Link", QT_TRANSLATE_NOOP("MenuTips", "Copy a magnet link of this file, to paste in a message or another program")},
    {"Copy ed2k Link", QT_TRANSLATE_NOOP("MenuTips", "Copy an ed2k link of this file, to paste in a message or another program")},
    {"Copy Name", QT_TRANSLATE_NOOP("MenuTips", "Copy the name of this row to the clipboard")},
    // users (every list with users)
    {"Browse", QT_TRANSLATE_NOOP("MenuTips", "Show the files this user shares (OctoMX and eD2k users; other networks do not allow it)")},
    {"Send Message", QT_TRANSLATE_NOOP("MenuTips", "Open a private message window with this user")},
    {"Whois", QT_TRANSLATE_NOOP("MenuTips", "Ask this user for their details: connection, files shared, client")},
    {"Add To Hotlist", QT_TRANSLATE_NOOP("MenuTips", "Keep this user in your Hotlist, to see when they are online and reach them later")},
    {"Ping", QT_TRANSLATE_NOOP("MenuTips", "Measure how fast this user answers; no answer usually means offline")},
    {"Ignore", QT_TRANSLATE_NOOP("MenuTips", "Hide this user's chat lines and messages from now on (Ignored button lists them)")},
    {"Chat", QT_TRANSLATE_NOOP("MenuTips", "Go to a chat room you share with this user, else open a private conversation")},
    {"Perm add to list", QT_TRANSLATE_NOOP("MenuTips", "Keep this temporary entry (someone who messaged you) on the Hotlist for good")},
    {"Rename on list", QT_TRANSLATE_NOOP("MenuTips", "Show this user under a name of your choice in your Hotlist only")},
    // search
    {"Download", QT_TRANSLATE_NOOP("MenuTips", "Start downloading the selected results")},
    {"Manually Download", QT_TRANSLATE_NOOP("MenuTips", "Download this file into a folder or under a name you choose")},
    {"Clear Search History", QT_TRANSLATE_NOOP("MenuTips", "Forget the words of the recent searches")},
    // shared files
    {"Share Folder...", QT_TRANSLATE_NOOP("MenuTips", "Choose a folder whose files the other users may download")},
    {"Un-share Folder", QT_TRANSLATE_NOOP("MenuTips", "Stop sharing this folder; the files stay on your disk")},
    {"Refresh", QT_TRANSLATE_NOOP("MenuTips", "Look for new, changed and removed files in the shared folders now")},
    // chat
    {"Join Channel", QT_TRANSLATE_NOOP("MenuTips", "Enter this chat room")},
    {"Part Channel", QT_TRANSLATE_NOOP("MenuTips", "Leave this chat room")},
    {"Add To Favorite Channels", QT_TRANSLATE_NOOP("MenuTips", "Keep this room in the Favorites menu of the channel list")},
    {"Add To Favorites", QT_TRANSLATE_NOOP("MenuTips", "Keep this room in the Favorites menu of the channel list")},
    {"Add Selected Channel", QT_TRANSLATE_NOOP("MenuTips", "Keep the room selected in the list in the Favorites menu")},
    {"Copy Channel Name", QT_TRANSLATE_NOOP("MenuTips", "Copy the room's full name, to give it to a friend")},
    {"Clear Window", QT_TRANSLATE_NOOP("MenuTips", "Remove the text shown in this room's window (for you only)")},
    {"Time Stamps", QT_TRANSLATE_NOOP("MenuTips", "Show the time in front of every chat line")},
    {"Backscroll Limit", QT_TRANSLATE_NOOP("MenuTips", "How many old lines the room window keeps")},
    {"Hold Window Open After Part", QT_TRANSLATE_NOOP("MenuTips", "Keep the room's text open after you leave the room")},
    {"Kick", QT_TRANSLATE_NOOP("MenuTips", "Room operators: send this user out of the room")},
    {"Ban", QT_TRANSLATE_NOOP("MenuTips", "Room operators: send this user out of the room and keep them out")},
    {"Kick/Ban", QT_TRANSLATE_NOOP("MenuTips", "Room operators: send this user out of the room and keep them out")},
    {"Set Level...", QT_TRANSLATE_NOOP("MenuTips", "Room operators: give this user another rank in the room")},
    // networks
    {"Connect", QT_TRANSLATE_NOOP("MenuTips", "Connect to this server or network now")},
    {"Disconnect", QT_TRANSLATE_NOOP("MenuTips", "Close the connection to this server or network")},
    {"View Server Messages", QT_TRANSLATE_NOOP("MenuTips", "Show what this server said when you connected (rules, news)")},
    // hotlist options
    {"Add User...", QT_TRANSLATE_NOOP("MenuTips", "Add an OctoMX user to the Hotlist by screen name")},
    {"Auto Responder", QT_TRANSLATE_NOOP("MenuTips", "An answer sent by itself to private messages while you are away")},
    {"Privacy Level...", QT_TRANSLATE_NOOP("MenuTips", "Choose who may message you, browse you and download from you (for example Hotlist users only)")},
    {"Confirm Remove", QT_TRANSLATE_NOOP("MenuTips", "Ask before an entry is removed from the Hotlist")},
    {"Browse on Double-Click", QT_TRANSLATE_NOOP("MenuTips", "A double-click on a Hotlist user shows their files instead of opening a message window")},
};

QString lookup(const QString &key)
{
    for (const Entry &e : kEntries)
        if (key == QLatin1String(e.text))
            return QCoreApplication::translate(kCtx, e.tip);
    return QString();
}
} // namespace

QString forMenuEntry(const QString &text)
{
    QString key = text;
    key.remove(QLatin1Char('&'));
    key = key.section(QLatin1Char('\t'), 0, 0).trimmed();
    return lookup(key);
}

void explainMenu(QMenu *menu, const QHash<QString, QString> &extra)
{
    if (!menu)
        return;
    menu->setToolTipsVisible(true);
    if (!menu->property("mxExplained").toBool()) {
        menu->setProperty("mxExplained", true);
        // the status bar of the window shows the explanation of the hovered entry
        QObject::connect(menu, &QMenu::hovered, menu, [menu](QAction *a) {
            QWidget *target = menu->parentWidget();
            if (!target || a->statusTip().isEmpty())
                return;
            QStatusTipEvent tip(a->statusTip());
            QCoreApplication::sendEvent(target, &tip);
        });
    }
    for (QAction *a : menu->actions()) {
        if (a->isSeparator())
            continue;
        QString key = a->text();
        key.remove(QLatin1Char('&'));
        // a tool tip of its own (QAction's default is the text itself)
        const bool own = !a->toolTip().isEmpty() && a->toolTip().remove(QLatin1Char('&')) != key;
        if (!own) {
            const QString tip = extra.contains(key) ? extra.value(key) : forMenuEntry(key);
            if (!tip.isEmpty()) {
                a->setToolTip(wrapped(tip));
                if (a->statusTip().isEmpty())
                    a->setStatusTip(tip);
            }
        } else {
            if (a->statusTip().isEmpty())
                a->setStatusTip(a->toolTip());
            if (!Qt::mightBeRichText(a->toolTip()))
                a->setToolTip(wrapped(a->toolTip()));
        }
        if (a->menu())
            explainMenu(a->menu(), extra);
    }
}

QString wrapped(const QString &text, int designWidth)
{
    if (text.isEmpty() || Qt::mightBeRichText(text))
        return text;
    const QFontMetrics fm(QToolTip::font());
    const int maxW = S(designWidth);
    QStringList out;
    for (const QString &para : text.split(QLatin1Char('\n'))) {
        QString line;
        for (const QString &word : para.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
            const QString next = line.isEmpty() ? word : line + QLatin1Char(' ') + word;
            if (!line.isEmpty() && fm.horizontalAdvance(next) > maxW) {
                out << line;
                line = word;
            } else {
                line = next;
            }
        }
        out << line;
    }
    return out.join(QLatin1Char('\n'));
}

namespace {
class ToolTipWrapper : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *o, QEvent *e) override
    {
        if (e->type() != QEvent::ToolTip || !o->isWidgetType())
            return false;
        auto *w = static_cast<QWidget *>(o);
        const QString tip = w->toolTip();
        if (tip.isEmpty() || Qt::mightBeRichText(tip))
            return false;   // the widget's own handling (lists show cell texts, rich text wraps itself)
        auto *he = static_cast<QHelpEvent *>(e);
        // below the widget: Qt puts the tip 16 px under the given point, so aim at the bottom edge
        const QPoint at(he->globalPos().x(), w->mapToGlobal(QPoint(0, w->height())).y() - 14);
        QToolTip::showText(at, wrapped(tip), w, w->rect());
        return true;
    }
};
} // namespace

void installToolTipWrapper(QObject *app)
{
    static QObject *installed = nullptr;
    if (!app || installed == app)
        return;
    installed = app;
    app->installEventFilter(new ToolTipWrapper(app));
}

} // namespace mx::ui::tips

// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/pages/chat_page.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimer>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtGui/QPainter>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMenu>
#include <QtWidgets/QScrollBar>
#include <QtGui/QActionGroup>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QStackedWidget>

#include "mx/services.h"
#include "mx/settings_keys.h"
#include "ui/dialogs/chat_warning_dialog.h"
#include "ui/dialogs/user_windows.h"
#include "ui/kit/tips.h"
#include "ui/kit/colors.h"
#include "ui/kit/draw.h"
#include "ui/kit/format.h"
#include "ui/kit/list_view.h"
#include "ui/kit/theme.h"
#include "ui/kit/widgets.h"
#include "ui/kit/scale.h"
#include "ui/main_window.h"
#include "ui/models/chat_models.h"
#include "ui/models/roles.h"
#include "ui/models/search_model.h"   // [ui-h] search::connectionText
#include "ui/pages/chat_view.h"

namespace mx::ui {

void registerUiBScriptVerbs();   // main_window_b.cpp

namespace {
bool s_warnedThisSession = false;

QString networkLabel(Net n)
{
    // our own overlay: "OctoMX Peer Network" (UI_SPEC §8.5; the original WinMX named its network after itself)
    return n == Net::Overlay || n == Net::Unknown ? QObject::tr("OctoMX Peer Network") : netName(n);
}

bool sameRoom(const ChatRoomInfo &r, const QString &idOrName)
{
    return r.id == idOrName || r.name.compare(idOrName, Qt::CaseInsensitive) == 0 ||
           r.name.startsWith(idOrName + QLatin1Char('_'), Qt::CaseInsensitive);
}

// the chat splitter handle: plain face, no decoration (3 px between output and user list)
class FaceSplitter : public QSplitter {
public:
    using QSplitter::QSplitter;

protected:
    QSplitterHandle *createHandle() override
    {
        struct H : QSplitterHandle {
            using QSplitterHandle::QSplitterHandle;
            void paintEvent(QPaintEvent *) override
            {
                QPainter p(this);
                p.fillRect(rect(), chrome::Face);
            }
        };
        return new H(orientation(), this);
    }
};

// "/topic <topic>" -> "/topic " (what choosing a Commands item pastes into the input line)
QString commandStem(const QString &item)
{
    int cut = int(item.size());
    for (QChar c : {QLatin1Char('<'), QLatin1Char('[')}) {
        const int i = int(item.indexOf(c));
        if (i >= 0)
            cut = qMin(cut, i);
    }
    QString s = item.left(cut);
    if (!s.endsWith(QLatin1Char(' ')) && cut < item.size())
        s += QLatin1Char(' ');
    return s;
}
} // namespace

// ================================================================================ ChannelListView

ChannelListView::ChannelListView(mx::Core *core, QWidget *parent) : QWidget(parent), m_core(core)
{
    setObjectName(QStringLiteral("channelListView"));
    auto mk = [this](const QString &t, const char *name) {
        auto *b = new PushButton(t, this);
        b->setObjectName(QString::fromLatin1(name));
        return b;
    };
    m_filter = mk(tr("Filter"), "chatFilter");
    m_filter->setCheckable(true);
    m_join = mk(tr("Join"), "chatJoin");
    m_host = mk(tr("Host"), "chatHost");
    m_close = mk(tr("Close"), "chatListClose");
    m_joinSel = mk(tr("Join Selected Channel"), "chatJoinSelected");
    m_refresh = mk(tr("Refresh List"), "chatRefreshList");
    m_favs = mk(tr("Favorites"), "chatFavorites");
    m_wFilter = new Well(m_filter, this);
    m_wJoin = new Well(m_join, this);
    m_wHost = new Well(m_host, this);
    m_wClose = new Well(m_close, this);
    m_wJoinSel = new Well(m_joinSel, this);
    m_wRefresh = new Well(m_refresh, this);
    m_wFavs = new Well(m_favs, this);

    m_edit = new QLineEdit(this);
    m_edit->setObjectName(QStringLiteral("chatFilterText"));
    // [ui-g] what each control does
    m_filter->setToolTip(tr("Show only the rooms whose name (and topic, if chosen in Settings) contains the text typed beside it"));
    m_join->setToolTip(tr("Enter the room whose name is typed in the box"));
    m_host->setToolTip(tr("Open a new room with the name typed in the box; you are its host while you stay in it"));
    m_close->setToolTip(tr("Close the channel list"));
    m_joinSel->setToolTip(tr("Enter the room selected in the list (double-click does the same)"));
    m_refresh->setToolTip(tr("Ask the network for the current list of rooms and their user counts"));
    m_favs->setToolTip(tr("Your favourite rooms: join one, or add the selected room"));
    m_edit->setToolTip(tr("A room name to join or host, and the filter text for the list (Ctrl+F comes here)"));
    m_edit->setTextMargins(-1, 0, 0, 0);
    // [post-1] the filter box is part of the black list: it follows the colour theme
    auto filterColors = [this] {
        QPalette pal = m_edit->palette();
        pal.setColor(QPalette::Base, color(keys::colors::ListBackground));
        pal.setColor(QPalette::Text, color(keys::colors::ListText));
        m_edit->setPalette(pal);
    };
    filterColors();
    connect(Colors::instance(), &Colors::changed, m_edit, filterColors);

    m_model = new ChannelModel(this);
    m_proxy = new ChannelProxy(this);
    m_proxy->setSourceModel(m_model);
    m_list = new MxListView(this);
    m_list->setFilterEnabled(true);   // [ui-e] Ctrl+F
    // [ui-g] the classic Filter well is the always visible filter of this list: Ctrl+F goes there
    m_list->setFindHandler([this] {
        m_edit->setFocus(Qt::ShortcutFocusReason);
        m_edit->selectAll();
        if (!m_filter->isChecked())
            m_filter->setChecked(true);
    });
    m_list->setObjectName(QStringLiteral("channelList"));
    m_list->setModel(m_proxy);
    m_list->setDefaultColumnWidths({315, 85, 400});
    m_list->setMinimumColumnTexts(1, {QStringLiteral("9999 / 9999")});   // [ui-h] Users never cut ("1500 / 2000")
    m_list->setStateKey(core->settings(), QStringLiteral("channels"));

    connect(m_filter, &QPushButton::toggled, this, &ChannelListView::applyFilter);
    connect(m_edit, &QLineEdit::textChanged, this, [this](const QString &t) {
        m_join->setEnabled(!t.trimmed().isEmpty());
        m_host->setEnabled(!t.trimmed().isEmpty());
        applyFilter();
    });
    connect(m_edit, &QLineEdit::returnPressed, m_join, &QPushButton::click);
    connect(m_join, &QPushButton::clicked, this, [this] {
        if (!m_edit->text().trimmed().isEmpty())
            emit joinRequested(m_edit->text().trimmed());
    });
    connect(m_host, &QPushButton::clicked, this, [this] {
        // hosting = joining a room that does not exist yet (ChatService creates it)
        if (!m_edit->text().trimmed().isEmpty())
            emit joinRequested(m_edit->text().trimmed());
    });
    connect(m_close, &QPushButton::clicked, this, &ChannelListView::closeRequested);
    connect(m_joinSel, &QPushButton::clicked, this, [this] {
        const QString n = selectedRoomName();
        if (!n.isEmpty())
            emit joinRequested(n);
    });
    connect(m_refresh, &QPushButton::clicked, this, [this] { m_core->chat()->refreshRoomList(m_net); });
    connect(m_favs, &QPushButton::clicked, this, &ChannelListView::showFavoritesMenu);
    connect(m_list, &MxListView::rowActivated, this, [this](const QModelIndex &vi) {
        const QModelIndex s = m_list->mapToSource(vi);
        if (s.isValid())
            emit joinRequested(m_model->room(s.row()).name);
    });
    connect(m_list, &MxListView::contextMenuAt, this, &ChannelListView::showRowMenu);
    connect(m_list->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [this] { m_joinSel->setEnabled(!selectedRoomName().isEmpty()); });
    connect(core->chat(), &ChatService::roomListChanged, this, &ChannelListView::reload);
    connect(core->settings(), &Settings::changed, this, [this](const QString &k) {
        if (k == QLatin1String(keys::identity::PrimaryConnection))
            updateHostVisibility();
        else if (k == QLatin1String(keys::chat::TopicInFilter))
            applyFilter();
    });
    m_join->setEnabled(false);
    m_host->setEnabled(false);
    m_joinSel->setEnabled(false);
    updateHostVisibility();
    reload();
}

void ChannelListView::updateHostVisibility()
{
    // "Host" is only offered with a primary connection (UI_SPEC §3.3.1)
    m_wHost->setVisible(m_core->settings()->b(keys::identity::PrimaryConnection));
    resizeEvent(nullptr);
}

void ChannelListView::setNetwork(Net n)
{
    if (m_net == n)
        return;
    m_net = n;
    reload();
}

void ChannelListView::reload()
{
    const QString sel = selectedRoomName();
    // roomList() holds every network's rooms (ChatRoomInfo::net): show the chosen network's
    QList<ChatRoomInfo> rooms;
    for (const ChatRoomInfo &r : m_core->chat()->roomList())
        if (r.net == m_net || (m_net == Net::Overlay && r.net == Net::Unknown))
            rooms << r;
    m_model->setRooms(rooms);
    if (!sel.isEmpty()) {
        for (int i = 0; i < m_model->roomCount(); ++i)
            if (m_model->room(i).name == sel) {
                const QModelIndex vi = m_list->mapFromSource(m_model->index(i, 0));
                if (vi.isValid())
                    m_list->selectionModel()->setCurrentIndex(vi, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
                break;
            }
    }
    emit statusChanged();
}

void ChannelListView::applyFilter()
{
    m_proxy->setFilter(m_edit->text().trimmed(), m_filter->isChecked(), m_core->settings()->b(keys::chat::TopicInFilter));
    emit statusChanged();
}

QString ChannelListView::selectedRoomName() const
{
    const QModelIndex s = m_list->currentSourceIndex();
    if (!s.isValid() || !m_list->selectionModel()->isRowSelected(m_list->currentIndex().row(), m_list->currentIndex().parent()))
        return {};
    return m_model->room(s.row()).name;
}

QStringList ChannelListView::statusFields() const
{
    const int n = m_proxy->rowCount();
    return {n == 1 ? tr("1 channel") : tr("%1 channels").arg(n)};
}

void ChannelListView::showFavoritesMenu()
{
    auto *menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("chatFavoritesMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    const QString sel = selectedRoomName();
    menu->addAction(tr("Add Selected Channel"), this, [this, sel] { m_core->chat()->setFavouriteRoom(sel, true); })
        ->setEnabled(!sel.isEmpty());
    const QStringList favs = m_core->chat()->favouriteRooms();
    if (!favs.isEmpty()) {
        menu->addSeparator();
        for (const QString &f : favs)
            menu->addAction(f, this, [this, f] { emit joinRequested(f); });
    }
    menu->addSeparator();
    QMenu *rm = menu->addMenu(tr("Remove"));
    for (const QString &f : favs)
        rm->addAction(f, this, [this, f] { m_core->chat()->setFavouriteRoom(f, false); });
    rm->setEnabled(!favs.isEmpty());
    tips::explainMenu(menu);   // [ui-g] tool tips + status bar text of the entries
    menu->popup(m_favs->mapToGlobal(QPoint(0, m_favs->height())));
}

void ChannelListView::showRowMenu(const QModelIndex &vi, const QPoint &globalPos)
{
    const QModelIndex s = m_list->mapToSource(vi);
    if (!s.isValid())
        return;
    const QString name = m_model->room(s.row()).name;
    auto *menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("channelMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    QAction *join = menu->addAction(tr("Join Channel"), this, [this, name] { emit joinRequested(name); });
    menu->setDefaultAction(join);
    menu->addSeparator();
    menu->addAction(tr("Add To Favorites"), this, [this, name] { m_core->chat()->setFavouriteRoom(name, true); });
    menu->addAction(tr("Copy Channel Name"), this, [name] { QGuiApplication::clipboard()->setText(name); });
    tips::explainMenu(menu);   // [ui-g] tool tips + status bar text of the entries
    menu->popup(globalPos);
}

void ChannelListView::resizeEvent(QResizeEvent *)
{
    // geometry measured on web/chat_channel_list_354b3.png (page coordinates)
    const int w = width();
    const int rowD = S(29), rowE = S(51), h = S(22), bw = S(60), one = S(1);
    const bool host = !m_wHost->isHidden();
    m_wFilter->setGeometry(one, rowD, bw, h);
    m_wClose->setGeometry(w - S(63), rowD, bw, h);
    int right = w - S(63);
    if (host) {
        m_wHost->setGeometry(right - bw, rowD, bw, h);
        right -= bw;
    }
    m_wJoin->setGeometry(right - bw, rowD, bw, h);
    right -= bw;
    m_edit->setGeometry(S(61), rowD, qMax(S(20), right - S(61)), h);
    const int total = w - S(4);
    const int w1 = total / 2, w2 = total / 4, w3 = total - w1 - w2;
    m_wJoinSel->setGeometry(one, rowE, w1, h);
    m_wRefresh->setGeometry(one + w1, rowE, w2, h);
    m_wFavs->setGeometry(one + w1 + w2, rowE, w3, h);
    m_list->setGeometry(one, S(73), total, qMax(0, height() - S(73)));
}

// =================================================================================== ChatRoomView

ChatRoomView::ChatRoomView(mx::Core *core, MainWindow *mw, const ChatRoomInfo &room, QWidget *parent)
    : QWidget(parent), m_core(core), m_main(mw), m_room(room)
{
    setObjectName(QStringLiteral("chatRoomView"));
    m_split = new FaceSplitter(Qt::Horizontal, this);
    m_split->setObjectName(QStringLiteral("chatRoomSplitter"));
    m_split->setHandleWidth(S(3));
    m_split->setChildrenCollapsible(false);
    m_out = new ChatOutput(m_split);
    m_out->setObjectName(QStringLiteral("chatOutput"));
    m_out->setFont(chatFont(core->settings()));
    m_userModel = new ChatUserModel(this);
    m_userProxy = new ChatUserProxy(this);
    m_userProxy->setSourceModel(m_userModel);
    m_users = new MxListView(m_split);
    m_users->setObjectName(QStringLiteral("chatUserList"));
    m_users->setModel(m_userProxy);
    m_users->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_users->setDefaultColumnWidths({136, 60, 45});
    // [ui-h] colors/chatUserListBackground, and columns that never cut what they show: the Speed column
    // fits the longest connection label ("Unknown", "10 Gbit+" ...) at the current font and scale, Files
    // five digits, User a short name; the list (and the splitter) keeps the width they need
    m_users->setBackgroundKey(QString(keys::colors::ChatUserListBackground));
    {
        QStringList speeds;
        for (const QString &t : connectionTypes())
            speeds << search::connectionText(t);
        m_users->setMinimumColumnTexts(ChatUserModel::Speed, speeds);
        m_users->setMinimumColumnTexts(ChatUserModel::Files, {QStringLiteral("99999")});
        m_users->setMinimumColumnTexts(ChatUserModel::User, {QStringLiteral("@Guest_0000")});
        m_users->setMinimumWidth(m_users->minimumListWidth());
        connect(Fonts::instance(), &Fonts::changed, m_users, [this] { m_users->setMinimumWidth(m_users->minimumListWidth()); });
    }
    m_users->setStateKey(core->settings(), QStringLiteral("chatUsers"));
    // default order: ops, voiced, then by name, without a sort arrow (the header has none)
    if (m_users->header()->sortIndicatorSection() < 0)
        m_userProxy->sort(ChatUserModel::User, Qt::AscendingOrder);
    m_split->addWidget(m_out);
    m_split->addWidget(m_users);
    m_split->setStretchFactor(0, 1);
    m_split->setStretchFactor(1, 0);

    m_input = new ChatInput(this);
    m_input->setObjectName(QStringLiteral("chatInput"));
    m_input->setToolTip(tr("Type a message to the room and press Enter. Commands start with / (see Commands)."));
    m_commands = new PushButton(tr("Commands"), this);
    m_commands->setObjectName(QStringLiteral("chatCommands"));
    m_exit = new PushButton(tr("Exit Channel"), this);
    m_exit->setObjectName(QStringLiteral("chatExit"));
    m_commands->setToolTip(tr("Chat commands you can send in this room (/me, /topic ...), sorted by what they do"));
    m_exit->setToolTip(tr("Leave this room and close its tab"));

    connect(m_input, &ChatInput::submitted, this, &ChatRoomView::submit);
    connect(m_commands, &QPushButton::clicked, this, &ChatRoomView::showCommandsMenu);
    connect(m_exit, &QPushButton::clicked, this, [this] {
        if (m_joined)
            m_core->chat()->leaveRoom(m_room.id);
        emit closeRequested();
    });
    connect(m_users, &MxListView::rowActivated, this, [this](const QModelIndex &vi) {
        const UserRef u = userAt(vi);
        if (u.isValid())
            m_main->openPrivateMessage(u);
    });
    connect(m_users, &MxListView::contextMenuAt, this, &ChatRoomView::showUserMenu);

    m_userTimer = new QTimer(this);
    m_userTimer->setSingleShot(true);
    m_userTimer->setInterval(150);
    connect(m_userTimer, &QTimer::timeout, this, &ChatRoomView::reloadUsers);

    Settings *st = core->settings();
    connect(st, &Settings::changed, this, [this](const QString &k) {
        if (k == QLatin1String(keys::chat::BacklogLines) || k == QLatin1String(keys::chat::AllowLargeBackscroll))
            applyBackscroll();
        else if (k == QLatin1String(keys::chat::FontFamily) || k == QLatin1String(keys::chat::FontSize))
            m_out->setFont(chatFont(m_core->settings()));
    });
    applyBackscroll();

    // splitter position shared by every room (restored on the first resize, see resizeEvent)
    connect(m_split, &QSplitter::splitterMoved, this, [this] {
        m_core->settings()->setValue(keys::ui::splitterKey(QStringLiteral("chatRoom")), m_split->saveState());
    });
    reloadUsers();
}

ChatRoomView::~ChatRoomView()
{
    if (m_log) {
        m_log->close();
        delete m_log;
    }
}

QString ChatRoomView::headerName() const
{
    return tr("%1 on %2").arg(m_room.name, networkLabel(m_room.net));
}

void ChatRoomView::applyBackscroll()
{
    int n = m_core->settings()->i(keys::chat::BacklogLines);
    if (n <= 0)
        n = 1000;
    if (!m_core->settings()->b(keys::chat::AllowLargeBackscroll))
        n = qMin(n, 1000);
    m_out->setMaxLines(n);
}

void ChatRoomView::setJoined(bool on)
{
    m_joined = on;
    m_input->setEnabled(on);
    update();
}

void ChatRoomView::setRoomInfo(const ChatRoomInfo &r)
{
    m_room.topic = r.topic;
    m_room.users = r.users;
    m_room.limit = r.limit;
    if (!r.name.isEmpty())
        m_room.name = r.name;
    update();
}

void ChatRoomView::loadBacklog()
{
    m_out->clearView();
    for (const ChatMessage &m : m_core->chat()->backlog(m_room.id))
        addMessage(m);
}

void ChatRoomView::addMessage(const ChatMessage &m)
{
    Settings *st = m_core->settings();
    if ((m.kind == ChatKind::Join || m.kind == ChatKind::Part) && !st->b(keys::chat::ShowJoinsParts))
        return;
    if ((m.kind == ChatKind::Message || m.kind == ChatKind::Action) && m.from.isValid() && m_core->users()->isIgnored(m.from))
        return;
    ChatFormat f;
    f.ownName = m_core->network()->screenName();
    f.timestamps = st->b(keys::chat::ShowTimestamps);
    f.timestampFormat = st->s(keys::chat::TimestampFormat);
    if (f.timestampFormat.isEmpty())
        f.timestampFormat = QStringLiteral("hh:mm");
    const QList<Segment> segs = chatSegments(m, f);
    m_out->appendSegments(segs);
    if (m_log) {
        QString plain;
        for (const Segment &s : segs)
            plain += s.text;
        logLine(plain);
    }
}

void ChatRoomView::system(const QString &text, bool error)
{
    m_out->appendLine(text, color(error ? keys::colors::ChatError : keys::colors::ChatSystem));
}

void ChatRoomView::logLine(const QString &plain)
{
    if (!m_log)
        return;
    m_log->write(plain.toUtf8());
    m_log->write("\r\n");
    m_log->flush();
}

void ChatRoomView::clearOutput()
{
    m_out->clearView();
}

void ChatRoomView::scheduleUserReload()
{
    if (!m_userTimer->isActive())
        m_userTimer->start();
}

void ChatRoomView::reloadUsers()
{
    // keep the selection (by user key) across the reset
    QStringList keysSel;
    for (const QModelIndex &i : m_users->selectedSourceRows())
        keysSel << m_userModel->user(i.row()).user.key();
    const int scroll = m_users->verticalScrollBar() ? m_users->verticalScrollBar()->value() : 0;
    m_userModel->setUsers(m_core->chat()->roomUsers(m_room.id));
    for (const QString &k : std::as_const(keysSel)) {
        const int r = m_userModel->rowOfKey(k);
        if (r >= 0) {
            const QModelIndex vi = m_users->mapFromSource(m_userModel->index(r, 0));
            m_users->selectionModel()->select(vi, QItemSelectionModel::Select | QItemSelectionModel::Rows);
        }
    }
    m_users->verticalScrollBar()->setValue(scroll);
}

UserRef ChatRoomView::userAt(const QModelIndex &vi) const
{
    const QModelIndex s = m_users->mapToSource(vi);
    if (!s.isValid())
        return {};
    return m_userModel->user(s.row()).user;
}

bool ChatRoomView::weAreOp() const
{
    const QString me = m_core->network()->screenName();
    for (int i = 0; i < m_userModel->userCount(); ++i) {
        const ChatUser &u = m_userModel->user(i);
        if (u.user.name == me)
            return u.rank >= 2;
    }
    return false;
}

void ChatRoomView::submit(const QString &raw)
{
    const QString text = raw.trimmed();
    if (text.isEmpty())
        return;
    const QString word = text.section(QLatin1Char(' '), 0, 0).toLower();
    const QString arg = text.section(QLatin1Char(' '), 1).trimmed();
    // client-side commands (UI_SPEC §3.3.2)
    if (word == QLatin1String("/clr")) {
        clearOutput();
        return;
    }
    if (word == QLatin1String("/channelname")) {
        system(m_room.name, true);   // echoed in red [obs: web/chat_room_channelname_output.png]
        return;
    }
    if (word == QLatin1String("/log")) {
        if (m_log && arg.isEmpty()) {
            system(tr("Stopped logging to %1").arg(QDir::toNativeSeparators(m_log->fileName())));
            m_log->close();
            delete m_log;
            m_log = nullptr;
            return;
        }
        QString name = arg.isEmpty() ? m_room.name : arg;
        name.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_"));
        if (!name.endsWith(QLatin1String(".txt"), Qt::CaseInsensitive))
            name += QStringLiteral(".txt");
        const QString dir = QDir(m_core->profileDir()).filePath(QStringLiteral("logs/chat"));
        QDir().mkpath(dir);
        delete m_log;
        m_log = new QFile(QDir(dir).filePath(name));
        if (!m_log->open(QIODevice::Append | QIODevice::Text)) {
            system(tr("Cannot open %1 for logging").arg(QDir::toNativeSeparators(m_log->fileName())), true);
            delete m_log;
            m_log = nullptr;
            return;
        }
        system(tr("Logging to %1").arg(QDir::toNativeSeparators(m_log->fileName())));
        return;
    }
    if (!m_joined) {
        system(tr("You are not in this channel. Use Commands > Join Channel."), true);
        return;
    }
    m_core->chat()->sendMessage(m_room.id, text);
}

void ChatRoomView::showCommandsMenu()
{
    auto *menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("chatCommandsMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    auto paste = [this](QMenu *m, const QString &item) {
        m->addAction(item, this, [this, item] {
            m_input->setText(commandStem(item));
            m_input->setFocus();
        });
    };
    paste(menu, QStringLiteral("/action <text>"));
    menu->addSeparator();
    QMenu *cfg = menu->addMenu(tr("Admin Channel Config Commands"));
    for (const char *c : {"/setchannelname <channel>", "/topic <topic>", "/setmotd <motd text>", "/motd", "/limit <maxusers>",
                          "/setchannelvisible <0-1>", "-", "/kickall", "/redirect <channel>", "/allowincomingredirect <0-1>", "-",
                          "/watchcmds [<minlevel> <maxlevel>]", "-", "@advanced", "-", "/savecfg <1-100>", "/loadcfg <1-1000>"}) {
        const QString s = QString::fromLatin1(c);
        if (s == QLatin1String("-")) {
            cfg->addSeparator();
        } else if (s == QLatin1String("@advanced")) {
            QMenu *adv = cfg->addMenu(tr("Advanced"));
            for (const char *a : {"/level_disp_ops <level>", "/level_disp_voice <level>", "/level_kick <level>", "/level_ban <level>",
                                  "/level_topic <level>", "/level_motd <level>", "/setdefbanmin <minutes>",
                                  "/setssbthreshsec <seconds>", "/setssbmin <minutes>"})
                paste(adv, QString::fromLatin1(a));
        } else {
            paste(cfg, s);
        }
    }
    QMenu *lvl = menu->addMenu(tr("Admin Level and Login Commands"));
    for (const char *c : {"/level [<user>]", "/login <passphrase>", "/setuserlevel <user> <level 5-230>", "/setdeflevel <level 5-230>",
                          "/setvoicelevel <level 5-230>", "-", "/mklogin <access level 5-230> <passphrase>", "/rmlogin <passphrase>",
                          "/lslogin", "/clearlogins"}) {
        const QString s = QString::fromLatin1(c);
        if (s == QLatin1String("-"))
            lvl->addSeparator();
        else
            paste(lvl, s);
    }
    QMenu *kb = menu->addMenu(tr("Admin Kick/Ban Commands"));
    for (const char *c : {"/kick <user>", "/ban <user> [minutes]", "/kickban <user> [minutes]", "/banip <user> <ip>[/nm] [minutes]",
                          "/unban <user>", "/listbans", "/clearbans", "/opmsg <message>"})
        paste(kb, QString::fromLatin1(c));
    menu->addSeparator();
    QAction *hold = menu->addAction(tr("Hold Window Open After Part"));
    hold->setCheckable(true);
    hold->setChecked(m_core->settings()->value(chatkeys::HoldOpenAfterPart).toBool());
    connect(hold, &QAction::toggled, this, [this](bool on) { m_core->settings()->setValue(chatkeys::HoldOpenAfterPart, on); });
    menu->addAction(tr("Join Channel"), this, [this] { m_core->chat()->joinRoom(m_room.name); })->setEnabled(!m_joined);
    menu->addAction(tr("Part Channel"), this, [this] { m_core->chat()->leaveRoom(m_room.id); })->setEnabled(m_joined);
    menu->addSeparator();
    menu->addAction(tr("Add To Favorite Channels"), this, [this] { m_core->chat()->setFavouriteRoom(m_room.name, true); });
    menu->addSeparator();
    menu->addAction(tr("Clear Window"), this, &ChatRoomView::clearOutput);
    Settings *st = m_core->settings();
    QMenu *ts = menu->addMenu(tr("Time Stamps"));
    auto *tsGroup = new QActionGroup(ts);
    const bool tsOn = st->b(keys::chat::ShowTimestamps);
    const QString tsFmt = st->s(keys::chat::TimestampFormat);
    struct T { const char *label; const char *fmt; };
    for (const T &t : {T{QT_TR_NOOP("Off"), ""}, T{"[hh:mm]", "hh:mm"}, T{"[hh:mm:ss]", "hh:mm:ss"}}) {
        QAction *a = ts->addAction(tr(t.label));
        a->setCheckable(true);
        a->setActionGroup(tsGroup);
        const QString f = QString::fromLatin1(t.fmt);
        a->setChecked(f.isEmpty() ? !tsOn : (tsOn && tsFmt == f));
        connect(a, &QAction::triggered, this, [this, f] {
            m_core->settings()->setValue(keys::chat::ShowTimestamps, !f.isEmpty());
            if (!f.isEmpty())
                m_core->settings()->setValue(keys::chat::TimestampFormat, f);
        });
    }
    QMenu *bs = menu->addMenu(tr("Backscroll Limit"));
    auto *bsGroup = new QActionGroup(bs);
    const int cur = st->i(keys::chat::BacklogLines);
    for (int n : {100, 300, 500, 1000}) {
        QAction *a = bs->addAction(QString::number(n));
        a->setCheckable(true);
        a->setActionGroup(bsGroup);
        a->setChecked(cur == n);
        connect(a, &QAction::triggered, this, [this, n] { m_core->settings()->setValue(keys::chat::BacklogLines, n); });
    }
    menu->addSeparator();
    menu->addAction(tr("Help..."), this, [this] {
        const QStringList help = {
            tr("Channel commands:"),
            tr("  /me <text>, /action <text>, /emote <text>  - action"),
            tr("  /topic <topic>  - change the topic (host/admins)"),
            tr("  /join <channel>, /part  - join / leave a channel"),
            tr("  /msg <user> <text>  - private message"),
            tr("  /whois <user>, /ignore <user>, /unignore <user>"),
            tr("  /clr  - clear this window,  /log [filename]  - start/stop logging"),
            tr("  /channelname  - show the full channel name"),
            tr("Every other /command is sent to the channel host (see the Admin menus)."),
        };
        for (const QString &l : help)
            system(l);
    });
    // the menu opens above the button, right-aligned with it [obs: web/menu_chat_room_commands.png]
    const QSize sz = menu->sizeHint();
    tips::explainMenu(menu);   // [ui-g] tool tips + status bar text of the entries
    menu->popup(m_commands->mapToGlobal(QPoint(m_commands->width() - sz.width(), -sz.height())));
}

void ChatRoomView::showUserMenu(const QModelIndex &vi, const QPoint &globalPos)
{
    const UserRef u = userAt(vi);
    if (!u.isValid())
        return;
    auto *menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("chatUserMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    QAction *send = menu->addAction(tr("Send Message"), this, [this, u] { m_main->openPrivateMessage(u); });
    menu->setDefaultAction(send);
    menu->addSeparator();
    menu->addAction(tr("Browse"), this, [this, u] { m_main->openBrowse(u); });
    menu->addAction(tr("Whois"), this, [this, u] { m_main->openWhois(u); });
    menu->addSeparator();
    QAction *hot = menu->addAction(tr("Add To Hotlist"), this, [this, u] { m_core->users()->addToHotlist(u); });
    const bool ign = m_core->users()->isIgnored(u);
    QAction *ignA = menu->addAction(ign ? tr("Unignore") : tr("Ignore"), this, [this, u, ign] { m_core->users()->ignore(u, !ign); });
    if (m_core->users()->isSelf(u)) {
        // [ovl-2] our own row: Browse shows our own shared files, Whois what others see; the rest makes
        // no sense for ourselves
        for (QAction *a : {send, hot, ignA}) {
            a->setEnabled(false);
            a->setToolTip(tr("This is you."));
        }
        menu->setDefaultAction(nullptr);
    }
    menu->addSeparator();
    const QString name = u.name.isEmpty() ? u.id : u.name;
    menu->addAction(tr("Copy Name"), this, [name] { QGuiApplication::clipboard()->setText(name); });
    if (weAreOp()) {
        menu->addSeparator();
        menu->addAction(tr("Kick"), this, [this, name] { m_core->chat()->sendMessage(m_room.id, QStringLiteral("/kick ") + name); });
        menu->addAction(tr("Ban"), this, [this, name] { m_core->chat()->sendMessage(m_room.id, QStringLiteral("/ban ") + name); });
        menu->addAction(tr("Kick/Ban"), this, [this, name] { m_core->chat()->sendMessage(m_room.id, QStringLiteral("/kickban ") + name); });
        menu->addAction(tr("Set Level..."), this, [this, name] {
            m_input->setText(QStringLiteral("/setuserlevel %1 ").arg(name));
            m_input->setFocus();
        });
    }
    tips::explainMenu(menu);   // [ui-g] tool tips + status bar text of the entries
    menu->popup(globalPos);
}

void ChatRoomView::paintEvent(QPaintEvent *)
{
    // header (face colour, 2 lines): bold "<channel> on OctoMX Peer Network", then the topic
    QPainter p(this);
    p.setPen(chrome::Text);
    // the room name is drawn larger than the UI font (measured: 13-px high bold text box)
    p.setFont(largeFont());
    const int w = width() - S(4);
    draw::text(&p, SR(1, 33, 0, 13).adjusted(0, 0, w, 0), Qt::AlignLeft, fmt::elide(headerName(), p.fontMetrics(), w));
    p.setFont(normalFont());
    draw::text(&p, SR(3, 52, 0, 13).adjusted(0, 0, w, 0), Qt::AlignLeft, fmt::elide(m_room.topic, p.fontMetrics(), w));
}

void ChatRoomView::resizeEvent(QResizeEvent *)
{
    // measured on web/chat_room_window.png (page coordinates): output/user list from y 76 to
    // 28 px above the page bottom, input line + Commands / Exit Channel 23 px above it
    const int w = width(), h = height();
    const int listH = qMax(S(40), h - S(76) - S(27));
    const bool first = !m_sized;
    m_sized = true;
    m_split->setGeometry(0, S(76), w, listH);
    if (first) {
        const QByteArray sp = m_core->settings()->value(keys::ui::splitterKey(QStringLiteral("chatRoom"))).toByteArray();
        if (sp.isEmpty() || !m_split->restoreState(sp))
            m_split->setSizes({qMax(S(100), w - S(245)), S(242)});
    }
    const int rowY = h - S(23), rowH = S(21);
    m_input->setGeometry(0, rowY, qMax(S(40), w - S(151)), rowH);
    m_commands->setGeometry(w - S(147), rowY, S(73), rowH);
    m_exit->setGeometry(w - S(73), rowY, S(72), rowH);
}

// ======================================================================================= ChatPage

ChatPage::ChatPage(mx::Core *core, MainWindow *mw, QWidget *parent) : Page(QStringLiteral("chat"), tr("Chat"), core, mw, parent)
{
    m_tabs = new TabStrip(this);
    m_tabs->setObjectName(QStringLiteral("chatTabs"));
    m_tabs->setSlotGeometry(168, 172, 4, 682);
    m_tabs->setLeadingButton(IconId::ListChannels, tr("List Channels"))->setObjectName(QStringLiteral("chatListChannels"));
    if (QWidget *b = m_tabs->findChild<QWidget *>(QStringLiteral("chatListChannels")))
        b->setToolTip(tr("Show the list of chat rooms on the network, to pick one to join"));
    m_stack = new QStackedWidget(this);
    m_blank = new QWidget(m_stack);
    m_stack->addWidget(m_blank);
    connect(m_tabs, &TabStrip::leadingClicked, this, &ChatPage::showListChannelsMenu);
    connect(m_tabs, &TabStrip::currentChanged, this, [this](int i) {
        if (QWidget *w = m_tabWidgets.value(i))
            m_stack->setCurrentWidget(w);
        if (auto *r = qobject_cast<ChatRoomView *>(m_stack->currentWidget()))
            r->input()->setFocus();
        m_transient.clear();
        emit statusChanged();
    });

    ChatService *chat = core->chat();
    connect(chat, &ChatService::roomJoined, this, &ChatPage::onJoined);
    connect(chat, &ChatService::roomLeft, this, &ChatPage::onLeft);
    connect(chat, &ChatService::messageReceived, this, &ChatPage::onMessage);
    connect(chat, &ChatService::roomUpdated, this, [this](const ChatRoomInfo &r) {
        if (ChatRoomView *v = room(r.id))
            v->setRoomInfo(r);
    });
    connect(chat, &ChatService::roomUsersChanged, this, [this](const QString &id) {
        if (ChatRoomView *v = room(id))
            v->scheduleUserReload();
    });
    connect(chat, &ChatService::backlogCleared, this, [this](const QString &id) {
        if (ChatRoomView *v = room(id))
            v->clearOutput();
    });
    connect(core->users(), &UserService::ignoredChanged, this, [this] {
        for (ChatRoomView *v : std::as_const(m_rooms))
            v->scheduleUserReload();
    });
    connect(core->settings(), &Settings::changed, this, [this](const QString &k) {
        if (k == QLatin1String(keys::chat::ShowTimestamps) || k == QLatin1String(keys::chat::TimestampFormat))
            for (ChatRoomView *v : std::as_const(m_rooms))
                v->loadBacklog();
    });
    connect(mw, &MainWindow::quitRequested, this, [this] {
        persistOpenRooms();
        m_quitting = true;
    });
    connect(mw, &MainWindow::pageChanged, this, [this](const QString &id) {
        if (id != pageId())
            m_prevPage = id;
    });
    // rejoin the rooms that were open at the last exit once the OctoMX network is up
    connect(core->network(), &NetworkService::networkChanged, this, [this](const NetworkStatus &st) {
        if (st.net == Net::Overlay && st.state == ConnState::Connected)
            rejoinSaved();
    });

    // private messages are routed to their windows from the start of the session
    PrivateChatHub::instance(mw);
    registerUiBScriptVerbs();
    showChannelList();
    // rooms the core already has (auto-joined favourites)
    for (const ChatRoomInfo &r : chat->joinedRooms())
        onJoined(r);
    m_tabs->setCurrentIndex(0);
    QTimer::singleShot(0, this, [this] {
        if (this->core()->network()->status(Net::Overlay).state == ConnState::Connected)
            rejoinSaved();
    });
}

void ChatPage::rejoinSaved()
{
    if (m_rejoined)
        return;
    m_rejoined = true;
    const QStringList saved = core()->settings()->value(chatkeys::OpenRooms).toStringList();
    const QList<ChatRoomInfo> joined = core()->chat()->joinedRooms();
    for (const QString &name : saved) {
        bool have = false;
        for (const ChatRoomInfo &r : joined)
            if (sameRoom(r, name))
                have = true;
        if (!have)
            core()->chat()->joinRoom(name);
    }
}

void ChatPage::persistOpenRooms()
{
    if (m_quitting)
        return;   // the core drops every room while shutting down: keep the list for the restart
    QStringList names;
    for (ChatRoomView *v : std::as_const(m_rooms))
        if (v->isJoined())
            names << v->room().name;
    core()->settings()->setValue(chatkeys::OpenRooms, names);
}

ChatRoomView *ChatPage::room(const QString &roomId) const
{
    for (ChatRoomView *v : m_rooms)
        if (v->roomId() == roomId)
            return v;
    return nullptr;
}

ChatRoomView *ChatPage::currentRoom() const
{
    return qobject_cast<ChatRoomView *>(m_stack->currentWidget());
}

void ChatPage::rebuildTabs()
{
    QWidget *cur = m_stack->currentWidget();
    while (m_tabs->count())
        m_tabs->removeTab(0);
    m_tabWidgets.clear();
    if (m_list) {
        const int i = m_tabs->addTab(networkLabel(m_list->network()), true, IconId::ChannelTab);
        m_tabs->tabButton(i)->setObjectName(QStringLiteral("chatTabList"));
        m_tabs->tabButton(i)->setToolTip(tr("The list of chat rooms of this network"));
        m_tabWidgets << m_list;
    }
    for (int k = 0; k < m_rooms.size(); ++k) {
        const int i = m_tabs->addTab(m_rooms[k]->room().name, true, IconId::Chat);
        m_tabs->tabButton(i)->setObjectName(QStringLiteral("chatTab%1").arg(k));
        m_tabs->tabButton(i)->setProperty("roomTab", true);   // named after the room: no tip needed
        m_tabs->tabButton(i)->setToolTip(m_rooms[k]->room().name);
        m_tabWidgets << m_rooms[k];
    }
    const int idx = int(m_tabWidgets.indexOf(cur));
    if (idx >= 0)
        m_tabs->setCurrentIndex(idx);
    else if (!m_tabWidgets.isEmpty())
        m_tabs->setCurrentIndex(int(m_tabWidgets.size()) - 1);
    else
        m_stack->setCurrentWidget(m_blank);
    emit statusChanged();
}

void ChatPage::setCurrentWidget(QWidget *w)
{
    const int i = int(m_tabWidgets.indexOf(w));
    if (i >= 0) {
        m_tabs->setCurrentIndex(i);
        m_stack->setCurrentWidget(w);
    }
    emit statusChanged();
}

void ChatPage::showChannelList()
{
    if (!m_list) {
        m_list = new ChannelListView(core(), m_stack);
        m_stack->addWidget(m_list);
        connect(m_list, &ChannelListView::joinRequested, this, &ChatPage::join);
        connect(m_list, &ChannelListView::statusChanged, this, [this] {
            if (m_stack->currentWidget() == m_list)
                emit statusChanged();
        });
        connect(m_list, &ChannelListView::closeRequested, this, [this] {
            ChannelListView *l = m_list;
            m_list = nullptr;
            m_stack->removeWidget(l);
            l->deleteLater();
            rebuildTabs();
        });
        m_listFetched = false;
        rebuildTabs();
    }
    setCurrentWidget(m_list);
    if (isVisible() && !m_listFetched && core()->settings()->b(keys::chat::FetchListImmediately)) {
        m_listFetched = true;
        core()->chat()->refreshRoomList(m_list->network());
    }
}

void ChatPage::showChannelListFor(Net n)
{
    const bool other = m_list && m_list->network() != n;
    showChannelList();
    if (other || m_list->network() != n) {
        m_list->setNetwork(n);
        rebuildTabs();
        setCurrentWidget(m_list);
        core()->chat()->refreshRoomList(n);
        m_listFetched = true;
    }
}

void ChatPage::showListChannelsMenu()
{
    auto *menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("listChannelsMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    // every network that offers rooms (ChatService::chatNetworks), our overlay first
    QList<Net> nets = core()->chat()->chatNetworks();
    if (nets.isEmpty())
        nets << Net::Overlay;
    if (nets.removeAll(Net::Overlay))
        nets.prepend(Net::Overlay);
    for (Net n : std::as_const(nets)) {
        QAction *a = menu->addAction(networkLabel(n), this, [this, n] { showChannelListFor(n); });
        if (n == nets.first())
            menu->setActiveAction(a);
    }
    ThinButton *b = m_tabs->leadingButton();
    tips::explainMenu(menu);   // [ui-g] tool tips + status bar text of the entries
    menu->popup(b->mapToGlobal(QPoint(0, b->height())));
}

void ChatPage::join(const QString &roomIdOrName)
{
    const QString r = roomIdOrName.trimmed();
    if (r.isEmpty())
        return;
    for (ChatRoomView *v : std::as_const(m_rooms))
        if (sameRoom(v->room(), r)) {
            setCurrentWidget(v);
            if (!v->isJoined())
                core()->chat()->joinRoom(v->room().name);
            return;
        }
    if (m_rooms.size() >= kMaxRooms) {
        setTransientStatus(tr("You can not be in more than %1 channels at once.").arg(kMaxRooms));
        return;
    }
    m_pendingJoins << r.toLower();
    setTransientStatus(tr("Joining %1...").arg(r));
    core()->chat()->joinRoom(r);
}

void ChatPage::onJoined(const ChatRoomInfo &r)
{
    bool userAsked = false;
    for (int i = 0; i < m_pendingJoins.size(); ++i)
        if (sameRoom(r, m_pendingJoins.at(i))) {
            userAsked = true;
            m_pendingJoins.removeAt(i);
            break;
        }
    ChatRoomView *v = room(r.id);
    if (!v) {
        v = new ChatRoomView(core(), mainWindow(), r, m_stack);
        m_stack->addWidget(v);
        m_rooms << v;
        connect(v, &ChatRoomView::closeRequested, this, [this, v] { closeRoom(v); });
        rebuildTabs();
    } else {
        v->setRoomInfo(r);
    }
    v->setJoined(true);
    v->loadBacklog();
    v->reloadUsers();
    m_transient.clear();
    if (userAsked || m_stack->currentWidget() == m_blank)
        setCurrentWidget(v);
    persistOpenRooms();
    emit statusChanged();
}

void ChatPage::onLeft(const QString &roomId)
{
    ChatRoomView *v = room(roomId);
    if (!v)
        return;
    if (core()->settings()->value(chatkeys::HoldOpenAfterPart).toBool()) {
        v->setJoined(false);
        v->output()->appendLine(tr("You have left the channel."), color(keys::colors::ChatPart));
        persistOpenRooms();
        return;
    }
    closeRoom(v);
}

void ChatPage::closeRoom(ChatRoomView *v)
{
    const int i = int(m_rooms.indexOf(v));
    if (i < 0)
        return;
    m_rooms.removeAt(i);
    m_stack->removeWidget(v);
    v->deleteLater();
    rebuildTabs();
    persistOpenRooms();
}

void ChatPage::onMessage(const ChatMessage &m)
{
    if (m.kind == ChatKind::Private || m.roomId.isEmpty())
        return;   // private messages: PrivateMessageWindow (see main_window_b.cpp)
    if (ChatRoomView *v = room(m.roomId)) {
        v->addMessage(m);
        return;
    }
    // e.g. "Not connected to the chat network." for a join that failed
    if (m.kind == ChatKind::System || m.kind == ChatKind::Error) {
        for (int i = 0; i < m_pendingJoins.size(); ++i)
            if (m_pendingJoins.at(i) == m.roomId.toLower())
                m_pendingJoins.removeAt(i--);
        setTransientStatus(m.text);
    }
}

void ChatPage::setTransientStatus(const QString &s)
{
    m_transient = s;
    emit statusChanged();
}

void ChatPage::focusUser(const UserRef &u)
{
    // a room with that user, else the channel list
    for (ChatRoomView *v : std::as_const(m_rooms))
        if (v->userModel()->rowOfKey(u.key()) >= 0) {
            setCurrentWidget(v);
            return;
        }
    showChannelList();
}

QStringList ChatPage::statusFields() const
{
    if (!m_transient.isEmpty())
        return {m_transient};
    if (m_list && m_stack->currentWidget() == m_list)
        return m_list->statusFields();
    return {};
}

void ChatPage::maybeWarn()
{
    if (s_warnedThisSession || core()->settings()->value(chatkeys::WarningDisabled).toBool())
        return;
    s_warnedThisSession = true;
    // window-modal but not blocking (the script driver and the tests keep running)
    auto *dlg = new ChatWarningDialog(window());
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    // [ui-h] "Do not show this warning again" + Continue: never again (ui/chatWarningDisabled)
    connect(dlg, &QDialog::accepted, this, [this, dlg] {
        if (dlg->doNotShowAgain())
            core()->settings()->setValue(chatkeys::WarningDisabled, true);
    });
    connect(dlg, &QDialog::rejected, this, [this] {
        if (mainWindow() && mainWindow()->currentPageId() == pageId())
            mainWindow()->showPage(m_prevPage);
    });
    dlg->open();
}

void ChatPage::activated()
{
    QTimer::singleShot(0, this, [this] {
        if (!isVisible())
            return;
        maybeWarn();
        if (m_list && !m_listFetched && core()->settings()->b(keys::chat::FetchListImmediately)) {
            m_listFetched = true;
            core()->chat()->refreshRoomList(m_list->network());
        }
        if (ChatRoomView *r = currentRoom())
            r->input()->setFocus();
    });
}

void ChatPage::saveState()
{
    if (m_list)
        m_list->list()->saveState();
    for (ChatRoomView *v : std::as_const(m_rooms))
        v->userList()->saveState();
    persistOpenRooms();
}

bool ChatPage::selectSubTab(const QString &name)
{
    if (name == QLatin1String("list")) {
        showChannelList();
        return true;
    }
    bool ok = false;
    const int i = name.toInt(&ok);
    if (ok && i >= 0 && i < m_tabWidgets.size()) {
        m_tabs->setCurrentIndex(i);
        return true;
    }
    for (ChatRoomView *v : std::as_const(m_rooms))
        if (v->room().name.contains(name, Qt::CaseInsensitive)) {
            setCurrentWidget(v);
            return true;
        }
    return false;
}

bool ChatPage::scriptCommand(const QString &verb, const QStringList &args, QString *error)
{
    // join <room name> | say <text> | part
    if (verb == QLatin1String("join")) {
        join(args.join(QLatin1Char(' ')));
        return true;
    }
    if (verb == QLatin1String("say")) {
        ChatRoomView *r = currentRoom();
        if (!r) {
            *error = QStringLiteral("no room is shown");
            return true;
        }
        r->submit(args.join(QLatin1Char(' ')));
        return true;
    }
    if (verb == QLatin1String("part")) {
        if (ChatRoomView *r = currentRoom())
            core()->chat()->leaveRoom(r->roomId());
        else
            *error = QStringLiteral("no room is shown");
        return true;
    }
    return false;
}

void ChatPage::resizeEvent(QResizeEvent *)
{
    m_tabs->setGeometry(S(2), S(3), width() - S(2), S(21));
    m_stack->setGeometry(0, 0, width(), height());
    m_tabs->raise();
}

} // namespace mx::ui

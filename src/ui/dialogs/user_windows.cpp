// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/dialogs/user_windows.h"

#include <QtCore/QTime>
#include <QtGui/QCloseEvent>
#include <QtGui/QTextBlock>
#include <QtGui/QPainter>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMenu>

#include "mx/services.h"
#include "mx/settings_keys.h"
#include "ui/kit/colors.h"
#include "ui/kit/draw.h"
#include "ui/kit/format.h"
#include "ui/kit/icons.h"
#include "ui/kit/list_view.h"
#include "ui/kit/status_bar.h"
#include "ui/kit/theme.h"
#include "ui/kit/widgets.h"
#include "ui/kit/scale.h"
#include "ui/kit/sound.h"
#include "ui/main_window.h"
#include "ui/models/chat_models.h"
#include "ui/pages/chat_view.h"

namespace mx::ui {

namespace {
QString displayName(const UserRef &u)
{
    return u.name.isEmpty() ? u.id : u.name;
}

void restoreGeo(QWidget *w, mx::Core *core, const QString &name, const QSize &def)
{
    const QByteArray g = core->settings()->value(keys::ui::dialogKey(name)).toByteArray();
    if (g.isEmpty() || !w->restoreGeometry(g))
        w->resize(SS(def));
}

void saveGeo(QWidget *w, mx::Core *core, const QString &name)
{
    core->settings()->setValue(keys::ui::dialogKey(name), w->saveGeometry());
}

QString humanDuration(qint64 sec)
{
    const qint64 d = sec / 86400, h = (sec % 86400) / 3600, m = (sec % 3600) / 60;
    QStringList parts;
    if (d)
        parts << (d == 1 ? QObject::tr("1 day") : QObject::tr("%1 days").arg(d));
    if (h)
        parts << (h == 1 ? QObject::tr("1 hour") : QObject::tr("%1 hours").arg(h));
    if (!d)
        parts << (m == 1 ? QObject::tr("1 minute") : QObject::tr("%1 minutes").arg(m));
    return parts.join(QLatin1Char(' '));
}
} // namespace

// ------------------------------------------------------------------------------------ WhoisView

WhoisView::WhoisView(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("whoisView"));
    connect(Colors::instance(), &Colors::changed, this, qOverload<>(&QWidget::update));
}

void WhoisView::setLines(const QList<QPair<QString, QString>> &lines, const QString &error)
{
    m_lines = lines;
    m_error = error;
    update();
}

void WhoisView::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), color(keys::colors::ListBackground));
    draw::fieldSunken(&p, rect());
    p.setFont(normalFont());
    int y = S(6);
    const int rowH = qMax(S(14), p.fontMetrics().height() + 1);
    for (const auto &l : std::as_const(m_lines)) {
        p.setPen(color(keys::colors::ListText));
        draw::text(&p, QRect(S(8), y, S(86), rowH), Qt::AlignLeft, l.first);
        draw::text(&p, QRect(S(96), y, width() - S(100), rowH), Qt::AlignLeft, fmt::elide(l.second, p.fontMetrics(), width() - S(100)));
        y += rowH;
    }
    if (!m_error.isEmpty()) {
        p.setPen(color(keys::colors::ChatError));
        draw::text(&p, QRect(S(8), y, width() - S(12), rowH), Qt::AlignLeft, fmt::elide(m_error, p.fontMetrics(), width() - S(12)));
    }
}

// ---------------------------------------------------------------------------------- WhoisWindow

QList<QPair<QString, QString>> WhoisWindow::format(const UserRef &u, const QVariantMap &info, QString *error)
{
    QVariantMap m;   // lower-case keys
    for (auto it = info.constBegin(); it != info.constEnd(); ++it)
        m.insert(it.key().toLower(), it.value());
    QList<QPair<QString, QString>> rows;
    auto take = [&m](const QStringList &ks) -> QVariant {
        for (const QString &k : ks)
            if (m.contains(k))
                return m.take(k);
        return {};
    };
    QVariant v = take({QStringLiteral("name"), QStringLiteral("user")});
    rows << qMakePair(tr("User:"), v.isValid() ? v.toString() : displayName(u));
    v = take({QStringLiteral("network")});
    rows << qMakePair(tr("Network:"), v.isValid() ? v.toString() : netName(u.net));
    v = take({QStringLiteral("connection")});
    if (v.isValid())
        rows << qMakePair(tr("Connection:"), v.toString());
    v = take({QStringLiteral("files")});
    const QVariant size = take({QStringLiteral("totalsize")});
    if (v.isValid()) {
        QString f = v.toString();
        if (size.isValid() && size.toLongLong() > 0)
            f = fmt::exactSizes() ? tr("%1 (%2 megs)").arg(fmt::number(v.toLongLong()), fmt::number(size.toLongLong() / (1024 * 1024)))
                                  : tr("%1 (%2)").arg(fmt::number(v.toLongLong()), fmt::size(size.toLongLong()));
        rows << qMakePair(tr("Files:"), f);
    }
    v = take({QStringLiteral("uploads"), QStringLiteral("slots")});
    if (v.isValid())
        rows << qMakePair(tr("Uploads:"), v.toString());
    const QVariant secs = take({QStringLiteral("onlinesec")});
    v = take({QStringLiteral("online")});
    if (secs.isValid())
        rows << qMakePair(tr("Online:"), humanDuration(secs.toLongLong()));
    else if (v.isValid() && v.typeId() == QMetaType::Bool)
        rows << qMakePair(tr("Status:"), v.toBool() ? tr("Online") : tr("Offline"));
    else if (v.isValid())
        rows << qMakePair(tr("Online:"), v.toString());
    v = take({QStringLiteral("client"), QStringLiteral("clientversion")});
    if (v.isValid())
        rows << qMakePair(tr("Client:"), v.toString());
    v = take({QStringLiteral("reachable")});
    if (v.isValid())
        rows << qMakePair(tr("Reachable:"), v.toBool() ? tr("Yes") : tr("No (firewalled)"));
    const QVariant err = take({QStringLiteral("error")});
    if (error)
        *error = err.toString();
    take({QStringLiteral("id")});
    take({QStringLiteral("requestId")});   // [ui-g] internal (the core's request number), not about the user
    for (auto it = m.constBegin(); it != m.constEnd(); ++it) {
        QString label = it.key();
        if (!label.isEmpty())
            label[0] = label.at(0).toUpper();
        rows << qMakePair(label + QLatin1Char(':'), it.value().toString());
    }
    return rows;
}

WhoisWindow::WhoisWindow(mx::Core *core, MainWindow *mw, const UserRef &user, QWidget *parent)
    : QWidget(parent, Qt::Window), m_core(core), m_main(mw), m_user(user)
{
    setObjectName(QStringLiteral("whoisWindow"));
    setProperty("userKey", user.key());
    setAttribute(Qt::WA_DeleteOnClose);
    setAutoFillBackground(true);
    setWindowIcon(appIcon());
    setWindowTitle(tr("Whois: %1").arg(displayName(user)));
    setMinimumSize(SS(260, 160));
    m_view = new WhoisView(this);
    m_browse = new PushButton(tr("Browse"), this);
    m_browse->setObjectName(QStringLiteral("whoisBrowse"));
    m_browse->setToolTip(tr("Open the list of files this user shares, to look through it and download from it."));
    m_message = new PushButton(tr("Send Message"), this);
    m_message->setObjectName(QStringLiteral("whoisSendMessage"));
    m_message->setToolTip(tr("Open a private message window to this user."));
    m_close = new PushButton(tr("Close"), this);
    m_close->setObjectName(QStringLiteral("whoisClose"));
    connect(m_browse, &QPushButton::clicked, this, [this] { m_main->openBrowse(m_user); });
    connect(m_message, &QPushButton::clicked, this, [this] { m_main->openPrivateMessage(m_user); });
    connect(m_close, &QPushButton::clicked, this, &QWidget::close);
    // the answer to our request id (whoisReply); a core without ids answers with whoisResult only
    connect(core->users(), &UserService::whoisReply, this, [this](quint32 id, const UserRef &, const QVariantMap &info) {
        if (id == 0 || id != m_requestId)
            return;
        QString err;
        m_view->setLines(format(m_user, info, &err), err);
        m_hasResult = true;
    });
    connect(core->users(), &UserService::whoisResult, this, [this](const UserRef &u, const QVariantMap &info) {
        if (m_requestId != 0)
            return;   // matched by request id instead
        if (u.key() != m_user.key() && displayName(u) != displayName(m_user))
            return;
        QString err;
        m_view->setLines(format(m_user, info, &err), err);
        m_hasResult = true;
    });
    restoreGeo(this, core, QStringLiteral("whois"), QSize(340, 200));
    request();
}

void WhoisWindow::request()
{
    m_hasResult = false;
    m_view->setLines({qMakePair(tr("User:"), displayName(m_user)), qMakePair(tr("Network:"), netName(m_user.net))},
                     tr("Requesting information..."));
    m_requestId = m_core->users()->requestWhois(m_user);
}

void WhoisWindow::resizeEvent(QResizeEvent *)
{
    const int w = width(), h = height();
    m_view->setGeometry(S(2), S(2), w - S(4), h - S(33));
    const int y = h - S(27), bh = S(23);
    m_close->setGeometry(w - S(79), y, S(75), bh);
    m_message->setGeometry(w - S(79 + 95), y, S(92), bh);
    m_browse->setGeometry(w - S(79 + 95 + 78), y, S(75), bh);
}

void WhoisWindow::closeEvent(QCloseEvent *e)
{
    saveGeo(this, m_core, QStringLiteral("whois"));
    QWidget::closeEvent(e);
}

// ------------------------------------------------------------------------- PrivateMessageWindow

QString PrivateMessageWindow::windowTitleFor(const UserRef &u)
{
    return tr("%1 - Private Message").arg(displayName(u));
}

PrivateMessageWindow::PrivateMessageWindow(mx::Core *core, MainWindow *mw, const UserRef &user, QWidget *parent)
    : QWidget(parent, Qt::Window), m_core(core), m_main(mw), m_user(user)
{
    setObjectName(QStringLiteral("privateMessageWindow"));
    setProperty("userKey", user.key());
    setAttribute(Qt::WA_DeleteOnClose);
    setAutoFillBackground(true);
    setWindowIcon(icon(IconId::Chat));
    setWindowTitle(windowTitleFor(user));
    setMinimumSize(SS(300, 160));
    m_out = new ChatOutput(this);
    m_out->setObjectName(QStringLiteral("pmOutput"));
    m_out->setFont(chatFont(core->settings()));
    // [set-1] Settings > Chat Options: an open window follows the chat font at once, as the rooms do
    connect(core->settings(), &Settings::changed, this, [this, core](const QString &k) {
        if (k == QLatin1String(keys::chat::FontFamily) || k == QLatin1String(keys::chat::FontSize))
            m_out->setFont(chatFont(core->settings()));
    });
    m_input = new ChatInput(this);
    m_input->setObjectName(QStringLiteral("pmInput"));
    m_input->setToolTip(tr("Type a message to this user and press Enter."));
    m_browse = new PushButton(tr("Browse"), this);
    m_browse->setObjectName(QStringLiteral("pmBrowse"));
    m_browse->setToolTip(tr("Open the list of files this user shares, to look through it and download from it."));
    m_whois = new PushButton(tr("Whois"), this);
    m_whois->setObjectName(QStringLiteral("pmWhois"));
    m_whois->setToolTip(tr("Open a window with what this user tells about themselves: network, connection, files shared, upload slots and time online."));
    m_hotlist = new PushButton(tr("Add To Hotlist"), this);
    m_hotlist->setObjectName(QStringLiteral("pmAddHotlist"));
    m_hotlist->setToolTip(tr("Put this user on your Hotlist, so you see when they are online and can browse or message them later."));
    m_status = new StatusBar(this);
    m_status->floatTab()->hide();
    connect(m_input, &ChatInput::submitted, this, &PrivateMessageWindow::submit);
    connect(m_browse, &QPushButton::clicked, this, [this] { m_main->openBrowse(m_user); });
    connect(m_whois, &QPushButton::clicked, this, [this] { m_main->openWhois(m_user); });
    connect(m_hotlist, &QPushButton::clicked, this, [this] { m_core->users()->addToHotlist(m_user); });
    restoreGeo(this, core, QStringLiteral("privateMessage"), QSize(420, 300));
    m_input->setFocus();
}

void PrivateMessageWindow::addMessage(const ChatMessage &m)
{
    ChatFormat f;
    f.ownName = m_core->network()->screenName();
    f.timestamps = m_core->settings()->b(keys::chat::ShowTimestamps);
    f.timestampFormat = m_core->settings()->s(keys::chat::TimestampFormat);
    if (f.timestampFormat.isEmpty())
        f.timestampFormat = QStringLiteral("hh:mm");
    m_out->appendSegments(chatSegments(m, f));
    if (!m.msgId.isEmpty() && m.to.isValid() && m.kind == ChatKind::Private) {
        // [ovl-2] remember where the text of our own line is, to recolour it on its delivery report
        const QTextBlock b = m_out->document()->lastBlock();
        const int len = int(m.text.size()) + 1;   // " text"
        const int end = b.position() + b.length() - 1;
        QTextCursor c(m_out->document());
        c.setPosition(qMax(b.position(), end - len));
        c.setPosition(end, QTextCursor::KeepAnchor);
        m_ownLines.insert(m.msgId, c);
        const auto d = PrivateChatHub::instance(m_main)->delivery(m.msgId);
        if (d.first >= 0)
            setDelivery(m.msgId, d.first, QString());   // (re)opened window: colour only, no repeated lines
    }
}

void PrivateMessageWindow::setDelivery(const QString &msgId, int status, const QString &detail)
{
    const auto st = PmDelivery(status);
    auto it = m_ownLines.find(msgId);
    if (it != m_ownLines.end() && it->hasSelection()) {
        QTextCharFormat cf;
        cf.setForeground(st == PmDelivery::Sending        ? color(keys::colors::ChatTimestamp)
                         : st == PmDelivery::NotDelivered || st == PmDelivery::Refused ? color(keys::colors::ChatError)
                                                                                     : color(keys::colors::ChatText));
        it->mergeCharFormat(cf);
    }
    switch (st) {
    case PmDelivery::Sending: m_deliveryText = tr("Sending..."); break;
    case PmDelivery::Sent: m_deliveryText = tr("Sent (no receipt from this user's version)"); break;
    case PmDelivery::Delivered: m_deliveryText = tr("Delivered %1").arg(QTime::currentTime().toString(QStringLiteral("hh:mm"))); break;
    case PmDelivery::NotDelivered: m_deliveryText = tr("Not delivered"); break;
    case PmDelivery::Refused: m_deliveryText = tr("Refused"); break;
    }
    m_status->setFields({m_deliveryText});
    if (detail.isEmpty())
        return;
    if (st == PmDelivery::NotDelivered)
        m_out->appendLine(tr("Not delivered: %1").arg(detail), keys::colors::ChatError);
    else if (st == PmDelivery::Refused)
        m_out->appendLine(tr("Not delivered: %1").arg(detail), keys::colors::ChatError);
    else if (st == PmDelivery::Delivered)
        m_out->appendLine(tr("Message delivered now."), keys::colors::ChatSystem);
}

void PrivateMessageWindow::submit(const QString &raw)
{
    const QString text = raw.trimmed();
    if (text.isEmpty())
        return;
    if (text.compare(QLatin1String("/clr"), Qt::CaseInsensitive) == 0) {
        m_out->clearView();
        return;
    }
    // the core echoes our own message back (ChatMessage::to set); it is shown from there
    m_core->chat()->sendPrivateMessage(m_user, text);
}

void PrivateMessageWindow::resizeEvent(QResizeEvent *)
{
    const int w = width(), h = height(), sh = m_status->height();
    const int rowY = h - sh - S(23), rh = S(21);
    m_out->setGeometry(0, S(2), w, qMax(S(20), rowY - S(3 + 2)));
    m_hotlist->setGeometry(w - S(89), rowY, S(88), rh);
    m_whois->setGeometry(w - S(89 + 61), rowY, S(60), rh);
    m_browse->setGeometry(w - S(89 + 61 + 61), rowY, S(60), rh);
    m_input->setGeometry(0, rowY, qMax(S(40), w - S(89 + 61 + 61 + 4)), rh);
    m_status->setGeometry(0, h - sh, w, sh);
}

void PrivateMessageWindow::closeEvent(QCloseEvent *e)
{
    saveGeo(this, m_core, QStringLiteral("privateMessage"));
    QWidget::closeEvent(e);
}

// ------------------------------------------------------------------------------- PrivateChatHub

PrivateChatHub *PrivateChatHub::instance(MainWindow *mw)
{
    if (auto *h = mw->findChild<PrivateChatHub *>(QStringLiteral("privateChatHub"), Qt::FindDirectChildrenOnly))
        return h;
    return new PrivateChatHub(mw->core(), mw);
}

PrivateChatHub::PrivateChatHub(mx::Core *core, MainWindow *mw) : QObject(mw), m_core(core), m_main(mw)
{
    setObjectName(QStringLiteral("privateChatHub"));
    connect(core->chat(), &ChatService::messageReceived, this, &PrivateChatHub::onMessage);
    connect(core->chat(), &ChatService::privateMessageStatus, this, &PrivateChatHub::onDelivery);
}

void PrivateChatHub::onDelivery(const UserRef &to, const QString &msgId, int status, const QString &detail)
{
    if (!m_delivery.contains(msgId)) {
        m_deliveryOrder << msgId;
        while (m_deliveryOrder.size() > 2000)
            m_delivery.remove(m_deliveryOrder.takeFirst());
    }
    m_delivery.insert(msgId, qMakePair(status, detail));
    PrivateMessageWindow *w = window(to);
    if (!w && (PmDelivery(status) == PmDelivery::NotDelivered || PmDelivery(status) == PmDelivery::Refused))
        w = open(to, false);   // never silent: a failure shows up even if the window was closed
    if (w)
        w->setDelivery(msgId, status, detail);
}

PrivateMessageWindow *PrivateChatHub::window(const UserRef &u) const
{
    return m_windows.value(u.key()).data();
}

QList<PrivateMessageWindow *> PrivateChatHub::windows() const
{
    QList<PrivateMessageWindow *> l;
    for (const auto &p : m_windows)
        if (p)
            l << p.data();
    return l;
}

PrivateMessageWindow *PrivateChatHub::open(const UserRef &u, bool activate)
{
    if (!u.isValid())
        return nullptr;
    PrivateMessageWindow *w = window(u);
    if (!w) {
        w = new PrivateMessageWindow(m_core, m_main, u, m_main);
        for (const ChatMessage &m : m_history.value(u.key()))
            w->addMessage(m);
        m_windows.insert(u.key(), w);
        if (!activate)
            w->setAttribute(Qt::WA_ShowWithoutActivating);
    }
    w->show();
    if (activate) {
        w->raise();
        w->activateWindow();
        w->input()->setFocus();
    }
    return w;
}

void PrivateChatHub::onMessage(const ChatMessage &m)
{
    if ((m.kind == ChatKind::Error || m.kind == ChatKind::System) && m.roomId.isEmpty() &&
        (m.from.isValid() != m.to.isValid())) {
        // [ovl-2] a notice about a private conversation ("X is not accepting private messages", "X is
        // offline", the core's "not connected" for a message we sent): shown in that user's window; an
        // error opens it, a status line only goes to an open one
        const UserRef partner = m.from.isValid() ? m.from : m.to;
        PrivateMessageWindow *w = window(partner);
        if (!w && m.kind == ChatKind::Error)
            w = open(partner, false);
        if (w)
            w->addMessage(m);
        return;
    }
    if (m.kind != ChatKind::Private)
        return;
    const bool own = m.to.isValid();
    const UserRef partner = own ? m.to : m.from;
    if (!partner.isValid())
        return;
    if (!own && (m_core->users()->isIgnored(partner) || displayName(partner) == m_core->network()->screenName()))
        return;
    QList<ChatMessage> &h = m_history[partner.key()];
    h << m;
    while (h.size() > 500)
        h.removeFirst();
    PrivateMessageWindow *w = window(partner);
    if (w) {
        w->addMessage(m);
    } else if (!own && m_core->settings()->b(keys::messages::OpenWindowOnMessage)) {
        w = open(partner, false);
    }
    if (!own) {
        if (m_core->settings()->b(keys::messages::FlashOnMessage)) {
            if (MainWindow::s_flashHook)   // [set-1] tests see the flash (offscreen has no taskbar)
                MainWindow::s_flashHook();
            else
                QApplication::alert(w ? static_cast<QWidget *>(w) : static_cast<QWidget *>(m_main));
        }
        // [ui-h] Settings > Incoming Msgs & Privacy, Incoming Message Alert (was a beep for every mode)
        sound::alert(m_core->settings(), sound::Event::Message);
    }
}

// ------------------------------------------------------------------------------ AddIgnoreDialog

AddIgnoreDialog::AddIgnoreDialog(QWidget *parent) : QDialog(parent)
{
    setObjectName(QStringLiteral("addIgnoreDialog"));
    setWindowTitle(tr("Add Ignored User"));
    setFixedSize(SS(300, 112));
    auto *l1 = new StaticText(tr("User name:"), this);
    l1->setGeometry(SR(12, 15, 70, 14));
    m_name = new QLineEdit(this);
    m_name->setObjectName(QStringLiteral("ignoreName"));
    m_name->setGeometry(SR(84, 12, 204, 21));
    auto *l2 = new StaticText(tr("Network:"), this);
    l2->setGeometry(SR(12, 42, 70, 14));
    m_net = new QComboBox(this);
    m_net->setObjectName(QStringLiteral("ignoreNetwork"));
    for (Net n : {Net::Overlay, Net::ED2K, Net::Gnutella, Net::BitTorrent, Net::Kad})
        m_net->addItem(n == Net::Overlay ? tr("OctoMX Peer Network") : netName(n), int(n));
    m_net->setGeometry(SR(84, 39, 204, 21));
    auto *ok = new PushButton(tr("OK"), this);
    ok->setObjectName(QStringLiteral("dlgOk"));
    ok->setDefault(true);
    ok->setGeometry(SR(132, 76, 75, 23));
    auto *cancel = new PushButton(tr("Cancel"), this);
    cancel->setObjectName(QStringLiteral("dlgCancel"));
    cancel->setGeometry(SR(213, 76, 75, 23));
    connect(ok, &QPushButton::clicked, this, [this] {
        if (!m_name->text().trimmed().isEmpty())
            accept();
    });
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
}

UserRef AddIgnoreDialog::user() const
{
    UserRef u;
    u.net = Net(m_net->currentData().toInt());
    u.name = m_name->text().trimmed();
    u.id = u.name;
    return u;
}

// -------------------------------------------------------------------------------- IgnoredWindow

IgnoredWindow::IgnoredWindow(mx::Core *core, MainWindow *mw, QWidget *parent)
    : QWidget(parent, Qt::Tool), m_core(core), m_main(mw)
{
    setObjectName(QStringLiteral("ignoredWindow"));
    setAttribute(Qt::WA_DeleteOnClose);
    setAutoFillBackground(true);
    setWindowIcon(icon(IconId::Ignored));
    setWindowTitle(tr("Ignored Users"));
    setMinimumSize(SS(220, 140));
    m_add = new PushButton(tr("Add"), this);
    m_add->setObjectName(QStringLiteral("ignoredAdd"));
    m_add->setToolTip(tr("Type the name of a user to ignore: their chat lines and private messages are not shown any more, and on the OctoMX Peer Network they cannot browse your files, see your whois details or download from you."));
    m_whois = new PushButton(tr("Whois"), this);
    m_whois->setObjectName(QStringLiteral("ignoredWhois"));
    m_whois->setToolTip(tr("Open a Whois window for each selected user."));
    m_remove = new PushButton(tr("Remove"), this);
    m_remove->setObjectName(QStringLiteral("ignoredRemove"));
    m_remove->setToolTip(tr("Stop ignoring the selected users: you see their chat lines and messages again, and they may browse, whois and download from you as your other settings allow."));
    m_model = new IgnoredModel(this);
    m_list = new MxListView(this);
    m_list->setObjectName(QStringLiteral("ignoredList"));
    m_list->setHeaderHidden(true);
    m_list->setSortingEnabled(false);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setModel(m_model);
    m_list->header()->setStretchLastSection(true);
    m_list->setStateKey(core->settings(), QStringLiteral("ignored"));
    m_status = new StatusBar(this);
    m_status->floatTab()->hide();

    connect(m_add, &QPushButton::clicked, this, &IgnoredWindow::addUser);
    connect(m_whois, &QPushButton::clicked, this, [this] {
        for (const UserRef &u : selectedUsers())
            m_main->openWhois(u);
    });
    connect(m_remove, &QPushButton::clicked, this, &IgnoredWindow::removeSelected);
    connect(m_list->selectionModel(), &QItemSelectionModel::selectionChanged, this, &IgnoredWindow::updateButtons);
    connect(m_list, &MxListView::deleteRequested, this, &IgnoredWindow::removeSelected);   // [qa-1] Del
    connect(m_list, &MxListView::contextMenuAt, this, [this](const QModelIndex &i, const QPoint &g) {
        if (!i.isValid())
            return;
        auto *menu = new QMenu(this);
        menu->setObjectName(QStringLiteral("ignoredMenu"));
        menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->setToolTipsVisible(true);
        QAction *w = menu->addAction(tr("Whois"), m_whois, &QPushButton::click);
        w->setToolTip(m_whois->toolTip());
        w->setStatusTip(w->toolTip());
        QAction *r = menu->addAction(tr("Remove"), this, &IgnoredWindow::removeSelected);
        r->setToolTip(m_remove->toolTip());
        r->setStatusTip(r->toolTip());
        menu->popup(g);
    });
    connect(core->users(), &UserService::ignoredChanged, this, &IgnoredWindow::reload);
    restoreGeo(this, core, QStringLiteral("ignored"), QSize(344, 226));
    reload();
}

void IgnoredWindow::reload()
{
    QStringList sel;
    for (const UserRef &u : selectedUsers())
        sel << u.key();
    m_model->setUsers(m_core->users()->ignored());
    for (int r = 0; r < m_model->userCount(); ++r)
        if (sel.contains(m_model->user(r).key()))
            m_list->selectionModel()->select(m_model->index(r), QItemSelectionModel::Select);
    const int n = m_model->userCount();
    m_status->setFields({n == 1 ? tr("1 user in list") : tr("%1 users in list").arg(n)});
    updateButtons();
}

QList<UserRef> IgnoredWindow::selectedUsers() const
{
    QList<UserRef> l;
    for (const QModelIndex &i : m_list->selectionModel()->selectedRows())
        l << m_model->user(i.row());
    return l;
}

void IgnoredWindow::updateButtons()
{
    const bool sel = !m_list->selectionModel()->selectedRows().isEmpty();
    m_whois->setEnabled(sel);
    m_remove->setEnabled(sel);
}

void IgnoredWindow::removeSelected()
{
    const QList<UserRef> users = selectedUsers();
    for (const UserRef &u : users)
        m_core->users()->ignore(u, false);
}

void IgnoredWindow::addUser()
{
    auto *dlg = new AddIgnoreDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &QDialog::accepted, this, [this, dlg] { m_core->users()->ignore(dlg->user(), true); });
    dlg->open();
}

void IgnoredWindow::resizeEvent(QResizeEvent *)
{
    // measured on web/ignored_users_window.png (client coordinates): three equal push buttons,
    // the list from y 26, the status bar
    const int w = width(), h = height(), sh = m_status->height();
    const int two = S(2), bw = (w - 2 * two) / 3, y = S(3), bh = S(21);
    m_add->setGeometry(two, y, bw, bh);
    m_whois->setGeometry(two + bw, y, bw, bh);
    m_remove->setGeometry(two + 2 * bw, y, w - 2 * two - 2 * bw, bh);
    m_list->setGeometry(two, S(26), w - 2 * two, qMax(0, h - S(26) - sh));
    m_status->setGeometry(0, h - sh, w, sh);
}

void IgnoredWindow::closeEvent(QCloseEvent *e)
{
    saveGeo(this, m_core, QStringLiteral("ignored"));
    QWidget::closeEvent(e);
}

} // namespace mx::ui

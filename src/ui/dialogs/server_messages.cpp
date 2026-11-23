// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/dialogs/server_messages.h"

#include <QtGui/QCloseEvent>
#include <QtWidgets/QVBoxLayout>

#include "mx/services.h"
#include "mx/settings_keys.h"
#include "ui/kit/colors.h"
#include "ui/kit/icons.h"
#include "ui/kit/status_bar.h"
#include "ui/kit/scale.h"
#include "ui/pages/chat_view.h"

namespace mx::ui {

ServerMessagesWindow::ServerMessagesWindow(mx::Core *core, Net net, const QString &serverId, QWidget *parent)
    : QWidget(parent, Qt::Window), m_core(core), m_net(net), m_serverId(serverId)
{
    setObjectName(QStringLiteral("serverMessagesWindow"));
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowIcon(appIcon());
    QString title = tr("[%1] Server Messages").arg(netName(net));
    for (const ServerEntry &e : core->network()->servers(net))
        if (e.id == serverId)
            title = tr("[%1] %2 - Server Messages").arg(netName(net), e.name.isEmpty() ? e.address : e.name);
    setWindowTitle(title);
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    m_out = new ChatOutput(this);
    m_out->setObjectName(QStringLiteral("serverMessagesText"));
    m_out->setMaxLines(1000);
    m_out->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);   // no text cursor
    lay->addWidget(m_out, 1);
    m_status = new StatusBar(this);
    m_status->floatTab()->hide();
    lay->addWidget(m_status);

    const QByteArray g = core->settings()->value(keys::ui::dialogKey(QStringLiteral("serverMessages"))).toByteArray();
    if (g.isEmpty() || !restoreGeometry(g))
        resize(SS(520, 300));
    m_restored = true;

    for (const ServerMessage &m : core->network()->serverMessages(net))
        if (wants(m))
            append(m);
    if (m_count == 0)
        m_out->appendLine(tr("No messages from this server yet."), color(keys::colors::ChatTimestamp));
    connect(core->network(), &NetworkService::serverMessageReceived, this, [this](const ServerMessage &m) {
        if (wants(m))
            append(m);
    });
    connect(Colors::instance(), &Colors::changed, m_out, &ChatOutput::applyColors);
    updateStatus();
}

bool ServerMessagesWindow::wants(const ServerMessage &m) const
{
    return m.net == m_net && (m_serverId.isEmpty() || m.serverId == m_serverId);
}

void ServerMessagesWindow::append(const ServerMessage &m)
{
    if (m_count == 0)
        m_out->clearView();
    ++m_count;
    QList<Segment> segs;
    // servers send their MOTD in one burst: the time tells old from new lines
    const QDateTime t = m.time.isValid() ? m.time.toLocalTime() : QDateTime::currentDateTime();
    segs << Segment{t.toString(QStringLiteral("[hh:mm] ")), color(keys::colors::ChatTimestamp)};
    if (m_serverId.isEmpty() && !m.serverName.isEmpty())
        segs << Segment{QStringLiteral("%1: ").arg(m.serverName), color(keys::colors::ChatSystem)};
    segs << Segment{m.text, color(keys::colors::ChatMotd)};
    m_out->appendSegments(segs);
    updateStatus();
}

void ServerMessagesWindow::updateStatus()
{
    m_status->setFields({m_count == 1 ? tr("1 message") : tr("%1 messages").arg(m_count)});   // [qa-1] no "(s)"
}

int ServerMessagesWindow::lineCount() const
{
    return m_count;
}

QString ServerMessagesWindow::plainText() const
{
    return m_out->plainText();
}

void ServerMessagesWindow::closeEvent(QCloseEvent *e)
{
    m_core->settings()->setValue(keys::ui::dialogKey(QStringLiteral("serverMessages")), saveGeometry());
    QWidget::closeEvent(e);
}

void ServerMessagesWindow::moveEvent(QMoveEvent *e)
{
    QWidget::moveEvent(e);
    if (m_restored && isVisible())
        m_core->settings()->setValue(keys::ui::dialogKey(QStringLiteral("serverMessages")), saveGeometry());
}

void ServerMessagesWindow::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    if (m_restored && isVisible())
        m_core->settings()->setValue(keys::ui::dialogKey(QStringLiteral("serverMessages")), saveGeometry());
}

} // namespace mx::ui

// SPDX-License-Identifier: GPL-3.0-or-later
// [str-1] OctoMX UI: Settings ▸ Other Networks ▸ Streaming API: the local API another program uses to
// search, pick a result and get a stream link for a player (docs/API.md). Off by default; every
// change applies at once.
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtCore/QRandomGenerator>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QSpinBox>

#include "mx/services.h"
#include "ui/dialogs/settings_dialog.h"
#include "ui/dialogs/settings_pages.h"
#include "ui/kit/widgets.h"

namespace mx::ui {

using namespace mx::keys;

namespace {

QString newApiToken()
{
    QByteArray b(24, Qt::Uninitialized);
    for (int i = 0; i < b.size(); ++i) b[i] = char(QRandomGenerator::system()->bounded(256));
    return QString::fromLatin1(b.toHex());
}

QString stateText(const StreamApiState &st)
{
    if (!st.available) return QObject::tr("This core has no streaming API.");
    if (!st.enabled) return QObject::tr("Off: nothing listens.");
    if (!st.error.isEmpty()) return st.error;
    if (!st.listening) return QObject::tr("Starting...");
    QString t = QObject::tr("Listening on %1, port %2.").arg(st.addresses.join(QStringLiteral(", "))).arg(st.port);
    if (st.streams > 0)
        t += QObject::tr(" %1 open, %2 reading.").arg(countNoun(st.streams, QObject::tr("stream")), countNoun(st.players, QObject::tr("player")));
    return t;
}

} // namespace

void buildStreamApiPage(SettingsDialog *d)
{
    PageKit k(d, d->addPage(QStringLiteral("api"), QObject::tr("Streaming API"), QStringLiteral("other")));
    Form f(k);
    f.group(QObject::tr("Streaming API (for players and other programs)"));
    f.check(api::Enabled, QObject::tr("Switch on the streaming API"));
    f.spin(api::Port, QObject::tr("Port"), QString(), 96);
    f.check(api::AllowLan, QObject::tr("Allow other devices on my network"));
    // the token: shown, copied, made new (the old one and every old stream link stop working)
    const int tokenY = f.y;
    k.label(QObject::tr("Access token"), f.left(), tokenY + 4, 92);
    QLineEdit *token = k.edit(api::Token, f.left() + 96, tokenY, f.width() - 96 - 2 * 58, 20);
    token->setReadOnly(true);
    PushButton *copyToken = k.button(QObject::tr("Copy"), f.left() + f.width() - 2 * 58 + 4, tokenY - 1, 54, 22);
    copyToken->setObjectName(QStringLiteral("apiCopyToken"));
    copyToken->setToolTip(QObject::tr("Put the access token in the clipboard, to paste it into the program that uses the API."));
    PushButton *newToken = k.button(QObject::tr("New"), f.left() + f.width() - 58 + 4, tokenY - 1, 54, 22);
    newToken->setObjectName(QStringLiteral("apiNewToken"));
    newToken->setToolTip(QObject::tr("Make a new access token. The old token and every stream link made with it stop working at once."));
    f.y += 24;
    // the base address
    const int baseY = f.y;
    k.label(QObject::tr("Address"), f.left(), baseY + 4, 92);
    auto *base = k.place(new QLineEdit, f.left() + 96, baseY, f.width() - 96 - 58, 20);
    base->setObjectName(QStringLiteral("apiBaseAddress"));
    base->setReadOnly(true);
    base->setToolTip(QObject::tr("The address of the API for the program that uses it (docs/API.md lists the calls)."));
    PushButton *copyBase = k.button(QObject::tr("Copy"), f.left() + f.width() - 58 + 4, baseY - 1, 54, 22);
    copyBase->setObjectName(QStringLiteral("apiCopyAddress"));
    copyBase->setToolTip(QObject::tr("Put the API's address in the clipboard."));
    f.y += 24;
    WrapLabel *state = f.text(QString(), 2);
    state->setObjectName(QStringLiteral("apiState"));
    state->setToolTip(QObject::tr("Whether the API listens now, where, and why not (a port another program uses)."));
    f.endGroup();
    f.group(QObject::tr("Stream Cache (in the incomplete folder)"));
    f.spin(api::CacheLimitMB, QObject::tr("All stream caches at most"), QObject::tr("MB"), 170)->setMaximum(1024 * 1024);
    f.spin(api::IdleMin, QObject::tr("Delete unread streams after"), QObject::tr("minutes"), 170)->setMaximum(7 * 1440);
    f.check(api::KeepStreams, QObject::tr("Keep what was streamed as a download"));
    f.endGroup();
    f.text(QObject::tr("Off by default. A player gets only a link; OctoMX fetches what it reads, from every network, and deletes the cache afterwards."), 3);

    Core *core = d->core();
    auto refresh = [core, state, base, token] {
        const StreamApiState st = core->streamApiState();
        state->setText(stateText(st));
        base->setText(st.available ? st.baseUrl : QString());
        base->setCursorPosition(0);
        token->setCursorPosition(0);
    };
    refresh();
    QObject::connect(core, &Core::streamApiChanged, state, refresh);
    QObject::connect(core->settings(), &Settings::changed, state, [refresh](const QString &key, const QVariant &) {
        if (key.startsWith(QLatin1String("api/"))) refresh();
    });
    QObject::connect(copyToken, &QPushButton::clicked, token, [core] { QGuiApplication::clipboard()->setText(core->settings()->s(api::Token)); });
    QObject::connect(copyBase, &QPushButton::clicked, base, [base] { QGuiApplication::clipboard()->setText(base->text()); });
    QObject::connect(newToken, &QPushButton::clicked, token, [core] { core->settings()->setValue(api::Token, newApiToken()); });
}

} // namespace mx::ui

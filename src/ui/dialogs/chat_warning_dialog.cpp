// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/dialogs/chat_warning_dialog.h"

#include <QtGui/QPainter>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QStyle>

#include "ui/kit/colors.h"
#include "ui/kit/draw.h"
#include "ui/kit/widgets.h"
#include "ui/kit/scale.h"

namespace mx::ui {

namespace {
constexpr int kTop = 12;
constexpr int kPitch = 13;
} // namespace

QStringList ChatWarningDialog::lines()
{
    return {
        QStringLiteral("*** WARNING ***"),
        QString(),
        // [ren-1] our own words (the 3.53 dialog had a text of its own with the same message)
        QStringLiteral("Chat rooms on the OctoMX Peer Network run without any central server."),
        QStringLiteral("Nobody but the people in a room can keep order there, and some rooms"),
        QStringLiteral("may contain crude, offensive or explicit talk."),
        QString(),
        QStringLiteral("PLEASE DO NOT GO ON IF RUDE OR SHOCKING LANGUAGE UPSETS YOU,"),
        QStringLiteral("OR IF YOU DO NOT WANT TO TAKE PART IN CHAT ROOMS THAT"),
        QStringLiteral("NOBODY MODERATES OR SUPERVISES."),
        QString(),
        QStringLiteral("The people who write and publish OctoMX take no part in these rooms"),
        QStringLiteral("and do not watch them.  They cannot answer for what other users say"),
        QStringLiteral("or do there."),
        QString(),
        QStringLiteral("Do you want to open the chat?"),
    };
}

ChatWarningDialog::ChatWarningDialog(QWidget *parent) : QDialog(parent)
{
    setObjectName(QStringLiteral("chatWarningDialog"));
    setWindowTitle(tr("OctoMX Decentralized Chat"));
    setWindowIcon(style()->standardIcon(QStyle::SP_MessageBoxWarning));
    setFixedSize(SS(406, 292));   // [ui-h] 18 px more for "Do not show this warning again"
    m_again = new QCheckBox(tr("Do not show this warning again"), this);
    m_again->setObjectName(QStringLiteral("chatWarningAgain"));
    m_again->setToolTip(tr("Tick it and press Continue: the Chat page opens without this warning from now on. "
                           "Settings > Advanced, ui/chatWarningDisabled brings it back."));
    m_again->setGeometry(SR(118, 230, 200, 16));
    auto *cont = new PushButton(tr("Continue"), this);
    cont->setObjectName(QStringLiteral("chatWarningContinue"));
    cont->setToolTip(tr("Open the Chat page. This warning is shown once each time OctoMX starts, unless the box above is ticked."));
    cont->setDefault(true);
    cont->setAutoDefault(true);
    cont->setGeometry(SR(126, 255, 75, 23));
    auto *cancel = new PushButton(tr("Cancel"), this);
    cancel->setObjectName(QStringLiteral("chatWarningCancel"));
    cancel->setToolTip(tr("Do not open the chat and go back to the page you came from."));
    cancel->setGeometry(SR(207, 255, 75, 23));
    connect(cont, &QPushButton::clicked, this, &QDialog::accept);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
}

bool ChatWarningDialog::doNotShowAgain() const
{
    return m_again && m_again->isChecked();
}

void ChatWarningDialog::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setPen(chrome::Text);
    const QStringList l = lines();
    for (int i = 0; i < l.size(); ++i)
        draw::text(&p, QRect(0, S(kTop + i * kPitch), width(), S(kPitch)), Qt::AlignHCenter, l.at(i));
}

} // namespace mx::ui

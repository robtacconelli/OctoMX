// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/dialogs/legal_notice.h"

#include <QtWidgets/QCheckBox>

#include "mx/services.h"
#include "mx/settings_keys.h"
#include "ui/kit/icons.h"
#include "ui/kit/scale.h"
#include "ui/kit/widgets.h"

namespace mx::ui {

QStringList LegalNotice::paragraphs()
{
    return {
        tr("OctoMX is a general tool for exchanging legal content over several peer-to-peer networks. "
           "It was not made to promote piracy or the exchange of illegal or copyright-protected material."),
        tr("By using this program you agree that:"),
        tr("1. You will use it only in ways that are legal where you live, and you will download and share "
           "only content that you have the right to download and share."),
        tr("2. You alone are responsible for what you search for, download and share."),
        tr("3. The authors and contributors do not host, control or check any content on these networks. "
           "They are not responsible for the content exchanged or for how the program is used."),
        tr("4. The program is free software under the GNU General Public License, version 3 or later, and "
           "comes with no warranty."),
    };
}

LegalNotice::LegalNotice(bool readOnly, QWidget *parent) : QDialog(parent)
{
    setObjectName(QStringLiteral("legalNotice"));
    setWindowTitle(tr("OctoMX Terms of Use"));
    setWindowIcon(appIcon());
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);

    const int margin = S(18);
    const int width = S(440);
    int y = S(16);
    const QStringList text = paragraphs();
    for (int i = 0; i < text.size(); ++i) {
        auto *p = new WrapLabel(text.at(i), this);
        p->setObjectName(QStringLiteral("legalText%1").arg(i));
        const int h = p->heightForWidth(width);
        p->setGeometry(margin, y, width, h);
        y += h + S(i == 0 ? 12 : 7);
    }
    y += S(8);
    const int buttonW = S(96), buttonH = S(23);
    if (readOnly) {
        auto *close = new PushButton(tr("Close"), this);
        close->setObjectName(QStringLiteral("legalClose"));
        close->setDefault(true);
        close->setGeometry(margin + (width - buttonW) / 2, y, buttonW, buttonH);
        connect(close, &QPushButton::clicked, this, &QDialog::accept);
    } else {
        m_check = new QCheckBox(tr("I have read this and I will use OctoMX only for legal purposes"), this);
        m_check->setObjectName(QStringLiteral("legalCheck"));
        m_check->setToolTip(tr("Tick this to confirm the terms above. I Agree becomes available then."));
        m_check->setGeometry(margin, y, width, S(16));
        y += S(16) + S(14);
        m_agree = new PushButton(tr("I Agree"), this);
        m_agree->setObjectName(QStringLiteral("legalAgree"));
        m_agree->setToolTip(tr("Accept the terms and start OctoMX. You are asked only once."));
        m_agree->setEnabled(false);
        m_agree->setGeometry(margin + width / 2 - buttonW - S(6), y, buttonW, buttonH);
        auto *exit = new PushButton(tr("Exit"), this);
        exit->setObjectName(QStringLiteral("legalExit"));
        exit->setToolTip(tr("Do not accept: OctoMX closes without connecting to any network."));
        exit->setGeometry(margin + width / 2 + S(6), y, buttonW, buttonH);
        connect(m_check, &QCheckBox::toggled, m_agree, [this](bool on) {
            m_agree->setEnabled(on);
            if (on)
                m_agree->setDefault(true);
        });
        connect(m_agree, &QPushButton::clicked, this, &QDialog::accept);
        connect(exit, &QPushButton::clicked, this, &QDialog::reject);
    }
    y += buttonH + S(14);
    setFixedSize(width + 2 * margin, y);
}

bool LegalNotice::accepted(const Settings *settings)
{
    return settings && settings->i(keys::internal::TermsAccepted) >= kTermsVersion;
}

bool LegalNotice::ensureAccepted(Settings *settings, QWidget *parent)
{
    if (accepted(settings))
        return true;
    LegalNotice dlg(false, parent);
    if (dlg.exec() != QDialog::Accepted)
        return false;
    settings->setValue(keys::internal::TermsAccepted, kTermsVersion);
    settings->sync();
    return true;
}

} // namespace mx::ui

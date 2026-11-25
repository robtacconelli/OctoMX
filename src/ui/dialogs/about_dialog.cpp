// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/dialogs/about_dialog.h"

#include <QtCore/QFile>
#include <QtGui/QPainter>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPlainTextEdit>

#include "mx/services.h"
#include "ui/kit/icons.h"
#include "ui/kit/theme.h"
#include "ui/kit/widgets.h"
#include "ui/kit/scale.h"
#include "ui/main_window.h"
#include "ui/update_ui.h"

namespace mx::ui {

AboutDialog::AboutDialog(mx::Core *core, QWidget *parent) : QDialog(parent)
{
    setObjectName(QStringLiteral("aboutDialog"));
    setWindowTitle(tr("About OctoMX"));
    setWindowIcon(appIcon());
    setFixedSize(SS(520, 346));   // [ren-1] wider: the former name and the statement of independence

    auto *icon = new QLabel(this);
    icon->setPixmap(QPixmap::fromImage(iconImage(IconId::App, S(32))));
    icon->setGeometry(SR(12, 12, 32, 32));

    auto *title = new QLabel(QStringLiteral("OctoMX v%1").arg(core->versionString()), this);
    title->setFont(boldFont());
    title->setGeometry(SR(56, 12, 340, 14));

    QStringList nets;
    for (const NetworkStatus &s : core->network()->networks())
        nets << netName(s.net);
    auto *info = new QLabel(this);
    info->setWordWrap(true);
    // [ren-1] what the program is, and the statement about the original WinMX
    info->setText(tr("OctoMX: file sharing with the classic WinMX-style interface on modern "
                     "peer-to-peer networks.\nNetworks: %1.%2\n\n"
                     "OctoMX is an independent open-source program. It is not affiliated with, endorsed by or derived "
                     "from the original WinMX or its makers, and contains none of its code; WinMX is named only to "
                     "describe the style of the interface.")
                      .arg(nets.join(QStringLiteral(", ")),
                           core->isMock() ? tr("\nDemo mode: no network connections are made.") : QString()));
    info->setObjectName(QStringLiteral("aboutText"));
    info->setGeometry(SR(56, 30, 450, 106));
    // [upd-1] what the update check found, right of the title: "OctoMX 5.7 is up to date." / "... 5.8 is available ..."
    if (UpdateController *u = UpdateController::of(qobject_cast<MainWindow *>(parent))) {
        auto *upd = new QLabel(u->statusLine(), this);
        upd->setObjectName(QStringLiteral("aboutUpdateStatus"));
        upd->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        upd->setGeometry(SR(200, 12, 308, 14));
    }
    info->setAlignment(Qt::AlignLeft | Qt::AlignTop);

    auto *lic = new QLabel(tr("Credits and licences of bundled components:"), this);
    lic->setGeometry(SR(12, 138, 496, 14));

    auto *text = new QPlainTextEdit(this);
    text->setObjectName(QStringLiteral("aboutLicenses"));
    text->setReadOnly(true);
    text->setLineWrapMode(QPlainTextEdit::NoWrap);
    QFile f(QStringLiteral(":/mx/licenses.txt"));
    if (f.open(QIODevice::ReadOnly))
        text->setPlainText(QString::fromUtf8(f.readAll()));
    text->setGeometry(SR(12, 156, 496, 150));

    auto *ok = new PushButton(tr("OK"), this);
    ok->setObjectName(QStringLiteral("aboutOk"));
    ok->setDefault(true);
    ok->setGeometry(SR(220, 314, 80, 23));
    connect(ok, &QPushButton::clicked, this, &QDialog::accept);
    // [post-1] left of OK, where the bottom row had room
    auto *upd = new PushButton(tr("Check for Updates..."), this);
    upd->setObjectName(QStringLiteral("aboutCheckUpdates"));
    upd->setToolTip(tr("Ask the project page whether a newer OctoMX is out. Update Now in the answer downloads, checks and installs it."));
    upd->setGeometry(SR(12, 314, 120, 23));
    connect(upd, &QPushButton::clicked, this, &AboutDialog::checkForUpdatesRequested);
}

} // namespace mx::ui

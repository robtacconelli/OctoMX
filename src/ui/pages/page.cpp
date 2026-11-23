// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/pages/page.h"

#include "ui/kit/colors.h"

namespace mx::ui {

Page::Page(const QString &id, const QString &title, mx::Core *core, MainWindow *mainWindow, QWidget *parent)
    : QWidget(parent), m_id(id), m_title(title), m_core(core), m_main(mainWindow)
{
    setObjectName(QStringLiteral("page_") + id);
    setAutoFillBackground(true);
}

PlaceholderPage::PlaceholderPage(const QString &id, const QString &title, mx::Core *core, MainWindow *mw, QWidget *parent)
    : Page(id, title, core, mw, parent)
{
}

} // namespace mx::ui

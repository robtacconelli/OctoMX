// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI [ui-g]: plain explanations of the menu entries that do not say it all ("Enter Remote
// Queue", "Perm add to list" ...), shown as menu tool tips and in the status bar while hovered.
#pragma once

#include <QtCore/QHash>
#include <QtCore/QString>

class QMenu;

namespace mx::ui::tips {

// the explanation of a common context-menu entry (by its text without '&'), or an empty string
QString forMenuEntry(const QString &text);

// Gives every entry of `menu` (and its submenus) without a tool tip the explanation from `extra` or
// forMenuEntry(), as tool tip and status tip, and shows the tool tips (QMenu::setToolTipsVisible).
// Entries that need none (their text is the whole story) keep the default (no tip).
void explainMenu(QMenu *menu, const QHash<QString, QString> &extra = {});

// A plain tip broken into lines of at most `designWidth` design px (scaled) in the tool-tip font
QString wrapped(const QString &text, int designWidth = 320);

// Application-wide: long plain tool tips of widgets are shown wrapped at a readable width, just below
// the widget (so they never cover the button they explain). Called by installClassicLook().
void installToolTipWrapper(QObject *app);

} // namespace mx::ui::tips

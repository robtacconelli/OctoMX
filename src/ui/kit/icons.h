// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: the 16×16 pixel-art icons of UI_SPEC §1.9, drawn procedurally into QImages
// (no smoothing, no scaling: on an integer high-DPI scale factor they are pixel-doubled).
#pragma once

#include <QtGui/QIcon>
#include <QtGui/QImage>
#include <QtGui/QPixmap>

namespace mx::ui {

enum class IconId {
    App,             // black disc, gray ring, green "MX"
    Networks,        // PC with teal screen
    SharedFiles,     // open yellow folder with papers
    Chat,            // speech balloon
    Search,          // magnifier
    Hotlist,         // flames over a card
    Transfers,       // green up/down arrows
    Bandwidth,       // area chart
    Ignored,         // red no-entry sign
    Settings,        // crossed tools
    Help,            // yellow question mark
    ShareFolder,     // green plus
    UnshareFolder,   // red minus bar
    Refresh,         // green circular arrow
    SharedFileTypes, // page with "?"
    ScanForMedia,    // page with magnifier
    OpenNewSearch,   // magnifier + sparkle
    SearchTab,       // small magnifier
    ListChannels,    // balloon + star
    ChannelTab,      // white square with '#'
    Browse,          // binoculars
    Folder,          // small closed yellow folder (list rows)
    File,            // small white page (list rows)
};

// 16×16 (or `size` for App: 16, 32, 48, 256) image with transparent background.
QImage iconImage(IconId id, int size = 16);
QPixmap iconPixmap(IconId id);
QIcon icon(IconId id);
// The multi-size application icon (16 ... 64 px, every size a title bar or a tray uses).
QIcon appIcon();
// [ui-e] every icon id and its name ("networks", "help" ...), icon export / review
QList<IconId> allIcons();
QString iconName(IconId id);
// [ui-h] the information sign of the original WinMX's own message boxes (a blue disc with a white "i"), drawn the
// same on every platform (the style's icon is wine's generic one under wine), `px` pixels square
QPixmap informationPixmap(int px);

} // namespace mx::ui

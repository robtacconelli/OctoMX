// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI, [ui-f]: the Search file-type menu, its labels and texts, built from the one type table
// in mxapi (mx::extensionsForMediaType, mx::FileTypeFilter). Used by the Search page, Settings ▸
// Search (default type and the button presets) and Shared File Types.
#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>

class QFontMetrics;
class QMenu;

namespace mx::ui {

class MenuButton;

namespace filetypes {

// "Audio", "Video", "Pictures", "Documents", "Software", "Archives", "Torrents", "MP3 only",
// "Lossless audio", "Server Lists", "Colour Schemes", "All Files", "Custom" (kind of a code)
QString kindName(const QString &kind);
// the extensions behind a kind (general types: the whole table row; others: the required ones)
QStringList kindExtensions(const QString &kind);
// "Audio (.mp3 .flac .m4a .ogg ...)": the first four extensions, all of them when there are <= 5
QString groupLabel(const QString &name, const QStringList &extensions);
QString menuLabel(const QString &kind);
// Button text of a code: "All Files", "Video (.mkv .mp4 .avi .mov ...)", "Audio - At least 192 kbps",
// "MP3 only - Any Bitrate", "Custom (.mkv .srt)"; empty for an invalid code.
QString buttonText(const QString &code);
// The same, shortened (fewer extensions) until it fits `width` pixels.
QString fittedButtonText(const QString &code, const QFontMetrics &fm, int width);
// The whole truth, for tool tips and the status bar: "Video: .mkv .mp4 .avi ... .mxf"
// (+ ", bitrate at least 192 kbps").
QString description(const QString &code);
QString kindDescription(const QString &kind);

// Fills a file-type MenuButton (data = codes): All Files, Audio ▸ (bitrates), Video, Pictures,
// Documents, Software, Archives, Disc Images, Torrents, a separator, MP3 only ▸, Lossless audio, Server Lists, Colour Schemes,
// then either the entry "Custom Extensions..." (data "custom", `customDialogEntry`) or one choice per
// code in `customCodes` ("Custom (.mkv .srt)"). Every entry has the full extension list as tool tip
// and status tip; the menus show tool tips; the button text is fitted to the button (fittedButtonText).
void populateMenu(MenuButton *button, bool customDialogEntry, const QStringList &customCodes = {});
// all menus of a button (the main menu and its submenus), e.g. to follow hovered()
QList<QMenu *> menusOf(MenuButton *button);

} // namespace filetypes
} // namespace mx::ui

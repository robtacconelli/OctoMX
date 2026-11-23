// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: [ui-g] the user's translation file (Settings > Appearance > Load Language From File...).
//
// misc/languageFile names a Qt translation (.qm, made with lupdate + Qt Linguist + lrelease); empty =
// English. follow() loads it at start (before any window exists, so every text is translated) and
// whenever the setting changes; texts already on screen keep their language until their window is
// built again (a restart shows every text). The WinMX 3.x ".lng" format is not read.
#pragma once

#include <QtCore/QString>

namespace mx {
class Settings;
}

namespace mx::ui::translation {

// Installs the translation in `qmPath` on the application (replacing an earlier one). An empty path
// removes it. Returns false (and keeps English) when the file is not a Qt translation.
bool apply(const QString &qmPath, QString *error = nullptr);
// The file that is installed now (empty = English).
QString current();
// True when `path` is a Qt translation file that QTranslator can load.
bool isTranslationFile(const QString &path);
// Applies misc/languageFile now and on every change of the setting.
void follow(mx::Settings *settings);

} // namespace mx::ui::translation

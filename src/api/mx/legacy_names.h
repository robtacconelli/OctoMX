// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX [ren-1]: copies installed before 5.0 keep working where they are. Everything below names what
// such a copy left on a user's computer (the legacy names), so a profile, a download folder or a
// database written by it keeps working where it is. Rule: nothing of an existing user is moved, old
// locations are honoured where they exist, a new install only ever sees the current names. (The
// literals here are compatibility literals: tools/names-allowlist.txt.)
#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>

namespace mx {
class Settings;
}

namespace mx::legacy {

// legacy names (pre-5.0), kept for compatibility
inline constexpr const char *kLegacyAppName = "WinMX";              // QStandardPaths application name
inline constexpr const char *kLegacyDatabase = "winmx.db";          // <profile>/winmx.db
inline constexpr const char *kLegacyDownloadFolder = "WinMX";       // <Downloads>/WinMX
inline constexpr const char *kLegacyUpdateUrl = "https://api.github.com/repos/robtacconelli/WinMX-NG/releases/latest";
inline constexpr const char *kLegacyUserAgentPrefix = "WinMX NG/";  // the legacy http/userAgent default
inline constexpr const char *kLegacyFontFamily = "WinMX Tahoma";    // the legacy family of the bundled font
inline constexpr const char *kLegacyNetName = "WinMX";              // the legacy netName(Net::Overlay)
// current names
inline constexpr const char *kDatabase = "octomx.db";
inline constexpr const char *kDownloadFolder = "OctoMX";

// The legacy profile folder next to the new default one: <dir of newDefault>/WinMX (same parent:
// QStandardPaths::AppDataLocation of the application name "WinMX"). Empty when newDefault is empty.
QString legacyProfileDir(const QString &newDefault);
// The default profile: newDefault, unless it has no settings.json and the legacy folder next to it
// has one; then the legacy folder, as it is, at every start (nothing is moved or copied).
QString chooseProfileDir(const QString &newDefault);
// true when dir is one of the standard (not explicitly chosen) profile folders: newDefault or the
// legacy folder next to it
bool isStandardProfileDir(const QString &dir, const QString &newDefault);

// <profile>/octomx.db, or the legacy winmx.db when that exists and octomx.db does not
// (a SQLite file is never renamed: its -wal / -journal side files would be orphaned)
QString databaseFile(const QString &profileDir);

// the default download folder in downloadsDir: the legacy <Downloads>/WinMX when that folder exists
// (an existing user's downloads stay where they are) and <Downloads>/OctoMX does not, else
// <Downloads>/OctoMX
QString defaultIncomingDir(const QString &downloadsDir);

// Stored settings that hold a legacy default follow the current one: updates/url equal to
// kLegacyUpdateUrl, an http/userAgent of the form kLegacyUserAgentPrefix + <version>, kLegacyNetName
// in search/networks, kLegacyFontFamily in ui/font*. Values the user chose stay. Returns the keys it
// changed (for the log).
QStringList migrateSettings(Settings *s);

} // namespace mx::legacy

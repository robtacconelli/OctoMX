// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX core: "File Launching": open a file with the configured player (launch/*Command with
// %1 = path) or the OS default handler, and open a file's folder. No Qt Gui dependency (QProcess).
#pragma once

#include <QtCore/QString>

namespace mx {

class Settings;

// Returns false if nothing could be started. `settings` may be null (= OS default).
bool launchFile(Settings *settings, const QString &path);
bool openContainingFolder(const QString &path);   // selects the file where the platform supports it
// The command line that launchFile would run (program + arguments), for tests / logging.
QStringList launchCommand(Settings *settings, const QString &path);

} // namespace mx

// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX core: logging categories and the file log handler.
#pragma once

#include <QtCore/QLoggingCategory>
#include <QtCore/QString>

Q_DECLARE_LOGGING_CATEGORY(lcCore)      // "mx.core"
Q_DECLARE_LOGGING_CATEGORY(lcDb)        // "mx.db"
Q_DECLARE_LOGGING_CATEGORY(lcLibrary)   // "mx.library"
Q_DECLARE_LOGGING_CATEGORY(lcSearch)    // "mx.search"
Q_DECLARE_LOGGING_CATEGORY(lcNet)       // "mx.net"
Q_DECLARE_LOGGING_CATEGORY(lcChat)      // "mx.chat"
Q_DECLARE_LOGGING_CATEGORY(lcEngine)    // "mx.engine"

namespace mx {

// Install a Qt message handler that appends every message to <profileDir>/logs/octomx.log
// ("2026-11-20 19:00:01.123 I mx.core: text"), rotating to octomx.1.log ... octomx.<keep>.log when the
// file exceeds maxBytes. Messages are also forwarded to the previous handler (stderr) unless
// alsoStderr is false. Thread-safe. Call once from main() (the app), after QCoreApplication exists.
// minLevel: 0 = debug, 1 = info, 2 = warning (debug messages of mx.* are dropped below it).
void installLogHandler(const QString &profileDir, qint64 maxBytes = 5 * 1024 * 1024, int keep = 5,
                       bool alsoStderr = true, int minLevel = 1);
// Change the level later (e.g. when misc/logLevel changes): "warning" | "info" | "debug".
void setLogLevel(const QString &level);
// [ui-h] Settings > Interface and Logs "Maximum size" / "Old files kept" (misc/logMaxSizeMB,
// misc/logKeepFiles): change the rotation limits of the installed handler (at least 64 KB, 1 file).
void setLogLimits(qint64 maxBytes, int keep);
qint64 logMaxBytes();
int logKeepFiles();
// Flush and detach the handler (restores the previous one).
void uninstallLogHandler();
// Path of the current log file (empty if not installed).
QString logFilePath();

} // namespace mx

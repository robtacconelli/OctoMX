// SPDX-License-Identifier: GPL-3.0-or-later
// [safe-1] The ways the system asks a program to end, turned into one queued `requested(reason)`
// on the main thread, so they all run the program's safe quit (downloads written and synced first):
//   Linux / macOS: SIGTERM (kill, system shutdown, an updater), SIGINT (Ctrl+C in a terminal),
//                  SIGHUP (the terminal closed)
//   Windows:       console Ctrl+C / Ctrl+Break / console closed / logoff / shutdown
// (the Windows end-session messages of a GUI program arrive through Qt: see src/app/main.cpp).
// A second signal while the quit runs is ignored; on Windows the console handler waits until
// finished() (the system ends the process when the handler returns).
#pragma once

#include <QtCore/QObject>

namespace mx {

class QuitSignals : public QObject {
    Q_OBJECT
public:
    // One per process; later calls return the same object.
    static QuitSignals *install(QObject *parent = nullptr);
    static QuitSignals *instance();
    // The safe quit is done (a waiting console handler may return).
    void finished();
    // what a signal does (also for tests): requested(reason) once
    void deliver(const QString &reason);
    ~QuitSignals() override;

signals:
    void requested(const QString &reason);

private:
    explicit QuitSignals(QObject *parent);
    struct Private;
    Private *d = nullptr;
};

} // namespace mx

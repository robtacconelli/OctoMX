// SPDX-License-Identifier: GPL-3.0-or-later
// [safe-1] see mx/quit_signals.h
#include "mx/quit_signals.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QPointer>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <QtCore/QSocketNotifier>

#include <csignal>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace mx {

namespace {
QPointer<QuitSignals> g_instance;
#ifdef Q_OS_WIN
HANDLE g_done = nullptr;

BOOL WINAPI consoleHandler(DWORD type)
{
    QString why;
    switch (type) {
    case CTRL_C_EVENT: why = QStringLiteral("Ctrl+C"); break;
    case CTRL_BREAK_EVENT: why = QStringLiteral("Ctrl+Break"); break;
    case CTRL_CLOSE_EVENT: why = QStringLiteral("the console was closed"); break;
    case CTRL_LOGOFF_EVENT: why = QStringLiteral("logoff"); break;
    case CTRL_SHUTDOWN_EVENT: why = QStringLiteral("system shutdown"); break;
    default: return FALSE;
    }
    QuitSignals *q = g_instance.data();
    if (!q) return FALSE;
    QMetaObject::invokeMethod(q, [why] { if (QuitSignals *x = g_instance.data()) x->deliver(why); }, Qt::QueuedConnection);
    // the process ends when this returns for the last three: wait for the safe quit (the system's
    // own limit applies)
    if (type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT)
        if (g_done) WaitForSingleObject(g_done, 120000);
    return TRUE;
}
#else
int g_pipe[2] = {-1, -1};

void onSignal(int sig)
{
    const char c = char(sig);
    if (g_pipe[0] >= 0) {
        const ssize_t n = ::write(g_pipe[0], &c, 1);   // async-signal-safe
        (void)n;
    }
}
#endif
} // namespace

struct QuitSignals::Private {
    bool delivered = false;
#ifndef Q_OS_WIN
    QSocketNotifier *notifier = nullptr;
#endif
};

QuitSignals *QuitSignals::instance() { return g_instance.data(); }

QuitSignals *QuitSignals::install(QObject *parent)
{
    if (g_instance) return g_instance.data();
    auto *q = new QuitSignals(parent ? parent : QCoreApplication::instance());
    g_instance = q;
#ifdef Q_OS_WIN
    if (!g_done) g_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler(consoleHandler, TRUE);
#else
    if (g_pipe[0] < 0 && ::socketpair(AF_UNIX, SOCK_STREAM, 0, g_pipe) != 0) {
        g_pipe[0] = g_pipe[1] = -1;
        return q;
    }
    q->d->notifier = new QSocketNotifier(qintptr(g_pipe[1]), QSocketNotifier::Read, q);
    QObject::connect(q->d->notifier, &QSocketNotifier::activated, q, [q] {
        char c = 0;
        if (::read(g_pipe[1], &c, 1) != 1) return;
        const int sig = int(c);
        q->deliver(sig == SIGTERM ? QStringLiteral("SIGTERM") : sig == SIGINT ? QStringLiteral("SIGINT")
                                                              : sig == SIGHUP ? QStringLiteral("SIGHUP") : QStringLiteral("signal %1").arg(sig));
    });
    struct sigaction sa = {};
    sa.sa_handler = onSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    for (int s : {SIGTERM, SIGINT, SIGHUP}) sigaction(s, &sa, nullptr);
#endif
    return q;
}

QuitSignals::QuitSignals(QObject *parent) : QObject(parent), d(new Private) {}

QuitSignals::~QuitSignals()
{
#ifdef Q_OS_WIN
    SetConsoleCtrlHandler(consoleHandler, FALSE);
    if (g_done) SetEvent(g_done);
#else
    struct sigaction sa = {};
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    for (int s : {SIGTERM, SIGINT, SIGHUP}) sigaction(s, &sa, nullptr);
#endif
    delete d;
}

void QuitSignals::deliver(const QString &reason)
{
    if (d->delivered) return;   // the quit runs already
    d->delivered = true;
    emit requested(reason);
}

void QuitSignals::finished()
{
#ifdef Q_OS_WIN
    if (g_done) SetEvent(g_done);
#endif
}

} // namespace mx

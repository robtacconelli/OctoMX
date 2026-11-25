// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal HTTP/1.1 server for the HTTP plugin tests (runs in its own thread, plain HTTP).
// Scenarios are selected per request by path and query parameters (see test_server.cpp).
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QMutex>
#include <QtCore/QSemaphore>
#include <QtCore/QStringList>
#include <QtCore/QThread>

namespace mxtest {

// Deterministic content of generated files: byte i of the file with `seed`.
void generate(quint64 seed, qint64 offset, char *out, qint64 n);
QByteArray generated(quint64 seed, qint64 offset, qint64 n);
quint64 seedFor(const QByteArray &name, int version = 0);

struct RequestLog {
    QByteArray method, target, range, ifRange, auth, userAgent;
};

class TestServer : public QThread {
    Q_OBJECT
public:
    TestServer();
    ~TestServer() override;
    quint16 port() const { return m_port; }
    QByteArray base() const { return "http://127.0.0.1:" + QByteArray::number(m_port); }

    // thread-safe stats
    int count(const QByteArray &method, const QByteArray &pathPrefix) const;
    int maxConcurrent() const;
    int active() const;                           // response bodies being sent right now
    QByteArray timeline() const;                  // every body start and end since reset(), with its time
    QList<RequestLog> log() const;
    void reset();

    // used by the server thread
    void record(const RequestLog &r);
    int bumpPath(const QByteArray &key);          // returns the new count for key
    int pathCount(const QByteArray &key) const;
    void activeDelta(int d, const QByteArray &what = {});

protected:
    void run() override;

private:
    quint16 m_port = 0;
    QSemaphore m_ready;
    mutable QMutex m_mutex;
    QList<RequestLog> m_log;
    QHash<QByteArray, int> m_counts;
    int m_active = 0, m_maxActive = 0;
    QByteArray m_timeline;
    QElapsedTimer m_since;
};

} // namespace mxtest

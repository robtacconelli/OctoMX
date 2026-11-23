// SPDX-License-Identifier: GPL-3.0-or-later
// A tiny range scheduler that drives one mx::RangeSource the way the core download engine does:
// split the file into chunks of preferredRequestSize(), keep maxPendingRequests() in flight,
// write data() at its offset, re-queue failed chunks. Used by tst_http and the http_probe tool.
#pragma once

#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QHash>
#include <QtCore/QObject>

#include "http_plugin.h"
#include "mx/plugin.h"

namespace mxtest {

class MiniScheduler : public QObject {
    Q_OBJECT
public:
    MiniScheduler(mx::RangeSource *src, QFileDevice *out, qint64 size, qint64 chunk = 0, QObject *parent = nullptr)
        : QObject(parent), m_src(src), m_out(out), m_size(size), m_chunk(chunk)
    {
        connect(src, &mx::RangeSource::stateChanged, this, &MiniScheduler::fill);
        connect(src, &mx::RangeSource::data, this, &MiniScheduler::onData);
        connect(src, &mx::RangeSource::requestFinished, this, &MiniScheduler::onFinished);
    }

    void start()
    {
        m_clock.start();
        const qint64 chunk = m_chunk > 0 ? m_chunk : m_src->preferredRequestSize();
        if (m_size < 0) {
            m_todo << qMakePair(qint64(0), qint64(1) << 50);     // size unknown: one sequential request
        } else {
            for (qint64 off = 0; off < m_size; off += chunk) m_todo << qMakePair(off, qMin(chunk, m_size - off));
        }
        fill();
    }

    bool done = false, ok = false;
    QString error;
    qint64 received = 0;        // bytes written (including re-fetched ones)
    qint64 finalSize = -1;
    int maxInFlight = 0;
    int failures = 0;
    int outOfOrder = 0;
    qint64 maxChunk = 0;
    qint64 elapsedMs() const { return m_clock.elapsed(); }

signals:
    void finished(bool ok);

private:
    void fill()
    {
        if (done) return;
        const auto st = m_src->state();
        if (st == mx::RangeSource::Failed || st == mx::RangeSource::Closed) {
            finish(false, m_src->stateText());
            return;
        }
        if (st != mx::RangeSource::Ready && st != mx::RangeSource::Busy) return;
        while (!m_todo.isEmpty() && m_inflight.size() < m_src->maxPendingRequests()) {
            const auto c = m_todo.takeFirst();
            m_inflight.insert(c.first, c.first);
            maxInFlight = qMax(maxInFlight, int(m_inflight.size()));
            m_src->request(c.first, c.second);
            if (done) return;
        }
    }
    void onData(qint64 offset, const QByteArray &bytes)
    {
        // find the request this belongs to: the one whose next expected position == offset
        bool found = false;
        for (auto it = m_inflight.begin(); it != m_inflight.end(); ++it) {
            if (it.value() == offset) { it.value() += bytes.size(); found = true; break; }
        }
        if (!found) ++outOfOrder;
        maxChunk = qMax<qint64>(maxChunk, bytes.size());
        m_out->seek(offset);
        m_out->write(bytes);
        received += bytes.size();
    }
    void onFinished(qint64 offset, qint64 length, bool okReq, const QString &err)
    {
        m_inflight.remove(offset);
        if (okReq) {
            if (m_size < 0) { finalSize = offset + length; m_todo.clear(); }
        } else {
            ++failures;
            if (err.startsWith(QLatin1String(mx::kHttpErrorNoRanges)) || failures > 20) { finish(false, err); return; }
            m_todo.prepend(qMakePair(offset, length));
        }
        if (m_todo.isEmpty() && m_inflight.isEmpty()) { finish(true, QString()); return; }
        // re-schedule asynchronously (as an engine would, from its own loop)
        QMetaObject::invokeMethod(this, &MiniScheduler::fill, Qt::QueuedConnection);
    }
    void finish(bool success, const QString &err)
    {
        if (done) return;
        done = true;
        ok = success;
        error = err;
        if (finalSize < 0) finalSize = m_size;
        m_out->flush();
        emit finished(success);
    }

    mx::RangeSource *m_src;
    QFileDevice *m_out;
    qint64 m_size;
    qint64 m_chunk;
    QList<QPair<qint64, qint64>> m_todo;
    QHash<qint64, qint64> m_inflight;     // request offset → next expected data offset
    QElapsedTimer m_clock;
};

} // namespace mxtest

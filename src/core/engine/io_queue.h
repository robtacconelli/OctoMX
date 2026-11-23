// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX engine: serial task queues on a shared worker pool.
// Every store owns one IoQueue: its tasks (writes, reads, hashing, moves) run one after another
// on a pool thread, in submission order, so a verification task queued after a write always sees
// the written bytes. Results are posted back to a main-thread QObject with post().
#pragma once

#include <QtCore/QMutex>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QThreadPool>
#include <QtCore/QWaitCondition>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>

namespace mx::engine {

class IoQueue {
public:
    explicit IoQueue(QThreadPool *pool);
    ~IoQueue();                                     // waits for the running task (not the backlog)
    IoQueue(const IoQueue &) = delete;
    IoQueue &operator=(const IoQueue &) = delete;

    void run(std::function<void()> task);           // enqueue
    int pending() const;                             // queued + running
    // Wait until everything queued so far has run (bounded). True when drained.
    bool drain(int timeoutMs);
    // Drop tasks that have not started yet (shutdown / cancel); the running one finishes.
    void discardPending();
    // A shared "cancel" flag tasks may poll (set by discardPending and the destructor).
    const std::atomic<bool> *cancelFlag() const { return &m_state->cancel; }

    // Run fn on the main thread (in ctx's thread) unless ctx was destroyed meanwhile.
    template <typename F>
    static void post(QObject *ctx, F &&fn)
    {
        QPointer<QObject> guard(ctx);
        QMetaObject::invokeMethod(ctx, [guard, f = std::forward<F>(fn)]() mutable {
            if (guard) f();
        }, Qt::QueuedConnection);
    }

private:
    struct State {
        QMutex mutex;
        QWaitCondition idle;
        std::deque<std::function<void()>> tasks;
        bool running = false;
        std::atomic<bool> cancel{false};
    };
    void pump(const std::shared_ptr<State> &st);
    QThreadPool *m_pool;
    std::shared_ptr<State> m_state;
};

} // namespace mx::engine

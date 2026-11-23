// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/engine/io_queue.h"

#include <QtCore/QDeadlineTimer>

namespace mx::engine {

IoQueue::IoQueue(QThreadPool *pool) : m_pool(pool), m_state(std::make_shared<State>()) {}

IoQueue::~IoQueue()
{
    discardPending();
    QMutexLocker l(&m_state->mutex);
    QDeadlineTimer dl(3000);
    while (m_state->running && !dl.hasExpired()) m_state->idle.wait(&m_state->mutex, dl);
}

void IoQueue::run(std::function<void()> task)
{
    std::shared_ptr<State> st = m_state;
    bool start = false;
    {
        QMutexLocker l(&st->mutex);
        st->tasks.push_back(std::move(task));
        if (!st->running) {
            st->running = true;
            start = true;
        }
    }
    if (start) pump(st);
}

void IoQueue::pump(const std::shared_ptr<State> &st)
{
    m_pool->start([st] {
        for (;;) {
            std::function<void()> t;
            {
                QMutexLocker l(&st->mutex);
                if (st->tasks.empty()) {
                    st->running = false;
                    st->idle.wakeAll();
                    return;
                }
                t = std::move(st->tasks.front());
                st->tasks.pop_front();
            }
            t();
        }
    });
}

int IoQueue::pending() const
{
    QMutexLocker l(&m_state->mutex);
    return int(m_state->tasks.size()) + (m_state->running ? 1 : 0);
}

bool IoQueue::drain(int timeoutMs)
{
    QMutexLocker l(&m_state->mutex);
    QDeadlineTimer dl(timeoutMs);
    while (m_state->running && !dl.hasExpired()) m_state->idle.wait(&m_state->mutex, dl);
    return !m_state->running;
}

void IoQueue::discardPending()
{
    QMutexLocker l(&m_state->mutex);
    m_state->cancel = true;
    m_state->tasks.clear();
}

} // namespace mx::engine

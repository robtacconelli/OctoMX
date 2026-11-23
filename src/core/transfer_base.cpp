// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/transfer_base.h"
#include "core/upload_tracker.h"

namespace mx {

TransferServiceBase::TransferServiceBase(UploadTracker *uploads, QObject *parent)
    : TransferService(parent)
    , m_uploads(uploads)
{
    if (!m_uploads) return;
    connect(m_uploads, &UploadTracker::uploadAdded, this, &TransferService::transferAdded);
    connect(m_uploads, &UploadTracker::uploadChanged, this, &TransferService::transferChanged);
    connect(m_uploads, &UploadTracker::uploadRemoved, this, [this](const QString &id) { emit transferRemoved(id, true); });
}

QList<TransferInfo> TransferServiceBase::uploads() const
{
    return m_uploads ? m_uploads->uploads() : QList<TransferInfo>();
}

void TransferServiceBase::clearUpload(const QString &id)
{
    if (m_uploads) m_uploads->clearUpload(id);
}

void TransferServiceBase::cancelUpload(const QString &id)
{
    if (m_uploads) m_uploads->cancelUpload(id);
}

bool TransferServiceBase::startUploadNow(const QString &id)
{
    return m_uploads && m_uploads->startNow(id);
}

bool TransferServiceBase::setUploadPriority(const QString &id, int priority)
{
    return m_uploads && m_uploads->setPriority(id, priority);
}

bool TransferServiceBase::uploadTransfer(const QString &id, TransferInfo *out) const
{
    return m_uploads && m_uploads->upload(id, out);
}

} // namespace mx

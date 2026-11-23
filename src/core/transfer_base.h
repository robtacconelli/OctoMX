// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX core: the upload half of mx::TransferService, shared by every DownloadEngine
// implementation (stub or real). Forwards the UploadTracker's rows/signals as transferAdded/
// transferChanged/transferRemoved(id, true). The engine implements the download half only and
// should use uploadTransfer() inside its transfer(id).
#pragma once

#include "mx/services.h"

namespace mx {

class UploadTracker;

class TransferServiceBase : public TransferService {
    Q_OBJECT
public:
    TransferServiceBase(UploadTracker *uploads, QObject *parent = nullptr);

    QList<TransferInfo> uploads() const override;
    void clearUpload(const QString &id) override;
    void cancelUpload(const QString &id) override;
    bool startUploadNow(const QString &id) override;                  // [glue-2]
    bool setUploadPriority(const QString &id, int priority) override; // [glue-2]
    UploadTracker *uploadTracker() const { return m_uploads; }

protected:
    bool uploadTransfer(const QString &id, TransferInfo *out) const;
    UploadTracker *m_uploads;
};

} // namespace mx

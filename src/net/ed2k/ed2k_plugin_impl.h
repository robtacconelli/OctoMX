// SPDX-License-Identifier: GPL-3.0-or-later
// The ED2K NetworkPlugin facade over Ed2kCore (private to the module; tests include it).
#pragma once

#include "ed2k_core.h"

#include "mx/plugin.h"

#include <memory>

namespace mx::ed2k {

// The engine is shared by the ED2K and Kad facades and lives as long as either of them.
std::shared_ptr<Ed2kCore> makeSharedCore();

// openSource() for both facades: an ed2k-peer:// descriptor (or an ed2k:// link with user.address)
mx::RangeSource *openEd2kSource(Ed2kCore *core, const QString &downloadId, const mx::FileHashes &h, qint64 size,
                                const mx::SearchSource &src);

class Ed2kPlugin : public mx::NetworkPlugin {
    Q_OBJECT
public:
    explicit Ed2kPlugin(QObject *parent = nullptr);
    Ed2kPlugin(std::shared_ptr<Ed2kCore> core, QObject *parent = nullptr);
    ~Ed2kPlugin() override;

    mx::Net net() const override { return mx::Net::ED2K; }
    int capabilities() const override { return CanSearch | CanFindSources | CanFetchRanges | CanServe; }
    void init(const mx::PluginContext &ctx) override;
    void start() override;
    void stop() override;
    mx::NetworkStatus status() const override;

    QList<mx::ServerEntry> servers() const override;
    void addServer(const QString &address, const QString &name) override;
    void removeServer(const QString &id) override;
    void connectServer(const QString &id) override;
    void disconnectServer(const QString &id) override;
    int importServers(const QByteArray &data) override;   // [glue-2]

    void search(const mx::SearchQuery &q) override;
    void cancelSearch(quint32 queryId) override;
    void findSources(const QString &downloadId, const mx::FileHashes &h, qint64 size, const QString &name) override;
    void stopFindSources(const QString &downloadId) override;
    mx::RangeSource *openSource(const QString &downloadId, const mx::FileHashes &h, qint64 size,
                                const mx::SearchSource &src) override;
    void setSharedFiles(const QList<mx::SharedFile> &files) override;
    // [glue-3] upload queue control (forwarded to the shared UploadManager)
    bool startUploadNow(const QString &uploadId) override;
    bool setUploadPriority(const QString &uploadId, int priority) override;
    void cancelUpload(const QString &uploadId) override;
    // [diag-1]
    mx::PluginDiagnostics diagnostics() const override;
    void recheckConnectivity() override;
    void portMappingChanged(const mx::PortMapping &m) override;

    Ed2kCore *core() const { return m_core.get(); }   // shared with the Kad plugin and tests
    std::shared_ptr<Ed2kCore> sharedCore() const { return m_core; }

private:
    void wire();
    bool primary() const;   // forwards the engine-wide signals (uploads, hashes, log)

    std::shared_ptr<Ed2kCore> m_core;
    bool m_started = false;
    bool m_inited = false;
    bool m_reconnectedForMapping = false;   // [diag-1] once per session
};

} // namespace mx::ed2k

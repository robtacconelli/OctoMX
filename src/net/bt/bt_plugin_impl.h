// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX: BitTorrent plugin facade (main thread). Private to mxnet_bt.
#pragma once

#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QThread>
#include <QtCore/QThreadPool>
#include <QtCore/QTimer>

#include <atomic>
#include <memory>

#include "bt_plugin.h"
#include "bt_worker.h"

class QNetworkAccessManager;

namespace mx {
namespace bt {

class BtPlugin : public NetworkPlugin, public TorrentBackend, public BitTorrentControl, public WorkerSink {
    Q_OBJECT
public:
    explicit BtPlugin(QObject *parent = nullptr);
    ~BtPlugin() override;

    // NetworkPlugin
    Net net() const override { return Net::BitTorrent; }
    int capabilities() const override { return CanServe; }
    void init(const PluginContext &ctx) override;
    void start() override;
    void stop() override;
    void prepareStop(bool closeNow) override;   // [safe-1]
    bool savePending() const override;
    NetworkStatus status() const override { return m_status; }
    QList<ServerEntry> servers() const override;
    void addServer(const QString &address, const QString &name) override;
    void removeServer(const QString &id) override;
    void connectServer(const QString &id) override;
    void setSharedFiles(const QList<SharedFile> &files) override;
    void cancelUpload(const QString &uploadId) override;
    // [diag-1]
    PluginDiagnostics diagnostics() const override { return m_sessionDiag; }
    void recheckConnectivity() override;
    void portMappingChanged(const PortMapping &m) override;
    TorrentDiag torrentDiagnostics(const QString &id) const override { return m_torrentDiag.value(id); }

    // TorrentBackend
    QString addTorrent(const QString &uri, const QByteArray &torrentFile, const QString &saveDir, bool paused,
                       const QList<int> &wantedFiles, QString *error) override;
    void removeTorrent(const QString &id, bool deleteFiles) override;
    void pauseTorrent(const QString &id) override;
    void resumeTorrent(const QString &id) override;
    void setWantedFiles(const QString &id, const QList<int> &indexes) override;
    void setTorrentPriority(const QString &id, int priority) override;
    void recheck(const QString &id) override;
    bool moveStorage(const QString &id, const QString &newSaveDir) override;   // [engine-3]
    void reannounce(const QString &id) override;
    QStringList torrents() const override { return m_order; }
    TorrentSnapshot snapshot(const QString &id) const override { return m_snap.value(id); }
    QByteArray pieceHash(const QString &id, int piece) const override;
    void reservePieces(const QString &id, const QList<int> &pieces, bool reserve) override;
    bool setDownloadOrder(const QString &id, bool sequential, bool firstLast) override;   // [dl-1]
    bool setStreamPieces(const QString &id, const QList<QPair<int, int>> &pieceDeadlines, bool restrict, int mode) override;   // [str-1]
    void setStreamCacheDir(const QString &dir) override;   // [str-1]
    void injectPiece(const QString &id, int piece, const QByteArray &data) override;
    bool seedLocalFile(const QString &path, QString *error) override;
    QObject *torrentEvents() override { return m_events; }

    // BitTorrentControl
    void setRateLimits(int downBps, int upBps) override;
    int downloadRateLimit() const override { return m_appliedDown; }
    int uploadRateLimit() const override { return m_appliedUp; }
    void addPeer(const QString &id, const QString &endpoint) override;

    // WorkerSink (called on the main thread)
    void wTorrentAdded(const TorrentSnapshot &snap, const QString &seededPath) override;
    void wTorrentMetadata(const QString &id, const QByteArray &pieceHashes, const TorrentSnapshot &snap) override;
    void wSnapshots(const QList<TorrentSnapshot> &snaps) override;
    void wPieces(const QString &id, const QList<int> &finished, const QList<int> &failed) override;
    void wTorrentFinished(const TorrentSnapshot &snap) override;
    void wTorrentRemoved(const QString &id) override;
    void wTorrentError(const QString &id, const QString &text) override;
    void wStorageMoved(const QString &id, const QString &path, bool ok, const QString &error) override;
    void wStatus(const NetworkStatus &st, int dhtNodes) override;
    void wUploads(const QList<TransferInfo> &changed, const QStringList &finished) override;
    void wLog(const QString &text) override;
    void wAccount(qint64 wireDown, qint64 wireUp, qint64 payloadDown, qint64 payloadUp) override;
    void wWaiting(qint64 down, qint64 up) override;   // [bw-2]
    void wAccountDiscovery(qint64 down, qint64 up) override;   // [bw-2]
    void wDiag(const QHash<QString, TorrentDiag> &torrents, const PluginDiagnostics &session) override;

private:
    template <typename F> void toWorker(F &&fn);
    template <typename F> void toWorkerBlocking(F &&fn);
    QString addParams(std::shared_ptr<lt::add_torrent_params> atp, Record rec, const QByteArray &torrentBytes,
                      const QList<int> &wanted, QString *error);
    void fetchUrl(const QString &url, const QString &saveDir, bool paused, const QList<int> &wanted);
    void onSeedHashed(const QString &path, const QByteArray &torrent, const QString &error);
    QString defaultSaveDir() const;
    void loadNodes();
    void saveNodes();
    void reloadConfig();
    void applyCtxToConfig();   // [diag-1] client name / version, central port mapping, announce port
    void pollRates();
    void noteChanged(const QString &id);
    void flushChanged();

    PluginContext m_ctx;
    QString m_dataDir;
    Config m_cfg;
    QString m_streamDir;   // [str-1] stream caches: sparse, not persisted
    bool m_inited = false;
    bool m_started = false;
    bool m_quitPrepared = false, m_quitHurry = false;   // [safe-1]
    QThread *m_thread = nullptr;
    Worker *m_worker = nullptr;
    BitTorrentEvents *m_events = nullptr;
    NetworkStatus m_status;
    QHash<QString, TorrentSnapshot> m_snap;
    QStringList m_order;
    QHash<QString, QByteArray> m_pieceHashes;
    QHash<QString, QString> m_seedPathToId;      // canonical path → id
    QSet<QString> m_hashing;                      // canonical paths being hashed
    QList<SharedFile> m_lastShared;               // [net-3] the Library's last list
    QSet<QString> m_seedWanted;                   // [net-3] canonical paths that may be seeded (bt/seedLibrary)
    bool m_haveShared = false;
    QHash<QString, QString> m_libraryDupes;       // [net-3] shared file -> the download torrent that is the same
    QSet<QString> m_confirmed;                    // ids the worker confirmed (torrentAdded emitted)
    QSet<QString> m_metaEmitted;
    bool m_restoring = false;
    QStringList m_userNodes, m_userTrackers;
    QHash<QString, qint64> m_lastChanged;
    QSet<QString> m_dirty;
    QElapsedTimer m_clock;
    QTimer m_changedTimer, m_cfgTimer, m_rateTimer;
    bool m_manualRates = false;
    int m_appliedDown = -1, m_appliedUp = -1;
    QNetworkAccessManager *m_nam = nullptr;
    // [diag-1]
    PluginDiagnostics m_sessionDiag;
    QHash<QString, TorrentDiag> m_torrentDiag;
    int m_announcePort = 0;
    QThreadPool m_hashPool;
    std::shared_ptr<std::atomic<bool>> m_cancelHashing = std::make_shared<std::atomic<bool>>(false);
};

} // namespace bt
} // namespace mx

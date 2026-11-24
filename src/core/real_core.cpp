// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/real_core.h"
#include "core/bandwidth.h"
#include "core/chat_manager.h"
#include "core/dao.h"
#include "core/diag/diagnostics_manager.h"
#include "core/database.h"
#include "core/engine/engine.h"
#include "core/json_settings.h"
#include "core/library.h"
#include "core/log.h"
#include "core/network_manager.h"
#include "core/pinger.h"
#include "core/rate_limiter.h"
#include "core/search_manager.h"
#include "core/search_merge.h"
#include "core/upload_tracker.h"
#include "core/user_manager.h"

#include "mx/app_paths.h"
#include "mx/legacy_names.h"
#include "mx/settings_keys.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QRandomGenerator>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>

namespace mx {

bool RealCore::isolatedProfile(const QString &profileDir)
{
    if (!qEnvironmentVariableIsEmpty("MX_OS_ROOT")) return true;
    const QByteArray explicitProfile = qgetenv("MX_PROFILE_EXPLICIT");
    if (!explicitProfile.isEmpty() && explicitProfile != "0") return true;
    const QString p = QDir::cleanPath(QDir(profileDir).absolutePath());
    // [ren-1] the standard folder (<AppData>/OctoMX) and the legacy one next to it (<AppData>/WinMX)
    const QString def = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (legacy::isStandardProfileDir(p, def)) return false;
    const QString exeDir = paths::portableDir();   // [mac-1] beside the app bundle on macOS
    if (!exeDir.isEmpty() && QFileInfo::exists(QDir(exeDir).filePath(QStringLiteral("portable.txt")))
        && p == QDir::cleanPath(QDir(exeDir).filePath(QStringLiteral("profile"))))
        return false;
    return true;
}

Core *createRealCore(const QString &profileDir, const QList<NetworkPlugin *> &plugins, QObject *parent)
{
    return new RealCore(profileDir, plugins, parent);
}

RealCore::RealCore(const QString &profileDir, const QList<NetworkPlugin *> &plugins, QObject *parent)
    : Core(parent)
    , m_profile(QDir::cleanPath(QDir(profileDir).absolutePath()))
{
    registerMetaTypes();
    QDir().mkpath(m_profile);

    // ---- settings
    m_settings = new JsonSettings(m_profile + QStringLiteral("/settings.json"), this);
    m_settings->load();
    // [core-5] ~/Downloads/OctoMX only for a normal start; an explicit / test profile keeps its
    // downloads inside itself (a scripted run once wrote into the owner's Downloads folder).
    // [ren-1] the legacy ~/Downloads/WinMX stays the default where it exists, while the setting is not stored.
    // MX_DOWNLOADS_DIR (tests) stands for the Downloads folder of an isolated profile.
    const bool isolated = isolatedProfile(m_profile);
    QString downloads = isolated ? QString() : QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (!qEnvironmentVariableIsEmpty("MX_DOWNLOADS_DIR")) downloads = qEnvironmentVariable("MX_DOWNLOADS_DIR");
    m_settings->setRuntimeDefault(keys::transfers::IncomingDir,
                                  downloads.isEmpty() ? m_profile + QStringLiteral("/incoming")
                                                      : legacy::defaultIncomingDir(downloads));
    // [ren-1] stored legacy defaults (update address, user agent, network and font names) follow the current ones
    const QStringList renamed = legacy::migrateSettings(m_settings);
    if (!renamed.isEmpty()) qCInfo(lcCore) << "settings that held an earlier default now follow the current one:" << renamed;
    m_settings->setRuntimeDefault(keys::transfers::IncompleteDir, m_profile + QStringLiteral("/incomplete"));
    if (m_settings->s(keys::internal::InstallId).isEmpty()) {
        m_settings->setValue(keys::internal::InstallId,
                             QString::number(QRandomGenerator::global()->generate64(), 16));
        // A new profile gets its own BitTorrent and rats-search ports: many providers filter the
        // well-known ones (6881-6889, 4444) for incoming connections. A port the user or a test
        // has already chosen is kept.
        for (const QString &key : {QString(keys::bt::ListenPort), QString(keys::rats::ListenPort)}) {
            if (m_settings->value(key) == m_settings->defaultValue(key))
                m_settings->setValue(key, 20000 + int(QRandomGenerator::global()->bounded(40000)));
        }
    }
    setLogLevel(m_settings->s(keys::misc::LogLevel));
    setLogLimits(qint64(m_settings->i(keys::misc::LogMaxSizeMB)) * 1024 * 1024, m_settings->i(keys::misc::LogKeepFiles));   // [ui-h]

    // ---- database
    m_db = new Database(this);
    QString err;
    // [ren-1] octomx.db; a legacy profile keeps its winmx.db under that name
    m_dbFile = legacy::databaseFile(m_profile);
    if (!m_db->open(m_dbFile, &err)) {
        qCCritical(lcCore) << "database unavailable:" << err;
        // keep going: DAOs log failures and return empty results
    }
    m_userDao = new UserListsDao(m_db);

    // ---- services
    m_limiter = new CoreRateLimiter(m_settings, this);
    m_library = new Library(m_settings, m_db, this);
    m_library->setSuggestHomeFolders(!isolated);
    m_network = new NetworkManager(m_settings, plugins, this);
    m_search = new SearchManager(m_settings, m_network, m_userDao, this);
    m_uploads = new UploadTracker(m_settings, this);
    m_uploads->setLibrary(m_library);
    for (NetworkPlugin *p : m_network->plugins()) m_uploads->attach(p);
    m_users = new UserManager(m_settings, m_network, m_userDao, this);
    m_chat = new ChatManager(m_settings, m_network, m_users, m_userDao, this);
    m_bandwidth = new CoreBandwidth(m_limiter, m_db, this);

    EngineContext ctx;
    ctx.settings = m_settings;
    ctx.db = m_db;
    ctx.library = m_library;
    ctx.limiter = m_limiter;
    ctx.uploads = m_uploads;
    ctx.plugins = m_network->plugins();
    ctx.profileDir = m_profile;
    ctx.search = m_search;   // [engine-4] downloads follow their search hit; background size+name searches
    for (NetworkPlugin *p : ctx.plugins) {
        if (auto *tb = dynamic_cast<TorrentBackend *>(p)) {
            ctx.torrents = tb;
            ctx.torrentPlugin = p;
            break;
        }
    }
    m_engine = new DownloadEngine(ctx, this);
    // [diag-1] Connection Test + the shared port mapper
    m_diag = new DiagnosticsManager(m_settings, m_network, m_engine, m_limiter, m_profile, this);
    m_diag->setTorrentBackend(ctx.torrents);

    // ---- wiring
    connect(m_library, &Library::sharedFilesUpdated, m_network, &NetworkManager::setSharedFiles);
    m_network->setSourceCounter([this] { return m_engine->knownSourceCount(); });
    m_network->setSecondaryCounter([this] {   // [engine-4] S=: sources in contact + uploads
        int n = m_engine->connectedSourceCount();
        for (const TransferInfo &u : m_uploads->uploads())
            if (u.state == TransferState::Transferring || u.state == TransferState::Queued || u.state == TransferState::Connecting) ++n;
        return n;
    });
    m_search->setLocalFlagger([this](SearchHit &h) { flagHit(h); });
    // [glue-2] pings (search results, users) and the own-searches-per-minute figure
    auto *pinger = new Pinger(m_network, this);
    m_network->setPinger(pinger);
    m_search->setPinger(pinger);
    connect(m_search, &SearchManager::searchStarted, m_network, &NetworkManager::noteOwnSearch);
    m_chat->setTransferPeerCheck([this](const UserRef &u, bool downloadingFrom) {
        if (downloadingFrom) {
            for (const TransferInfo &t : m_engine->downloads())
                for (const TransferSource &s : t.sources)
                    if (s.user.key() == u.key()) return true;
            return false;
        }
        for (const TransferInfo &t : m_uploads->uploads())
            if (t.peer.key() == u.key()) return true;
        return false;
    });
    connect(m_network, &NetworkService::overallChanged, this, [this] { emit statusMessage(m_network->overallHeadline()); });
    connect(m_settings, &Settings::changed, this, [this](const QString &key, const QVariant &v) {
        if (key == keys::misc::LogLevel) setLogLevel(v.toString());
        else if (key == keys::misc::LogMaxSizeMB || key == keys::misc::LogKeepFiles)   // [ui-h]
            setLogLimits(qint64(m_settings->i(keys::misc::LogMaxSizeMB)) * 1024 * 1024, m_settings->i(keys::misc::LogKeepFiles));
    });
}

RealCore::~RealCore()
{
    if (m_started && !m_shutDown) {
        // emergency path (no shutdown() called): still persist what we can
        m_diag->shutdown(500);
        m_network->stopAll();
        m_engine->shutdown();
        m_library->stop(2000);
        m_bandwidth->shutdown();
        m_settings->sync();
        m_db->close();
    }
    // explicit reverse-order teardown: plugins and services may use settings/db in their destructors
    delete m_diag;
    delete m_engine;
    delete m_chat;
    delete m_users;
    delete m_uploads;
    delete m_search;
    delete m_network;     // deletes the plugins
    delete m_library;
    delete m_bandwidth;
    delete m_limiter;
    delete m_userDao;
    delete m_db;
    delete m_settings;
}

Settings *RealCore::settings() { return m_settings; }
NetworkService *RealCore::network() { return m_network; }
SearchService *RealCore::search() { return m_search; }
TransferService *RealCore::transfers() { return m_engine; }
LibraryService *RealCore::library() { return m_library; }
ChatService *RealCore::chat() { return m_chat; }
UserService *RealCore::users() { return m_users; }
BandwidthService *RealCore::bandwidth() { return m_bandwidth; }
DiagnosticsService *RealCore::diagnostics() { return m_diag; }

void RealCore::start()
{
    if (m_started) return;
    m_started = true;
    QElapsedTimer t;
    t.start();
    qCInfo(lcCore) << "OctoMX" << MX_VERSION_STRING << "starting, profile" << m_profile << "database" << QFileInfo(m_dbFile).fileName() << "with"
                   << m_network->plugins().size() << "network plugins";
    if (!m_db->isOpen()) m_db->open(m_dbFile);
    m_network->initPlugins(m_profile, m_library, m_limiter);
    m_users->start();
    m_chat->start();
    m_library->start();
    m_bandwidth->start();
    m_network->startNetworks();
    m_engine->start();
    m_diag->start();
    if (!m_settings->b(keys::internal::FirstRunDone)) m_settings->setValue(keys::internal::FirstRunDone, true);
    m_settings->setValue(keys::internal::LastVersion, QStringLiteral(MX_VERSION_STRING));
    qCInfo(lcCore) << "core started in" << t.elapsed() << "ms";
    emit statusMessage(m_network->overallHeadline());
}

void RealCore::shutdown()
{
    if (m_shutDown) {
        QTimer::singleShot(0, this, &Core::shutdownFinished);
        return;
    }
    m_shutDown = true;
    QElapsedTimer t;
    t.start();
    if (m_started) {
        m_engine->shutdown();
        m_diag->shutdown(2000);   // [diag-1] remove the router mappings while the sockets still exist
        m_network->stopAll();
        m_library->stop(2500);
        m_bandwidth->shutdown();
    }
    m_settings->sync();
    m_db->close();
    qCInfo(lcCore) << "core shut down in" << t.elapsed() << "ms";
    QTimer::singleShot(0, this, &Core::shutdownFinished);
}

void RealCore::flagHit(SearchHit &h) const
{
    SharedFile f;
    if (!h.hashes.isEmpty() && m_library->findByHash(h.hashes, &f)) h.alreadyInLibrary = true;
    const QString norm = SearchMerger::normalizedName(h.fileName);
    for (const TransferInfo &d : m_engine->downloadsBrief()) {   // [engine-4] no source rows needed
        if (d.state == TransferState::Completed || d.state == TransferState::Cancelled
            || d.state == TransferState::Seeding)
            continue;
        const bool byHash = d.hashes.matches(h.hashes);
        const bool byName = !byHash && !d.hashes.conflicts(h.hashes) && d.size == h.size && h.size >= 1024 * 1024
                            && SearchMerger::normalizedName(d.name) == norm;
        if (!byHash && !byName) continue;
        h.matchesIncompleteLocal = true;
        if (d.state != TransferState::Paused && d.state != TransferState::Error && d.state != TransferState::TimedOut)
            h.canJoinRunningDownload = true;
        if (byHash && d.state == TransferState::Transferring) h.currentlyDownloading = true;
    }
    // [ui-j] partial data of downloads removed with "keep the file": downloading the hit continues it
    if (m_engine->matchesKept(h)) h.matchesIncompleteLocal = true;
}

} // namespace mx

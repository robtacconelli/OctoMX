// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX core: the real mx::Core facade (created by mx::createRealCore, see mx/services.h).
//
// Construction (cheap, synchronous): creates <profile>, loads <profile>/settings.json, opens
// <profile>/octomx.db (or the winmx.db of a legacy profile; migrating it) and builds every service, so the UI may connect signals and
// read settings/lists before start(). Plugins are owned by the NetworkManager.
// start():    init all plugins, start the library scan, restore downloads, start the enabled
//             networks (network/connectOnStart), start bandwidth sampling.
// shutdown(): engine → plugins → library → bandwidth totals → settings → DB; emits
//             shutdownFinished (queued): well within the 5 s budget.
#pragma once

#include <QtCore/QPointer>

#include "mx/plugin.h"
#include "mx/services.h"

namespace mx {

class JsonSettings;
class Database;
class UserListsDao;
class CoreRateLimiter;
class Library;
class NetworkManager;
class SearchManager;
class UploadTracker;
class DownloadEngine;
class UserManager;
class ChatManager;
class CoreBandwidth;
class DiagnosticsManager;
namespace stream { class StreamService; }

class RealCore : public Core {
    Q_OBJECT
public:
    RealCore(const QString &profileDir, const QList<NetworkPlugin *> &plugins, QObject *parent = nullptr);
    ~RealCore() override;
    // [core-5] True when this profile must not reach into the user's real folders: MX_OS_ROOT is set
    // (tests, scripted runs), MX_PROFILE_EXPLICIT is set (the app got --profile), or the profile is
    // neither the default per-user data folder nor the portable "profile" folder next to the
    // program. Then every default folder (incoming, incomplete) lies inside the profile and no
    // home folders are suggested for sharing.
    static bool isolatedProfile(const QString &profileDir);

    Settings *settings() override;
    NetworkService *network() override;
    SearchService *search() override;
    TransferService *transfers() override;
    LibraryService *library() override;
    ChatService *chat() override;
    UserService *users() override;
    BandwidthService *bandwidth() override;
    DiagnosticsService *diagnostics() override;   // [diag-1]
    QString profileDir() const override { return m_profile; }
    QString versionString() const override { return QStringLiteral(MX_VERSION_STRING); }
    void start() override;
    void shutdown() override;
    void beginShutdown() override;             // [safe-1]
    bool shutdownBusy() const override;
    qint64 shutdownBytesLeft() const override;
    void closeNow() override;
    bool lastStopWasUnclean() const override;
    StreamApiState streamApiState() const override;   // [str-1]

    // internals for the app / tests
    Database *database() const { return m_db; }
    Library *libraryImpl() const { return m_library; }
    NetworkManager *networkManager() const { return m_network; }
    DownloadEngine *engine() const { return m_engine; }
    CoreRateLimiter *rateLimiter() const { return m_limiter; }
    DiagnosticsManager *diagnosticsImpl() const { return m_diag; }
    stream::StreamService *streamService() const { return m_streams; }   // [str-1]
    bool isStarted() const { return m_started; }

private:
    void flagHit(SearchHit &h) const;

    QString m_profile;
    QString m_dbFile;   // [ren-1] octomx.db, or the winmx.db a legacy profile has
    JsonSettings *m_settings = nullptr;
    Database *m_db = nullptr;
    UserListsDao *m_userDao = nullptr;
    CoreRateLimiter *m_limiter = nullptr;
    Library *m_library = nullptr;
    NetworkManager *m_network = nullptr;
    SearchManager *m_search = nullptr;
    UploadTracker *m_uploads = nullptr;
    DownloadEngine *m_engine = nullptr;
    UserManager *m_users = nullptr;
    ChatManager *m_chat = nullptr;
    CoreBandwidth *m_bandwidth = nullptr;
    DiagnosticsManager *m_diag = nullptr;   // [diag-1]
    stream::StreamService *m_streams = nullptr;   // [str-1] the local streaming API
    bool m_started = false;
    bool m_shutDown = false;
    bool m_closing = false;    // [safe-1] beginShutdown() ran
};

} // namespace mx

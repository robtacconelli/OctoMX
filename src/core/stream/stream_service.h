// SPDX-License-Identifier: GPL-3.0-or-later
// [str-1] OctoMX core: the local streaming API (docs/API.md). Off by default (api/enabled).
//
// Another program searches every network, reads the results, selects one (a file of a bundle) and
// gets a stream link: http://<host>:<port>/stream/<id>/<key>/<file name>, plain HTTP with Range,
// for any player. The engine fetches what the players read, when they read it (the stream
// download, see core/engine/download_stream.cpp); the bytes live in a cache below
// <incomplete>/Streams that is dropped when the stream stops, at the next start, by the cache limit
// and by the idle time (or kept as a normal download: api/keepStreams).
//
// Safety: 127.0.0.1 and ::1 only unless api/allowLan (then also the private addresses of this
// computer, never a public one); an access token on every API call (Authorization: Bearer), a key
// derived from it in every stream link; Host header checked (DNS rebinding), no CORS, calls from
// web pages (an Origin header) refused, only GET / HEAD / POST / DELETE, size, connection and time
// limits (HttpServer). The API reads and writes nothing but its streams: no settings, no shared
// folders, no other files.
#pragma once

#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QSet>
#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QTimer>

#include <memory>

#include "core/engine/range_set.h"
#include "core/stream/http_server.h"
#include "mx/plugin.h"
#include "mx/services.h"

namespace mx {

class DownloadEngine;

namespace stream {

class StreamReader;

class StreamService : public QObject {
    Q_OBJECT
public:
    struct Tuning {
        int     pollMs = 100;                      // read positions to the engine, waiting players
        int     bufferSec = 30;                    // read-ahead: seconds of playing ahead of each read position
        qint64  minBuffer = qint64(4) << 20;       // ... at least (and for an unknown bit rate)
        qint64  maxBuffer = qint64(64) << 20;      // ... at most
        int     urgentSec = 4;                     // the part right after a read position: deadlines
        int     stallNoSourceMs = 60000;           // a player waits this long and no source is there: closed
        int     stallMs = 300000;                  // a player waits this long in any case: closed
        int     metaWaitMs = 300000;               // the size / file list of a torrent: wait this long (the stall time)
        int     maxSearches = 16;
        int     maxStreams = 8;
        int     endedKeepMs = 10 * 60 * 1000;      // an ended stream's status stays readable this long
        int     idleMinuteMs = 60000;              // one minute of api/idleMin (tests shrink it)
        int     releaseMs = 20000;                 // no player connected this long: the sources are let go
        int     pausedMs = 3000;                   // a player that took nothing this long has paused
    };

    StreamService(Settings *settings, SearchService *search, DownloadEngine *engine, NetworkService *network,
                  TorrentBackend *torrents, const QString &profileDir, QObject *parent = nullptr);
    ~StreamService() override;

    void start();                 // after the engine started: old caches go, the API listens if switched on
    void shutdown();              // before the engine shuts down: streams end (dropped or kept)
    StreamApiState state() const;
    void setTuning(const Tuning &t) { m_tune = t; }
    const Tuning &tuning() const { return m_tune; }
    HttpServer *server() { return &m_http; }

    // ---- for the HTTP handler and tests
    void handle(const HttpRequest &req, HttpResponse &resp);
    QString streamKey(const QString &streamId) const;    // the access key in a stream link
    static QString newToken();

signals:
    void stateChanged();

private:
    friend class StreamReader;
    struct Search {
        quint32 api = 0, sid = 0;
        SearchQuery query;
        bool typeFilter = false;
        qint64 createdMs = 0;
    };
    struct Reader;
    struct Stream {
        QString id, downloadId, name, mime, hitKey;
        quint32 search = 0;
        int file = -1;                 // bundle file index (-1 = the single file)
        bool owned = false;            // the stream created its download (its cache is its own)
        bool normal = false;           // "download it normally" was asked
        QString cacheDir;
        qint64 size = -1;
        qint64 bitrate = 0;            // bytes/s from the search result's duration (0 = unknown)
        qint64 lastRate = 0;           // the rate of the last player that left (a seek: the next one)
        qint64 createdMs = 0, lastPlayerMs = 0;
        qint64 lastReadMs = 0;         // a player took bytes
        qint64 dropAtMs = -1;          // the idle time ends then (-1 = a player reads)
        bool ended = false;
        qint64 endedMs = 0;
        QString state = QStringLiteral("waiting");
        QString error;
        QString endReason;
        QList<StreamReader *> readers;
        int nextReader = 1;
        qint64 served = 0;             // bytes sent to players
        qint64 unverifiedServed = 0;   // ... before any check could prove them
        engine::RangeSet unverifiedSet;
        qint64 failedServed = 0;       // unverified bytes sent that failed their check later
        bool overLimit = false;
        QString torrentId;             // to delete its folder once libtorrent let it go
        qint64 longestWaitMs = 0;
    };

    void applySettings();
    void listenNow();
    void tick();                  // pollMs: reads to the engine; 1 s: status texts, idle, limits
    void housekeeping();
    void cleanLeftovers();
    void endStream(Stream *s, const QString &reason, bool allowKeep = true);
    void deleteCacheLater(const QString &dir, const QString &torrentId, int attempt = 0);
    Stream *streamById(const QString &id) const;
    Search *searchById(quint32 id) const;
    QJsonObject streamJson(const Stream *s) const;
    QJsonObject cacheJson() const;   // every stream's cache against the limit
    QJsonObject hitJson(const Search *se, const SearchHit &h, bool full) const;
    QList<SearchHit> searchHits(const Search *se) const;
    static QString hitKeyOf(const SearchHit &h);
    void pushReads(Stream *s);
    void readerGone(StreamReader *r);
    QString linkFor(const Stream *s, const QByteArray &host) const;
    QByteArray hostFor(const HttpRequest &req) const;
    bool hostAllowed(const QByteArray &host) const;
    bool tokenOk(const QByteArray &given) const;
    // routes
    void apiCall(const HttpRequest &req, HttpResponse &resp);
    void streamCall(const HttpRequest &req, HttpResponse &resp, const QByteArray &id, const QByteArray &key);
    void selectResult(const HttpRequest &req, HttpResponse &resp, const QJsonObject &body);

    Settings *m_settings;
    SearchService *m_search;
    DownloadEngine *m_engine;
    NetworkService *m_network;
    TorrentBackend *m_torrents;
    QString m_profile;
    Tuning m_tune;
    HttpServer m_http;
    QTimer m_timer;
    QElapsedTimer m_clock;
    qint64 m_lastHouseMs = 0;
    bool m_started = false;
    bool m_listening = false;
    QString m_error;
    QHash<quint32, Search *> m_searches;
    quint32 m_nextSearch = 1;
    QList<Stream *> m_streams;
    QStringList m_allowedHosts;
    QSet<uint> m_askedFiles;     // bundles whose file list a select call asked for once
};

} // namespace stream
} // namespace mx

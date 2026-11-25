// SPDX-License-Identifier: GPL-3.0-or-later
// [str-1] OctoMX core: the local streaming API (see stream_service.h and docs/API.md).
#include "core/stream/stream_service.h"

#include "core/download_request.h"
#include "core/engine/download.h"
#include "core/engine/engine.h"
#include "core/log.h"
#include "mx/download_order.h"
#include "mx/settings_keys.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QMessageAuthenticationCode>
#include <QtCore/QRandomGenerator>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtNetwork/QNetworkInterface>

#include <algorithm>

namespace mx::stream {

using engine::Download;
using engine::Range;
using engine::RangeSet;

namespace {

QString mimeFor(const QString &name)
{
    static const QHash<QString, QString> m = {
        {QStringLiteral("mp4"), QStringLiteral("video/mp4")},       {QStringLiteral("m4v"), QStringLiteral("video/mp4")},
        {QStringLiteral("mkv"), QStringLiteral("video/x-matroska")}, {QStringLiteral("webm"), QStringLiteral("video/webm")},
        {QStringLiteral("avi"), QStringLiteral("video/x-msvideo")},  {QStringLiteral("mov"), QStringLiteral("video/quicktime")},
        {QStringLiteral("mpg"), QStringLiteral("video/mpeg")},       {QStringLiteral("mpeg"), QStringLiteral("video/mpeg")},
        {QStringLiteral("ts"), QStringLiteral("video/mp2t")},        {QStringLiteral("m2ts"), QStringLiteral("video/mp2t")},
        {QStringLiteral("ogv"), QStringLiteral("video/ogg")},        {QStringLiteral("wmv"), QStringLiteral("video/x-ms-wmv")},
        {QStringLiteral("flv"), QStringLiteral("video/x-flv")},      {QStringLiteral("3gp"), QStringLiteral("video/3gpp")},
        {QStringLiteral("ogg"), QStringLiteral("audio/ogg")},        {QStringLiteral("oga"), QStringLiteral("audio/ogg")},
        {QStringLiteral("opus"), QStringLiteral("audio/opus")},      {QStringLiteral("mp3"), QStringLiteral("audio/mpeg")},
        {QStringLiteral("flac"), QStringLiteral("audio/flac")},      {QStringLiteral("m4a"), QStringLiteral("audio/mp4")},
        {QStringLiteral("aac"), QStringLiteral("audio/aac")},        {QStringLiteral("wav"), QStringLiteral("audio/wav")},
        {QStringLiteral("wma"), QStringLiteral("audio/x-ms-wma")},
    };
    return m.value(QFileInfo(name).suffix().toLower(), QStringLiteral("application/octet-stream"));
}

QString typeName(MediaType t)
{
    switch (t) {
    case MediaType::Audio: return QStringLiteral("audio");
    case MediaType::Video: return QStringLiteral("video");
    case MediaType::Image: return QStringLiteral("image");
    case MediaType::Document: return QStringLiteral("document");
    case MediaType::Software: return QStringLiteral("software");
    case MediaType::Archive: return QStringLiteral("archive");
    case MediaType::Other: return QStringLiteral("other");
    default: return QStringLiteral("any");
    }
}

void json(HttpResponse &r, int status, const QJsonObject &o)
{
    r.status = status;
    r.setHeader("Content-Type", "application/json; charset=utf-8");
    r.setHeader("Cache-Control", "no-store");
    r.setHeader("X-Content-Type-Options", "nosniff");
    r.body = QJsonDocument(o).toJson(QJsonDocument::Indented);
}

void error(HttpResponse &r, int status, const QString &code, const QString &message, const QJsonObject &extra = {})
{
    QJsonObject o = extra;
    o.insert(QStringLiteral("error"), code);
    o.insert(QStringLiteral("message"), message);
    json(r, status, o);
}

// equal in constant time (the length too)
bool sameBytes(const QByteArray &a, const QByteArray &b)
{
    const int n = qMax(a.size(), b.size());
    unsigned diff = unsigned(a.size() ^ b.size());
    for (int i = 0; i < n; ++i) {
        const unsigned x = i < a.size() ? uchar(a.at(i)) : 0u;
        const unsigned y = i < b.size() ? uchar(b.at(i)) : 0u;
        diff |= x ^ y;
    }
    return diff == 0 && n > 0;
}

bool privateAddress(const QHostAddress &a)
{
    if (a.isLoopback()) return false;
    if (a.protocol() == QAbstractSocket::IPv4Protocol) {
        const quint32 v = a.toIPv4Address();
        return (v >> 24) == 10 || (v >> 20) == ((172u << 4) | 1u) || (v >> 16) == ((192u << 8) | 168u);
    }
    if (a.protocol() == QAbstractSocket::IPv6Protocol) {
        const Q_IPV6ADDR x = a.toIPv6Address();
        return (x[0] & 0xfe) == 0xfc;   // unique local fc00::/7 (link-local needs a scope: not used)
    }
    return false;
}

QString humanBytes(qint64 n)
{
    if (n < 1024 * 1024) return QStringLiteral("%1 KB").arg(n / 1024);
    if (n < (qint64(1) << 30)) return QStringLiteral("%1 MB").arg(double(n) / (1 << 20), 0, 'f', 1);
    return QStringLiteral("%1 GB").arg(double(n) / (qint64(1) << 30), 0, 'f', 2);
}

// "bytes=a-b" / "bytes=a-" / "bytes=-n": [start, end) of size; 0 = no (usable) range, -1 = unsatisfiable
int parseRange(const QByteArray &h, qint64 size, qint64 *start, qint64 *end)
{
    const QByteArray v = h.trimmed();
    if (!v.startsWith("bytes=")) return 0;
    const QByteArray spec = v.mid(6).trimmed();
    if (spec.contains(',')) return 0;   // several ranges: the whole file instead (allowed by RFC 9110)
    const int dash = spec.indexOf('-');
    if (dash < 0) return 0;
    const QByteArray a = spec.left(dash).trimmed(), b = spec.mid(dash + 1).trimmed();
    bool okA = true, okB = true;
    if (a.isEmpty()) {
        const qint64 n = b.toLongLong(&okB);
        if (!okB || n <= 0) return okB ? -1 : 0;
        *start = qMax<qint64>(0, size - n);
        *end = size;
        return size > 0 ? 1 : -1;
    }
    const qint64 s = a.toLongLong(&okA);
    const qint64 e = b.isEmpty() ? size - 1 : b.toLongLong(&okB);
    if (!okA || !okB || s < 0) return 0;
    if (s >= size) return -1;
    if (e < s) return 0;
    *start = s;
    *end = qMin(size, e + 1);
    return 1;
}

QString asciiName(const QString &n)
{
    QString o;
    for (QChar c : n) o += (c.unicode() >= 32 && c.unicode() < 127 && c != QLatin1Char('"') && c != QLatin1Char('\\')) ? c : QLatin1Char('_');
    return o;
}

} // namespace

// ============================================================================ a player's read

class StreamReader : public HttpBody {
public:
    StreamReader(StreamService *svc, const QString &stream, int id, qint64 pos, qint64 end)
        : m_svc(svc), m_stream(stream), m_id(id), m_start(pos), m_pos(pos), m_end(end)
    {
        m_openedMs = svc->m_clock.elapsed();
    }
    ~StreamReader() override
    {
        if (m_svc) m_svc->readerGone(this);
    }
    qint64 produce(QByteArray &out, qint64 max) override;

    QPointer<StreamService> m_svc;
    QString m_stream;
    int m_id = 0;
    qint64 m_start = 0, m_pos = 0, m_end = 0;   // file coordinates
    qint64 m_sent = 0;
    qint64 m_openedMs = 0;
    qint64 m_firstByteMs = -1;                 // time to the first byte of this read
    qint64 m_waitSinceMs = -1;                 // waiting for bytes since (-1 = not waiting)
    qint64 m_longestWaitMs = 0;                // after the first byte
    bool m_aborted = false;
    engine::RateMeter m_rate;
    QFile m_file;
    QString m_path;
};

qint64 StreamReader::produce(QByteArray &out, qint64 max)
{
    if (!m_svc || m_aborted) return -2;
    if (m_pos >= m_end) return -1;
    StreamService::Stream *s = m_svc->streamById(m_stream);
    if (!s || s->ended) return -2;
    Download *d = m_svc->m_engine->downloadObject(s->downloadId);
    const qint64 now = m_svc->m_clock.elapsed();
    if (!d) {
        s->error = QStringLiteral("The download of this stream is gone (removed in the Transfers window).");
        return -2;
    }
    const Range span = d->streamSpan();
    Download::StreamServe sv = d->streamServable(span.first + m_pos);
    QByteArray data;
    if (sv.avail > 0 && !sv.path.isEmpty()) {
        if (m_path != sv.path || !m_file.isOpen()) {
            m_file.close();
            m_file.setFileName(sv.path);
            m_path = sv.path;
            // unbuffered: a buffer read ahead would hold bytes that were not written yet
            if (!m_file.open(QIODevice::ReadOnly | QIODevice::Unbuffered)) m_path.clear();
        }
        const qint64 n = qMin(qMin(sv.avail, m_end - m_pos), max);
        if (m_file.isOpen() && m_file.seek(span.first + m_pos - sv.fileBase)) data = m_file.read(n);
    }
    if (data.isEmpty()) {
        if (m_waitSinceMs < 0) m_waitSinceMs = now;
        const qint64 waited = now - m_waitSinceMs;
        const StreamService::Tuning &t = m_svc->m_tune;
        const bool noSource = d->connectedSources() == 0;
        if ((noSource && waited > t.stallNoSourceMs) || waited > t.stallMs) {
            const QString where = humanBytes(m_pos);
            s->error = noSource ? QStringLiteral("A player waited %1 s for the bytes at %2 and no source has them: the connection was closed. "
                                                 "A retry of the player continues when a source is found.").arg(waited / 1000).arg(where)
                                : QStringLiteral("A player waited %1 s for the bytes at %2: the connection was closed. A retry continues.")
                                      .arg(waited / 1000).arg(where);
            qCWarning(lcCore).noquote() << "stream" << s->id << s->error;
            return -2;
        }
        return 0;
    }
    if (m_waitSinceMs >= 0) {
        if (m_firstByteMs >= 0) {
            m_longestWaitMs = qMax(m_longestWaitMs, now - m_waitSinceMs);
            s->longestWaitMs = qMax(s->longestWaitMs, m_longestWaitMs);
        }
        m_waitSinceMs = -1;
    }
    if (m_firstByteMs < 0) m_firstByteMs = now - m_openedMs;
    s->lastReadMs = now;
    if (sv.unverified) {
        s->unverifiedServed += data.size();
        s->unverifiedSet.add(span.first + m_pos, span.first + m_pos + data.size());
    }
    m_pos += data.size();
    m_sent += data.size();
    s->served += data.size();
    m_rate.add(engine::nowMs(), data.size());
    out += data;
    return data.size();
}

// ============================================================================ service

StreamService::StreamService(Settings *settings, SearchService *search, DownloadEngine *engine, NetworkService *network,
                             TorrentBackend *torrents, const QString &profileDir, QObject *parent)
    : QObject(parent), m_settings(settings), m_search(search), m_engine(engine), m_network(network), m_torrents(torrents),
      m_profile(profileDir)
{
    m_clock.start();
    m_http.setHandler([this](const HttpRequest &req, HttpResponse &resp) { handle(req, resp); });
    connect(&m_http, &HttpServer::connectionsChanged, this, &StreamService::stateChanged);
    m_timer.setInterval(m_tune.pollMs);
    connect(&m_timer, &QTimer::timeout, this, &StreamService::tick);
    if (m_settings)
        connect(m_settings, &Settings::changed, this, [this](const QString &key, const QVariant &) {
            if (!m_started) return;
            if (key == keys::api::Enabled || key == keys::api::Port || key == keys::api::AllowLan) applySettings();
            else if (key == keys::api::Token) emit stateChanged();
        });
    if (m_torrents)
        if (auto *ev = qobject_cast<TorrentEvents *>(m_torrents->torrentEvents()))
            connect(ev, &TorrentEvents::torrentPieceFinished, this, [this](const QString &, int) {
                // a piece is there: the players waiting for it get it now, not at the next poll
                for (Stream *s : std::as_const(m_streams))
                    if (!s->ended) pushReads(s);
            });
}

StreamService::~StreamService()
{
    for (Stream *s : std::as_const(m_streams))
        for (StreamReader *r : std::as_const(s->readers)) r->m_svc = nullptr;
    m_http.close();
    qDeleteAll(m_streams);
    qDeleteAll(m_searches);
}

QString StreamService::newToken()
{
    QByteArray b(24, Qt::Uninitialized);
    QRandomGenerator *g = QRandomGenerator::system();
    for (int i = 0; i < b.size(); ++i) b[i] = char(g->bounded(256));
    return QString::fromLatin1(b.toHex());
}

QString StreamService::streamKey(const QString &streamId) const
{
    const QByteArray token = m_settings ? m_settings->s(keys::api::Token).toUtf8() : QByteArray();
    return QString::fromLatin1(QMessageAuthenticationCode::hash(("stream:" + streamId).toUtf8(), token, QCryptographicHash::Sha256)
                                   .toHex()
                                   .left(32));
}

void StreamService::start()
{
    if (m_started) return;
    m_started = true;
    const QString root = m_engine->streamCacheRoot();
    QDir().mkpath(root);
    if (m_torrents) m_torrents->setStreamCacheDir(root);
    cleanLeftovers();
    QTimer::singleShot(15000, this, [this] { cleanLeftovers(); });   // torrents the backend restored late
    m_timer.start();
    applySettings();
}

void StreamService::shutdown()
{
    if (!m_started) return;
    m_timer.stop();
    for (Stream *s : std::as_const(m_streams))
        if (!s->ended) endStream(s, QStringLiteral("OctoMX was closed"));
    m_http.close();
    m_listening = false;
    for (Search *se : std::as_const(m_searches)) m_search->closeSearch(se->sid);
    qDeleteAll(m_searches);
    m_searches.clear();
    m_started = false;
}

// every cache below <incomplete>/Streams that no download of the list uses: a stream of an
// earlier session (or a crash) leaves nothing behind
void StreamService::cleanLeftovers()
{
    const QString root = QDir::cleanPath(m_engine->streamCacheRoot());
    QStringList used;
    QSet<QString> live;
    for (const Stream *s : std::as_const(m_streams))
        if (!s->ended) live.insert(s->downloadId);
    for (const TransferInfo &t : m_engine->downloads()) {
        used << QDir::cleanPath(t.savePath);
        for (const QString &p : m_engine->diskInfo(t.id).paths) used << QDir::cleanPath(p);
    }
    if (m_torrents) {
        for (const QString &tid : m_torrents->torrents()) {
            const QString sp = QDir::cleanPath(m_torrents->snapshot(tid).savePath);
            if (!sp.startsWith(root + QLatin1Char('/'))) continue;
            bool mine = false;
            for (const TransferInfo &t : m_engine->downloadsBrief())
                if (QString::fromLatin1(t.hashes.btih.toHex()) == tid) mine = true;
            if (!mine) {
                qCInfo(lcCore) << "streaming: a torrent of an earlier stream leaves the session" << tid.left(8);
                m_torrents->removeTorrent(tid, true);
            }
        }
    }
    const QFileInfoList entries = QDir(root).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    for (const QFileInfo &fi : entries) {
        const QString p = QDir::cleanPath(fi.absoluteFilePath());
        bool inUse = false;
        for (const QString &u : std::as_const(used))
            if (u == p || u.startsWith(p + QLatin1Char('/'))) inUse = true;
        for (const QString &id : std::as_const(live))
            if (fi.fileName() == id) inUse = true;
        if (inUse) continue;
        qCInfo(lcCore) << "streaming: dropping the cache of an earlier stream" << fi.fileName();
        if (fi.isDir() && !fi.isSymLink()) QDir(p).removeRecursively();
        else QFile::remove(p);
    }
}

void StreamService::applySettings()
{
    const bool on = m_settings && m_settings->b(keys::api::Enabled);
    if (on && m_settings->s(keys::api::Token).isEmpty()) m_settings->setValue(keys::api::Token, newToken());
    if (!on) {
        if (m_listening) qCInfo(lcCore) << "streaming API switched off";
        m_http.close();
        m_listening = false;
        m_error.clear();
        emit stateChanged();
        return;
    }
    listenNow();
}

void StreamService::listenNow()
{
    QList<QHostAddress> addrs{QHostAddress(QHostAddress::LocalHost), QHostAddress(QHostAddress::LocalHostIPv6)};
    if (m_settings->b(keys::api::AllowLan))
        for (const QHostAddress &a : QNetworkInterface::allAddresses())
            if (privateAddress(a) && !addrs.contains(a)) addrs << a;
    const int port = m_settings->i(keys::api::Port);
    QString err;
    bool inUse = false;
    m_allowedHosts.clear();
    if (!m_http.listen(addrs, quint16(port), &err, &inUse)) {
        m_listening = false;
        m_error = inUse ? QStringLiteral("Port %1 is in use by another program: the streaming API does not listen. "
                                         "Choose another port.").arg(port)
                        : err;
        qCWarning(lcCore).noquote() << "streaming API:" << m_error;
        emit stateChanged();
        return;
    }
    m_listening = true;
    m_error.clear();
    QStringList shown;
    for (const QHostAddress &a : m_http.addresses()) {
        const QString h = a.protocol() == QAbstractSocket::IPv6Protocol ? QLatin1Char('[') + a.toString() + QLatin1Char(']') : a.toString();
        m_allowedHosts << (h + QLatin1Char(':') + QString::number(m_http.port())).toLower();
        shown << a.toString();
    }
    m_allowedHosts << QStringLiteral("localhost:%1").arg(m_http.port());
    qCInfo(lcCore).noquote() << "streaming API listening on" << shown.join(QStringLiteral(", ")) << "port" << m_http.port();
    emit stateChanged();
}

StreamApiState StreamService::state() const
{
    StreamApiState st;
    st.available = true;
    st.enabled = m_settings && m_settings->b(keys::api::Enabled);
    st.listening = m_listening;
    st.port = m_listening ? m_http.port() : (m_settings ? m_settings->i(keys::api::Port) : 0);
    for (const QHostAddress &a : m_http.addresses()) st.addresses << a.toString();
    st.baseUrl = QStringLiteral("http://127.0.0.1:%1/api/v1/").arg(st.port);
    st.error = m_error;
    for (const Stream *s : m_streams)
        if (!s->ended) {
            ++st.streams;
            st.players += int(s->readers.size());
        }
    return st;
}

StreamService::Stream *StreamService::streamById(const QString &id) const
{
    for (Stream *s : m_streams)
        if (s->id == id) return s;
    return nullptr;
}

StreamService::Search *StreamService::searchById(quint32 id) const { return m_searches.value(id); }

// ---------------------------------------------------------------------------------- the polls

void StreamService::tick()
{
    for (Stream *s : std::as_const(m_streams))
        if (!s->ended) pushReads(s);
    const qint64 now = m_clock.elapsed();
    if (now - m_lastHouseMs >= 1000) {
        m_lastHouseMs = now;
        housekeeping();
    }
}

void StreamService::pushReads(Stream *s)
{
    Download *d = m_engine->downloadObject(s->downloadId);
    if (!d || !d->streaming()) return;
    const Range span = d->streamSpan();
    if (span.second <= span.first) return;
    struct R { Download::StreamRead r; bool waiting; qint64 ahead; };
    QList<R> rs;
    const qint64 now = engine::nowMs();
    for (StreamReader *x : std::as_const(s->readers)) {
        if (x->m_aborted || x->m_pos >= x->m_end) continue;
        // the bit rate: the search result's, else what this player read in the last 30 s
        const qint64 rate = qMax(qMax<qint64>(s->bitrate, s->lastRate), qint64(x->m_rate.rate(now, 30)));
        const qint64 buffer = qBound(m_tune.minBuffer, rate * m_tune.bufferSec, m_tune.maxBuffer);
        const qint64 urgent = qBound<qint64>(512 * 1024, rate * m_tune.urgentSec, qint64(4) << 20);
        R r;
        r.r.pos = span.first + x->m_pos;
        r.r.end = qMin(span.first + x->m_end, r.r.pos + buffer);
        r.r.urgentEnd = qMin(r.r.end, r.r.pos + urgent);
        r.waiting = x->m_waitSinceMs >= 0;
        r.r.waiting = r.waiting;
        r.ahead = d->streamServable(r.r.pos).avail;
        rs << r;
    }
    std::stable_sort(rs.begin(), rs.end(), [](const R &a, const R &b) {
        if (a.waiting != b.waiting) return a.waiting;
        return a.ahead < b.ahead;
    });
    QList<Download::StreamRead> reads;
    for (const R &r : std::as_const(rs)) reads << r.r;
    d->setStreamReads(reads);
}

void StreamService::readerGone(StreamReader *r)
{
    for (Stream *s : std::as_const(m_streams)) {
        if (!s->readers.removeOne(r)) continue;
        s->lastRate = qMax<qint64>(r->m_rate.rate(engine::nowMs(), 30), s->lastRate / 2);
        s->lastPlayerMs = m_clock.elapsed();
        if (!s->ended) pushReads(s);
        emit stateChanged();
        return;
    }
}

void StreamService::housekeeping()
{
    const qint64 now = m_clock.elapsed();
    const qint64 idleMs = qint64(m_settings ? m_settings->i(keys::api::IdleMin) : 5) * m_tune.idleMinuteMs;
    const qint64 limit = qint64(m_settings ? m_settings->i(keys::api::CacheLimitMB) : 4096) << 20;
    qint64 total = 0;
    QList<Stream *> idle;
    for (Stream *s : std::as_const(m_streams)) {
        if (s->ended) continue;
        Download *d = m_engine->downloadObject(s->downloadId);
        if (!d) {
            endStream(s, QStringLiteral("its download was removed in the Transfers window"), false);
            continue;
        }
        // unverified bytes a player got that are no longer held: their check failed (fetched again)
        if (!s->unverifiedSet.isEmpty() && s->torrentId == d->torrentId()) {
            RangeSet lost = s->unverifiedSet;
            lost.remove(d->streamServableSet());
            if (!lost.isEmpty()) {
                s->failedServed += lost.total();
                s->unverifiedSet.remove(lost);
                qCWarning(lcCore) << "stream" << s->id << lost.total() << "bytes a player got failed their check later";
            }
        }
        if (s->owned) total += d->streamDiskBytes();
        if (s->torrentId != d->torrentId()) {   // the part file became a torrent (promotion): other coordinates
            s->unverifiedSet.clear();
            s->torrentId = d->torrentId();
        }
        const bool players = !s->readers.isEmpty();
        bool waiting = false;
        qint64 ahead = -1, waitAt = -1;
        const Range span = d->streamSpan();
        for (StreamReader *r : std::as_const(s->readers)) {
            if (r->m_waitSinceMs >= 0) { waiting = true; waitAt = r->m_pos; }
            const qint64 a = d->streamServable(span.first + r->m_pos).avail;
            ahead = ahead < 0 ? a : qMin(ahead, a);
        }
        // no player connected for a while: its sources are let go (nothing is fetched anyway)
        d->setStreamIdle(!players && now - s->lastPlayerMs > m_tune.releaseMs);
        // the idle time counts from the moment the last player left, or took its last byte
        const bool tookNothing = players && !waiting && now - qMax(s->lastReadMs, s->lastPlayerMs) > m_tune.pausedMs;
        const qint64 idleSince = !players ? s->lastPlayerMs : (tookNothing ? qMax(s->lastReadMs, s->lastPlayerMs) : -1);
        s->dropAtMs = idleSince >= 0 ? idleSince + idleMs : -1;
        if (d->isPausedByUser()) s->state = QStringLiteral("paused in the Transfers window");
        else if (d->hasError()) s->state = QStringLiteral("error");
        else if (!players) s->state = s->served > 0 ? QStringLiteral("idle") : QStringLiteral("waiting for a player");
        else if (waiting) s->state = QStringLiteral("waiting for data");
        else if (tookNothing) s->state = QStringLiteral("paused");
        else s->state = QStringLiteral("playing");
        auto left = [&] {
            const qint64 sec = qMax<qint64>(0, (s->dropAtMs - now) / 1000);
            return sec >= 3600 ? QStringLiteral("%1 h %2 min").arg(sec / 3600).arg((sec % 3600) / 60)
                               : sec >= 60 ? QStringLiteral("%1 min").arg(sec / 60) : QStringLiteral("%1 s").arg(sec);
        };
        QString note;
        if (!players) {
            note = QStringLiteral("Streaming: idle, no player (the cache goes in %1)").arg(left());
        } else if (tookNothing) {
            note = QStringLiteral("Streaming: paused by the player, %1 buffered ahead (the cache goes in %2 if it stays paused)")
                       .arg(humanBytes(qMax<qint64>(0, ahead)), left());
        } else if (waiting) {
            note = QStringLiteral("Streaming: a player waits for the bytes at %1").arg(humanBytes(waitAt));
        } else {
            note = QStringLiteral("Streaming: %1, %2 buffered ahead").arg(countNoun(s->readers.size(), QStringLiteral("player")), humanBytes(qMax<qint64>(0, ahead)));
        }
        if (s->overLimit) note += QStringLiteral(", over the cache limit");
        if (d->streamTrustsReceived() && s->unverifiedServed > 0) note += QStringLiteral(", bytes served before their check");
        d->setStreamNote(note);
        if (!players && s->lastPlayerMs == 0) s->lastPlayerMs = s->createdMs;
        if ((s->dropAtMs >= 0 && now >= s->dropAtMs) || (!players && s->overLimit)) {
            endStream(s, s->overLimit && !players ? QStringLiteral("the stream caches were over their limit and no player was connected")
                                                  : QStringLiteral("no player read it for %1 min").arg(idleMs / 60000.0, 0, 'g', 4));
            continue;
        }
        if (!players) idle << s;
    }
    // the cache limit: idle streams go first (the oldest player first); one that is still played is
    // kept until its player stops
    if (total > limit) {
        std::sort(idle.begin(), idle.end(), [](const Stream *a, const Stream *b) { return a->lastPlayerMs < b->lastPlayerMs; });
        for (Stream *s : std::as_const(idle)) {
            if (total <= limit) break;
            Download *d = m_engine->downloadObject(s->downloadId);
            const qint64 b = d ? d->streamDiskBytes() : 0;
            endStream(s, QStringLiteral("the stream caches were over their limit (%1)").arg(humanBytes(limit)));
            total -= b;
        }
        for (Stream *s : std::as_const(m_streams))
            if (!s->ended && s->owned) s->overLimit = total > limit;
    } else {
        for (Stream *s : std::as_const(m_streams)) s->overLimit = false;
    }
    for (int i = int(m_streams.size()) - 1; i >= 0; --i) {
        Stream *s = m_streams.at(i);
        if (s->ended && s->readers.isEmpty() && now - s->endedMs > m_tune.endedKeepMs) {
            m_streams.removeAt(i);
            delete s;
        }
    }
    emit stateChanged();
}

void StreamService::endStream(Stream *s, const QString &reason, bool allowKeep)
{
    if (s->ended) return;
    for (StreamReader *r : std::as_const(s->readers)) r->m_aborted = true;
    Download *d = m_engine->downloadObject(s->downloadId);
    const bool keep = allowKeep && s->owned && m_settings && m_settings->b(keys::api::KeepStreams);
    if (d) {
        if (s->owned && keep) {
            d->endStream(true, m_engine->incomingDir());
        } else if (s->owned) {
            const QString tid = d->torrentId();
            m_engine->dropStream(s->downloadId);
            deleteCacheLater(s->cacheDir, tid);
        } else {
            d->endStream(false, QString());
        }
    } else if (s->owned) {
        deleteCacheLater(s->cacheDir, s->torrentId);
    }
    s->ended = true;
    s->endedMs = m_clock.elapsed();
    s->state = QStringLiteral("ended");
    s->endReason = keep ? reason + QStringLiteral("; it is a normal download now") : reason;
    qCInfo(lcCore).noquote() << "stream" << s->id << s->name << "ended:" << s->endReason << "; served" << s->served << "bytes";
    emit stateChanged();
}

void StreamService::deleteCacheLater(const QString &dir, const QString &torrentId, int attempt)
{
    const QString root = QDir::cleanPath(m_engine->streamCacheRoot());
    const QString d = QDir::cleanPath(dir);
    if (d.isEmpty() || !d.startsWith(root + QLatin1Char('/'))) return;   // never anything else
    // a torrent: libtorrent closes its files once it has left the session
    if (!torrentId.isEmpty() && m_torrents && m_torrents->torrents().contains(torrentId) && attempt < 40) {
        QTimer::singleShot(250, this, [this, dir, torrentId, attempt] { deleteCacheLater(dir, torrentId, attempt + 1); });
        return;
    }
    QTimer::singleShot(attempt == 0 ? 50 : 1000, this, [this, d, torrentId, attempt] {
        if (!QFileInfo::exists(d)) return;
        if (QDir(d).removeRecursively() || attempt >= 60) {
            if (QFileInfo::exists(d)) qCWarning(lcCore) << "streaming: cannot delete the cache" << d << "(it goes at the next start)";
            return;
        }
        deleteCacheLater(d, QString(), attempt + 1);   // a file still open (Windows): again in a moment
    });
}

// ---------------------------------------------------------------------------------- HTTP

QByteArray StreamService::hostFor(const HttpRequest &req) const
{
    QByteArray h = req.header("host").trimmed().toLower();
    if (h.isEmpty()) {
        const QHostAddress a = req.local;
        h = (a.protocol() == QAbstractSocket::IPv6Protocol ? "[" + a.toString().toLatin1() + "]" : a.toString().toLatin1()) + ':'
            + QByteArray::number(m_http.port());
    }
    return h;
}

bool StreamService::hostAllowed(const QByteArray &host) const
{
    if (host.isEmpty()) return true;   // a client without a Host header is no browser (DNS rebinding needs one)
    return m_allowedHosts.contains(QString::fromLatin1(host).toLower());
}

bool StreamService::tokenOk(const QByteArray &given) const
{
    const QByteArray token = m_settings ? m_settings->s(keys::api::Token).toUtf8() : QByteArray();
    if (token.isEmpty()) return false;
    return sameBytes(given, token);
}

void StreamService::handle(const HttpRequest &req, HttpResponse &resp)
{
    static const QList<QByteArray> methods{"GET", "HEAD", "POST", "DELETE"};
    if (!methods.contains(req.method)) {
        resp.setHeader("Allow", "GET, HEAD, POST, DELETE");
        error(resp, 405, QStringLiteral("method"), QStringLiteral("Only GET, HEAD, POST and DELETE."));
        return;
    }
    if (!req.header("host").isEmpty() && !hostAllowed(req.header("host").trimmed())) {
        error(resp, 403, QStringLiteral("host"), QStringLiteral("This Host name is not one of the API's addresses."));
        resp.close = true;
        return;
    }
    if (req.path.startsWith("/stream/")) {
        const QList<QByteArray> parts = req.path.mid(8).split('/');
        if (parts.size() < 2 || parts.at(0).isEmpty() || parts.at(1).isEmpty()) {
            error(resp, 401, QStringLiteral("unauthorized"), QStringLiteral("Unauthorized."));
            return;
        }
        streamCall(req, resp, parts.at(0), parts.at(1));
        return;
    }
    const QByteArray auth = req.header("authorization");
    if (!auth.startsWith("Bearer ") || !tokenOk(auth.mid(7).trimmed())) {
        resp.setHeader("WWW-Authenticate", "Bearer");
        error(resp, 401, QStringLiteral("unauthorized"), QStringLiteral("Unauthorized."));
        return;
    }
    // a web page cannot use the API: browsers send Origin on cross-origin calls, API clients do not
    if (!req.header("origin").isEmpty()) {
        error(resp, 403, QStringLiteral("web_page"), QStringLiteral("Calls from web pages are not accepted."));
        return;
    }
    if (!req.path.startsWith("/api/")) {
        error(resp, 404, QStringLiteral("not_found"), QStringLiteral("Not found: the API is under /api/v1/."));
        return;
    }
    apiCall(req, resp);
}

QString StreamService::hitKeyOf(const SearchHit &h)
{
    return QString::fromLatin1(QCryptographicHash::hash(h.key.toUtf8(), QCryptographicHash::Sha1).toHex().left(16));
}

QList<SearchHit> StreamService::searchHits(const Search *se) const
{
    QList<SearchHit> out;
    for (const SearchHit &h : m_search->hits(se->sid))
        if (!se->typeFilter || hitMatchesFileType(se->query, h)) out << h;   // the Search page's own filter
    std::stable_sort(out.begin(), out.end(), [](const SearchHit &a, const SearchHit &b) {
        const int pa = qMax(a.totalPeers, a.sourceCount), pb = qMax(b.totalPeers, b.sourceCount);
        return pa > pb;
    });
    return out;
}

QJsonObject StreamService::hitJson(const Search *se, const SearchHit &h, bool full) const
{
    Q_UNUSED(se)
    QJsonObject o;
    o.insert(QStringLiteral("key"), hitKeyOf(h));
    o.insert(QStringLiteral("name"), h.fileName);
    o.insert(QStringLiteral("size"), double(h.size));
    o.insert(QStringLiteral("type"), typeName(classifyHit(h)));
    QJsonArray nets;
    for (Net n : h.networks) nets << netName(n);
    o.insert(QStringLiteral("networks"), nets);
    o.insert(QStringLiteral("sources"), h.sourceCount);
    o.insert(QStringLiteral("peers"), h.totalPeers);
    o.insert(QStringLiteral("seeds"), h.seeds);
    o.insert(QStringLiteral("bundle"), h.isBundle);
    if (h.durationSec > 0) o.insert(QStringLiteral("durationSec"), h.durationSec);
    if (h.bitrateKbps > 0) o.insert(QStringLiteral("bitrateKbps"), h.bitrateKbps);
    if (!h.suspect.isEmpty()) o.insert(QStringLiteral("suspect"), h.suspect);
    if (h.alreadyInLibrary) o.insert(QStringLiteral("inLibrary"), true);
    if (h.canJoinRunningDownload) o.insert(QStringLiteral("downloading"), true);
    if (h.isBundle) {
        if (h.files.isEmpty()) {
            o.insert(QStringLiteral("files"), QJsonValue::Null);
            o.insert(QStringLiteral("streamable"), QJsonValue::Null);
        } else {
            QJsonArray fl;
            bool any = false;
            for (const BundleFile &f : h.files) {
                const bool media = preview::isMediaFile(f.path);
                any = any || media;
                if (!full && fl.size() >= 200) continue;
                fl << QJsonObject{{QStringLiteral("index"), f.index}, {QStringLiteral("path"), f.path},
                                  {QStringLiteral("size"), double(f.size)}, {QStringLiteral("streamable"), media}};
            }
            o.insert(QStringLiteral("files"), fl);
            o.insert(QStringLiteral("streamable"), any);
        }
    } else {
        o.insert(QStringLiteral("streamable"), preview::isMediaFile(h.fileName));
    }
    return o;
}

void StreamService::apiCall(const HttpRequest &req, HttpResponse &resp)
{
    const QByteArray p = req.path;
    if (!p.startsWith("/api/v1/") && p != "/api/v1") {
        error(resp, 404, QStringLiteral("version"), QStringLiteral("Unknown API version: this OctoMX speaks /api/v1/."));
        return;
    }
    const QList<QByteArray> seg = p.mid(8).split('/');
    const QByteArray what = seg.value(0);
    const QByteArray m = req.method == "HEAD" ? QByteArray("GET") : req.method;
    QJsonObject body;
    if (m == "POST") {
        const QByteArray ct = req.header("content-type").toLower();
        if (!ct.startsWith("application/json")) {
            error(resp, 415, QStringLiteral("content_type"), QStringLiteral("Send the body as JSON (Content-Type: application/json)."));
            return;
        }
        QJsonParseError pe;
        const QJsonDocument doc = QJsonDocument::fromJson(req.body, &pe);
        if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
            error(resp, 400, QStringLiteral("json"), QStringLiteral("The body is not a JSON object: %1.").arg(pe.errorString()));
            return;
        }
        body = doc.object();
    }
    const QUrlQuery query(QString::fromUtf8(req.query));
    auto wrongMethod = [&](const char *allow) {
        resp.setHeader("Allow", allow);
        error(resp, 405, QStringLiteral("method"), QStringLiteral("This call takes %1.").arg(QLatin1String(allow)));
    };

    // ---- GET /api/v1/status
    if (what == "status" && seg.size() == 1) {
        if (m != "GET") return wrongMethod("GET");
        QJsonObject o;
        o.insert(QStringLiteral("version"), QStringLiteral(MX_VERSION_STRING));
        o.insert(QStringLiteral("api"), QStringLiteral("on"));
        QJsonArray nets;
        for (const NetworkStatus &n : m_network->networks()) {
            static const char *states[] = {"disabled", "disconnected", "connecting", "connected", "error"};
            nets << QJsonObject{{QStringLiteral("name"), netName(n.net)}, {QStringLiteral("state"), QLatin1String(states[qBound(0, int(n.state), 4)])},
                                {QStringLiteral("peers"), n.peers}};
        }
        o.insert(QStringLiteral("networks"), nets);
        int live = 0;
        for (const Stream *s : std::as_const(m_streams)) live += s->ended ? 0 : 1;
        o.insert(QStringLiteral("streams"), live);
        o.insert(QStringLiteral("searches"), int(m_searches.size()));
        o.insert(QStringLiteral("cache"), cacheJson());
        json(resp, 200, o);
        return;
    }

    // ---- searches
    if (what == "searches") {
        if (seg.size() == 1) {
            if (m == "GET") {
                QJsonArray a;
                for (const Search *se : std::as_const(m_searches))
                    a << QJsonObject{{QStringLiteral("id"), int(se->api)}, {QStringLiteral("text"), se->query.text},
                                     {QStringLiteral("running"), m_search->isRunning(se->sid)}};
                json(resp, 200, QJsonObject{{QStringLiteral("searches"), a}});
                return;
            }
            if (m != "POST") return wrongMethod("GET, POST");
            const QString text = body.value(QStringLiteral("text")).toString().trimmed();
            const QString link = body.value(QStringLiteral("link")).toString().trimmed();
            if (text.isEmpty() && link.isEmpty()) {
                error(resp, 400, QStringLiteral("text"), QStringLiteral("Give \"text\" (keywords) or \"link\" (a magnet / ed2k link or a hash)."));
                return;
            }
            if (text.size() > 200 || link.size() > 4000) {
                error(resp, 400, QStringLiteral("text"), QStringLiteral("The search text is too long."));
                return;
            }
            if (int(m_searches.size()) >= m_tune.maxSearches) {
                error(resp, 429, QStringLiteral("busy"),
                      QStringLiteral("The API is busy: %1 searches are open. Delete one first (DELETE /api/v1/searches/<id>).").arg(m_tune.maxSearches));
                return;
            }
            auto *se = new Search;
            if (!link.isEmpty()) {
                se->query.text = link;
                se->sid = m_search->searchHash(link);
            } else {
                SearchQuery q;
                q.text = text;
                q.text2 = body.value(QStringLiteral("text2")).toString().trimmed().left(200);
                const QString type = body.value(QStringLiteral("type")).toString(QStringLiteral("any")).trimmed();
                const FileTypeFilter ft = FileTypeFilter::fromCode(type.isEmpty() ? QStringLiteral("any") : type);
                if (!ft.isValid()) {
                    delete se;
                    error(resp, 400, QStringLiteral("type"),
                          QStringLiteral("Unknown type \"%1\": one of %2.").arg(type, FileTypeFilter::kinds().join(QStringLiteral(", "))));
                    return;
                }
                ft.applyTo(q);
                se->typeFilter = ft.kind != QLatin1String("any");
                const QString ext = body.value(QStringLiteral("extension")).toString().trimmed().toLower();
                if (!ext.isEmpty()) {
                    q.extension = ext.startsWith(QLatin1Char('.')) ? ext.mid(1) : ext;
                    se->typeFilter = true;
                }
                q.minSize = qint64(body.value(QStringLiteral("minSize")).toDouble());
                q.maxSize = qint64(body.value(QStringLiteral("maxSize")).toDouble());
                for (const QJsonValue &v : body.value(QStringLiteral("networks")).toArray()) {
                    const Net n = netFromName(v.toString());
                    if (n == Net::Unknown) {
                        delete se;
                        error(resp, 400, QStringLiteral("networks"), QStringLiteral("Unknown network \"%1\".").arg(v.toString()));
                        return;
                    }
                    q.networks << n;
                }
                q.maxResults = m_settings->i(keys::search::MaxResults);
                if (q.maxResults <= 0) q.maxResults = 2000;
                se->query = q;
                se->sid = m_search->startSearch(q);
            }
            se->api = m_nextSearch++;
            se->createdMs = m_clock.elapsed();
            m_searches.insert(se->api, se);
            qCInfo(lcCore).noquote() << "streaming API: search" << se->api << "for" << se->query.text;
            json(resp, 201, QJsonObject{{QStringLiteral("id"), int(se->api)},
                                        {QStringLiteral("url"), QStringLiteral("/api/v1/searches/%1").arg(se->api)},
                                        {QStringLiteral("running"), true}});
            return;
        }
        bool ok = false;
        const quint32 id = seg.at(1).toUInt(&ok);
        Search *se = ok ? searchById(id) : nullptr;
        if (!se) {
            error(resp, 404, QStringLiteral("not_found"), QStringLiteral("No search with this id."));
            return;
        }
        if (seg.size() == 2) {
            if (m == "DELETE") {
                m_search->closeSearch(se->sid);
                m_searches.remove(id);
                delete se;
                json(resp, 200, QJsonObject{{QStringLiteral("deleted"), true}});
                return;
            }
            if (m != "GET") return wrongMethod("GET, DELETE");
            const QList<SearchHit> hits = searchHits(se);
            const int offset = qMax(0, query.queryItemValue(QStringLiteral("offset")).toInt());
            int limit = query.hasQueryItem(QStringLiteral("limit")) ? query.queryItemValue(QStringLiteral("limit")).toInt() : 100;
            limit = qBound(1, limit, 500);
            QJsonArray a;
            for (int i = offset; i < hits.size() && a.size() < limit; ++i) a << hitJson(se, hits.at(i), false);
            QJsonObject o{{QStringLiteral("id"), int(se->api)}, {QStringLiteral("text"), se->query.text},
                          {QStringLiteral("running"), m_search->isRunning(se->sid)}, {QStringLiteral("count"), int(hits.size())},
                          {QStringLiteral("offset"), offset}, {QStringLiteral("results"), a}};
            if (hits.isEmpty())
                o.insert(QStringLiteral("note"), m_search->isRunning(se->sid) ? QStringLiteral("No results yet: the networks are still being asked.")
                                                                               : QStringLiteral("The search found nothing."));
            json(resp, 200, o);
            return;
        }
        if (seg.size() == 4 && seg.at(2) == "results") {
            if (m != "GET") return wrongMethod("GET");
            const QString key = QString::fromLatin1(seg.at(3));
            for (const SearchHit &h : searchHits(se)) {
                if (hitKeyOf(h) != key) continue;
                QJsonObject o = hitJson(se, h, true);
                if (h.isBundle && h.files.isEmpty()) {
                    m_search->requestBundleFiles(se->sid, h.key);
                    o.insert(QStringLiteral("note"), QStringLiteral("The file list of this torrent was asked for: read this result again in a few seconds."));
                }
                json(resp, 200, o);
                return;
            }
            error(resp, 404, QStringLiteral("not_found"), QStringLiteral("No result with this key in this search."));
            return;
        }
        error(resp, 404, QStringLiteral("not_found"), QStringLiteral("Not found."));
        return;
    }

    // ---- streams
    if (what == "streams") {
        if (seg.size() == 1) {
            if (m == "POST") return selectResult(req, resp, body);
            if (m != "GET") return wrongMethod("GET, POST");
            QJsonArray a;
            for (const Stream *s : std::as_const(m_streams)) {
                QJsonObject o = streamJson(s);
                o.insert(QStringLiteral("url"), linkFor(s, hostFor(req)));
                a << o;
            }
            json(resp, 200, QJsonObject{{QStringLiteral("streams"), a}, {QStringLiteral("cache"), cacheJson()}});
            return;
        }
        Stream *s = seg.size() == 2 ? streamById(QString::fromLatin1(seg.at(1))) : nullptr;
        if (!s) {
            error(resp, 404, QStringLiteral("not_found"), QStringLiteral("No stream with this id."));
            return;
        }
        if (m == "DELETE") {
            const bool was = !s->ended;
            endStream(s, QStringLiteral("stopped through the API"));
            json(resp, 200, QJsonObject{{QStringLiteral("stopped"), was}, {QStringLiteral("state"), s->state},
                                        {QStringLiteral("message"), s->endReason}});
            return;
        }
        if (m != "GET") return wrongMethod("GET, DELETE");
        QJsonObject o = streamJson(s);
        o.insert(QStringLiteral("url"), linkFor(s, hostFor(req)));
        json(resp, 200, o);
        return;
    }
    error(resp, 404, QStringLiteral("not_found"), QStringLiteral("Not found."));
}

QString StreamService::linkFor(const Stream *s, const QByteArray &host) const
{
    QString name = s->name;
    name.replace(QLatin1Char('/'), QLatin1Char('_'));
    return QStringLiteral("http://%1/stream/%2/%3/%4")
        .arg(QString::fromLatin1(host), s->id, streamKey(s->id), QString::fromLatin1(QUrl::toPercentEncoding(name)));
}

void StreamService::selectResult(const HttpRequest &req, HttpResponse &resp, const QJsonObject &body)
{
    const quint32 sid = quint32(body.value(QStringLiteral("search")).toInt());
    const QString key = body.value(QStringLiteral("result")).toString();
    const bool normal = body.value(QStringLiteral("download")).toBool();
    Search *se = searchById(sid);
    if (!se) {
        error(resp, 404, QStringLiteral("not_found"), QStringLiteral("No search with this id (\"search\")."));
        return;
    }
    const QList<SearchHit> hits = searchHits(se);
    if (hits.isEmpty()) {
        error(resp, 409, QStringLiteral("search_empty"), m_search->isRunning(se->sid) ? QStringLiteral("The search has no results yet: it is still running.")
                                                                                     : QStringLiteral("The search found nothing."));
        return;
    }
    const SearchHit *hit = nullptr;
    for (const SearchHit &h : hits)
        if (hitKeyOf(h) == key) { hit = &h; break; }
    if (!hit) {
        error(resp, 404, QStringLiteral("not_found"),
              QStringLiteral("No result with this key (\"result\"). Results can merge while a search runs: read them again."));
        return;
    }
    SearchHit h = *hit;
    int file = body.contains(QStringLiteral("file")) ? body.value(QStringLiteral("file")).toInt(-1) : -1;
    QString name = h.fileName;
    qint64 size = h.size;
    bool filesUnknown = false;
    if (h.isBundle) {
        if (h.files.isEmpty() && body.value(QStringLiteral("waitForFiles")).toBool(true) && !body.contains(QStringLiteral("file"))
            && !m_askedFiles.contains(se->api * 1000003u + qHash(h.key))) {
            // asked once: the networks may know the file list; selecting again without it streams the
            // largest audio or video file once the torrent's own file list arrives
            m_askedFiles.insert(se->api * 1000003u + qHash(h.key));
            m_search->requestBundleFiles(se->sid, h.key);
            error(resp, 409, QStringLiteral("files_unknown"),
                  QStringLiteral("The file list of this torrent is not known yet. It was asked for: select again in a few seconds "
                                 "(without a file list the largest audio or video file is streamed once the torrent sends its list)."));
            return;
        }
        filesUnknown = h.files.isEmpty();
    }
    if (filesUnknown) {
        name = h.fileName;
    } else if (h.isBundle) {
        if (file < 0) {
            QList<int> media;
            for (const BundleFile &f : std::as_const(h.files))
                if (preview::isMediaFile(f.path)) media << f.index;
            if (media.size() != 1) {
                error(resp, 400, QStringLiteral("choose_file"),
                      QStringLiteral("This torrent holds %1 audio or video files: say which one (\"file\": its index).").arg(media.size()));
                return;
            }
            file = media.first();
        }
        const BundleFile *bf = nullptr;
        for (const BundleFile &f : std::as_const(h.files))
            if (f.index == file) bf = &f;
        if (!bf) {
            error(resp, 400, QStringLiteral("file"), QStringLiteral("This torrent has no file %1.").arg(file));
            return;
        }
        name = QFileInfo(bf->path).fileName();
        size = bf->size;
    } else {
        file = -1;
    }
    const bool media = filesUnknown || preview::isMediaFile(name);
    if (!media && !normal) {
        error(resp, 415, QStringLiteral("not_streamable"),
              QStringLiteral("'%1' is not an audio or video file: it cannot be streamed. Download it instead (\"download\": true).").arg(name));
        return;
    }
    if (h.sources.isEmpty()) {
        error(resp, 409, QStringLiteral("no_sources"), QStringLiteral("Nothing to stream: no network named a source of this result."));
        return;
    }
    int live = 0;
    for (const Stream *s : std::as_const(m_streams)) live += s->ended ? 0 : 1;
    // the reason a download did not start, in the engine's words
    QString notStarted;
    const QMetaObject::Connection c = connect(m_engine, &TransferService::downloadNotStarted, this,
                                              [&notStarted](const QString &, const QString &why) { notStarted = why; });
    QString id;
    auto start = [&] {
        if (h.isBundle) {
            DownloadRequest r = requestFromHit(h);
            if (file >= 0) r.wantedFiles = {file};   // else every file is wanted: the largest media file is streamed
            return m_engine->download(r);
        }
        return m_engine->downloadHit(h);
    };
    if (normal) {
        id = start();
    } else {
        if (live >= m_tune.maxStreams) {
            disconnect(c);
            error(resp, 429, QStringLiteral("busy"), QStringLiteral("The API is busy: %1 streams are open. Stop one first.").arg(live));
            return;
        }
        m_engine->setNextStream(file, m_engine->streamCacheRoot());
        id = start();
        m_engine->clearNextStream();
    }
    disconnect(c);
    Download *d = id.isEmpty() ? nullptr : m_engine->downloadObject(id);
    if (!d) {
        error(resp, 409, QStringLiteral("not_started"), notStarted.isEmpty() ? QStringLiteral("The transfer could not be started.") : notStarted);
        return;
    }
    if (!media) {   // a normal download of a file that cannot be streamed: no link
        json(resp, 201, QJsonObject{{QStringLiteral("download"), id}, {QStringLiteral("url"), QJsonValue::Null},
                                    {QStringLiteral("message"), QStringLiteral("Downloading normally; this type cannot be streamed.")}});
        return;
    }
    // the same download streamed already: the same stream
    for (Stream *s : std::as_const(m_streams)) {
        if (s->ended || s->downloadId != id || s->file != file) continue;
        if (normal && s->owned && d->streamRestricts()) d->endStream(true, m_engine->incomingDir());
        QJsonObject o = streamJson(s);
        o.insert(QStringLiteral("url"), linkFor(s, hostFor(req)));
        json(resp, 200, o);
        return;
    }
    if (!normal && !d->streaming() && live >= m_tune.maxStreams) {
        error(resp, 429, QStringLiteral("busy"), QStringLiteral("The API is busy: %1 streams are open. Stop one first.").arg(live));
        return;
    }
    auto *s = new Stream;
    {
        QByteArray b(6, Qt::Uninitialized);
        for (int i = 0; i < b.size(); ++i) b[i] = char(QRandomGenerator::system()->bounded(256));
        s->id = QString::fromLatin1(b.toHex());
    }
    s->downloadId = id;
    s->name = name;
    s->mime = mimeFor(name);
    s->hitKey = h.key;
    s->search = se->api;
    s->file = file;
    s->size = size;
    s->normal = normal;
    s->owned = d->streamRestricts();
    if (s->owned) s->cacheDir = QDir(m_engine->streamCacheRoot()).filePath(id);
    if (!d->streaming() && !d->isFinished()) d->setStream(true, false, file);   // joined a normal download: its order follows the player
    d->setStreamNote(QStringLiteral("Streaming: waiting for a player"));
    if (h.durationSec > 0 && !h.isBundle && size > 0) s->bitrate = size / h.durationSec;
    s->createdMs = s->lastPlayerMs = m_clock.elapsed();
    m_streams << s;
    qCInfo(lcCore).noquote() << "streaming API: stream" << s->id << name << (s->owned ? "(its own cache)" : "(of a download in the list)")
                             << "download" << id;
    QJsonObject o = streamJson(s);
    o.insert(QStringLiteral("url"), linkFor(s, hostFor(req)));
    json(resp, 201, o);
    emit stateChanged();
}

QJsonObject StreamService::cacheJson() const
{
    // the limit ends streams nobody reads; one that is played is never cut and can grow to its file's size
    const qint64 limit = qint64(m_settings ? m_settings->i(keys::api::CacheLimitMB) : 4096) << 20;
    qint64 total = 0;
    QJsonArray per;
    for (const Stream *s : m_streams) {
        if (s->ended || !s->owned) continue;
        Download *d = m_engine->downloadObject(s->downloadId);
        const qint64 b = d ? d->streamDiskBytes() : 0;
        total += b;
        per << QJsonObject{{QStringLiteral("stream"), s->id}, {QStringLiteral("bytes"), double(b)}};
    }
    return QJsonObject{{QStringLiteral("totalBytes"), double(total)}, {QStringLiteral("limitBytes"), double(limit)},
                       {QStringLiteral("overLimit"), total > limit}, {QStringLiteral("streams"), per},
                       {QStringLiteral("rule"), QStringLiteral("Over the limit, streams nobody reads are ended (their caches deleted), "
                                                               "the longest unread first; a stream that is being played is never cut "
                                                               "and can grow to the size of its file.")}};
}

QJsonObject StreamService::streamJson(const Stream *s) const
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), s->id);
    o.insert(QStringLiteral("name"), s->name);
    o.insert(QStringLiteral("contentType"), s->mime);
    o.insert(QStringLiteral("state"), s->state);
    o.insert(QStringLiteral("download"), s->downloadId);
    o.insert(QStringLiteral("search"), int(s->search));
    if (s->file >= 0) o.insert(QStringLiteral("file"), s->file);
    o.insert(QStringLiteral("ownCache"), s->owned);
    o.insert(QStringLiteral("normalDownload"), s->normal || !s->owned);
    o.insert(QStringLiteral("served"), double(s->served));
    o.insert(QStringLiteral("unverifiedServed"), double(s->unverifiedServed));
    o.insert(QStringLiteral("failedChecks"), double(s->failedServed));
    o.insert(QStringLiteral("longestWaitMs"), double(s->longestWaitMs));
    if (!s->ended && s->dropAtMs >= 0) o.insert(QStringLiteral("cacheDropInSec"), double(qMax<qint64>(0, (s->dropAtMs - m_clock.elapsed()) / 1000)));
    if (!s->error.isEmpty()) o.insert(QStringLiteral("error"), s->error);
    if (s->ended) {
        o.insert(QStringLiteral("ended"), s->endReason);
        o.insert(QStringLiteral("size"), double(s->size));
        return o;
    }
    Download *d = m_engine->downloadObject(s->downloadId);
    if (!d) return o;
    const Range span = d->streamSpan();
    const bool known = span.second > span.first;
    o.insert(QStringLiteral("size"), known ? double(span.second - span.first) : double(s->size));
    o.insert(QStringLiteral("sizeKnown"), known);
    if (!known)
        o.insert(QStringLiteral("waitingFor"), d->record().isBundle
                     ? QStringLiteral("the file list from the BitTorrent network (a player's request waits for it)")
                     : QStringLiteral("the file list from the BitTorrent network; the other networks of this file take over after %1 s "
                                      "(a player's request waits)").arg(m_engine->tuning().streamMetadataFallbackMs / 1000));
    if (!d->streamFileName().isEmpty()) {   // a bundle selected before its file list: the file now known
        o.insert(QStringLiteral("name"), d->streamFileName());
        o.insert(QStringLiteral("contentType"), mimeFor(d->streamFileName()));
    }
    const TransferInfo t = d->info();
    o.insert(QStringLiteral("transferStatus"), t.statusText);
    if (!t.errorText.isEmpty()) o.insert(QStringLiteral("transferError"), t.errorText);
    o.insert(QStringLiteral("speedBps"), t.rateDownBps);
    o.insert(QStringLiteral("sources"), t.sourcesTotal);
    o.insert(QStringLiteral("sourcesActive"), t.sourcesActive);
    QJsonArray nets;
    for (Net n : t.networks) nets << netName(n);
    o.insert(QStringLiteral("networks"), nets);
    QJsonObject fetched;
    QHash<QString, qint64> byNet;
    for (const TransferSource &src : t.sources)
        if (src.bytes > 0) byNet[netName(src.net)] += src.bytes;
    for (auto it = byNet.constBegin(); it != byNet.constEnd(); ++it) fetched.insert(it.key(), double(it.value()));
    o.insert(QStringLiteral("fetchedByNetwork"), fetched);
    o.insert(QStringLiteral("cachedBytes"), double(d->streamDiskBytes()));
    // what is on disk and can be served, in file coordinates
    if (known) {
        QJsonArray ranges;
        const QList<Range> rl = d->streamServableSet().ranges();
        for (int i = 0; i < rl.size() && i < 64; ++i)
            ranges << QJsonArray{double(rl.at(i).first - span.first), double(rl.at(i).second - span.first)};
        o.insert(QStringLiteral("ranges"), ranges);
        if (rl.size() > 64) o.insert(QStringLiteral("rangesNotShown"), int(rl.size() - 64));
    }
    o.insert(QStringLiteral("verification"),
             d->torrentId().isEmpty() || !known
                 ? (d->streamTrustsReceived()
                        ? QStringLiteral("Bytes are served as they arrive: the only check of this file is a large unit (an eD2k part of "
                                         "9.28 MB) or the whole file. A unit that fails its check is fetched again; bytes a player got "
                                         "from it are counted in failedChecks.")
                        : QStringLiteral("Every block is checked (tiger tree leaves, eD2k parts) before it is served."))
                 : QStringLiteral("Every BitTorrent piece is checked before it is served."));
    // the players
    const qint64 now = engine::nowMs();
    qint64 rateEst = s->bitrate;
    QJsonArray players;
    for (StreamReader *r : s->readers) {
        const qint64 rr = r->m_rate.rate(now, 30);
        const qint64 rate = qMax(qMax<qint64>(s->bitrate, s->lastRate), rr);
        rateEst = qMax(rateEst, rr);
        const qint64 ahead = known ? d->streamServable(span.first + r->m_pos).avail : 0;
        QJsonObject p{{QStringLiteral("id"), r->m_id},
                      {QStringLiteral("from"), double(r->m_start)},
                      {QStringLiteral("position"), double(r->m_pos)},
                      {QStringLiteral("end"), double(r->m_end)},
                      {QStringLiteral("sent"), double(r->m_sent)},
                      {QStringLiteral("rateBps"), double(rr)},
                      {QStringLiteral("bufferedAhead"), double(ahead)},
                      {QStringLiteral("bufferTarget"), double(qMin(r->m_end - r->m_pos, qBound(m_tune.minBuffer, rate * m_tune.bufferSec, m_tune.maxBuffer)))},
                      {QStringLiteral("firstByteMs"), double(r->m_firstByteMs)},
                      {QStringLiteral("longestWaitMs"), double(r->m_longestWaitMs)}};
        if (rate > 0) p.insert(QStringLiteral("bufferedAheadSec"), double(ahead) / double(rate));
        if (r->m_waitSinceMs >= 0)
            p.insert(QStringLiteral("waitingFor"), QJsonObject{{QStringLiteral("offset"), double(r->m_pos)},
                                                               {QStringLiteral("ms"), double(m_clock.elapsed() - r->m_waitSinceMs)}});
        players << p;
    }
    o.insert(QStringLiteral("players"), players);
    if (rateEst > 0) o.insert(QStringLiteral("bitrateBps"), double(rateEst));
    const int limitKBs = m_settings ? m_settings->i(keys::bandwidth::DownLimitKBs) : 0;
    o.insert(QStringLiteral("limits"),
             QJsonObject{{QStringLiteral("downloadsAtOnce"), QStringLiteral("not counted: a stream always runs")},
                         {QStringLiteral("speed"), limitKBs > 0 ? QStringLiteral("the download limit of %1 KB/s applies to streams too").arg(limitKBs)
                                                                : QStringLiteral("no download limit")},
                         {QStringLiteral("connections"), QStringLiteral("the proxy and network adapter settings apply as for every download")}});
    {
        const QJsonObject c = cacheJson();
        o.insert(QStringLiteral("cacheTotalBytes"), c.value(QStringLiteral("totalBytes")));
        o.insert(QStringLiteral("cacheLimitBytes"), c.value(QStringLiteral("limitBytes")));
    }
    if (s->overLimit) o.insert(QStringLiteral("cache"), QStringLiteral("over the cache limit: it is not cut while it is played; it ends as soon as no player reads it"));
    return o;
}

void StreamService::streamCall(const HttpRequest &req, HttpResponse &resp, const QByteArray &id, const QByteArray &key)
{
    Stream *s = streamById(QString::fromLatin1(id));
    if (!s || !sameBytes(key, streamKey(s->id).toLatin1())) {
        error(resp, 401, QStringLiteral("unauthorized"), QStringLiteral("Unauthorized."));
        return;
    }
    if (req.method != "GET" && req.method != "HEAD") {
        resp.setHeader("Allow", "GET, HEAD");
        error(resp, 405, QStringLiteral("method"), QStringLiteral("A stream link takes GET and HEAD."));
        return;
    }
    if (s->ended) {
        error(resp, 410, QStringLiteral("ended"), QStringLiteral("This stream has ended: %1.").arg(s->endReason));
        return;
    }
    const QString streamId = s->id;
    const QByteArray range = req.header("range");
    const bool head = req.method == "HEAD";
    const qint64 startedMs = m_clock.elapsed();
    QPointer<StreamService> self(this);
    auto fill = [self, streamId, range, head, startedMs](HttpResponse &r) -> bool {
        if (!self) {
            error(r, 503, QStringLiteral("closed"), QStringLiteral("OctoMX is closing."));
            return true;
        }
        Stream *s = self->streamById(streamId);
        Download *d = s && !s->ended ? self->m_engine->downloadObject(s->downloadId) : nullptr;
        if (!s || s->ended || !d) {
            error(r, 410, QStringLiteral("ended"), QStringLiteral("This stream has ended."));
            return true;
        }
        const Range span = d->streamSpan();
        if (span.second <= span.first) {
            if (self->m_clock.elapsed() - startedMs < self->m_tune.metaWaitMs) {   // the size is on its way
                s->lastPlayerMs = self->m_clock.elapsed();   // a player waits for it: not idle
                d->setStreamIdle(false);
                return false;
            }
            s->error = QStringLiteral("The size of the file did not arrive in %1 s (the torrent's file list): no peer sent it yet.")
                           .arg(self->m_tune.metaWaitMs / 1000);
            error(r, 504, QStringLiteral("no_size"), s->error);
            return true;
        }
        const qint64 size = span.second - span.first;
        qint64 a = 0, b = size;
        const int pr = range.isEmpty() ? 0 : parseRange(range, size, &a, &b);
        r.setHeader("Accept-Ranges", "bytes");
        r.setHeader("Cache-Control", "no-store");
        r.setHeader("X-Content-Type-Options", "nosniff");
        if (pr < 0) {
            r.status = 416;
            r.setHeader("Content-Range", "bytes */" + QByteArray::number(size));
            r.body.clear();
            return true;
        }
        if (pr == 0) { a = 0; b = size; }
        r.status = pr > 0 ? 206 : 200;
        const QString fileName = d->streamFileName().isEmpty() ? s->name : d->streamFileName();
        r.setHeader("Content-Type", mimeFor(fileName).toLatin1());
        r.setHeader("Content-Disposition", "inline; filename=\"" + asciiName(fileName).toLatin1() + "\"");
        if (pr > 0) r.setHeader("Content-Range", "bytes " + QByteArray::number(a) + '-' + QByteArray::number(b - 1) + '/' + QByteArray::number(size));
        r.setHeader("Content-Length", QByteArray::number(b - a));
        if (head || b <= a) return true;
        auto *reader = new StreamReader(self, streamId, s->nextReader++, a, b);
        s->readers << reader;
        s->lastPlayerMs = self->m_clock.elapsed();
        d->setStreamIdle(false);   // a player is back: the sources again, at once
        r.stream.reset(reader);
        r.streamLength = b - a;
        self->pushReads(s);   // the engine knows at once where this player reads
        emit self->stateChanged();
        return true;
    };
    if (!fill(resp)) resp.deferred = fill;
}

} // namespace mx::stream

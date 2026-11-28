// SPDX-License-Identifier: GPL-3.0-or-later
// [api-2] OctoMX core: the downloads of the local API (docs/API.md, "Downloads").
//
// Every download of the Transfers list (the ones the user started in the window too) as a resource:
// start one from a search result or a link (any file type), list them with totals, read one in full
// (state, progress, speed, sources, peers, files, hashes, the engine's diagnosis), pause, resume,
// look for more sources, change it (priority, wanted files, order), remove it from the list keeping
// its files or with them (only when the user allowed it: api/allowDelete), clear the finished ones.
// A stream's own cache (POST /streams without "download") is a stream, not a download of this list.
//
// Safety: the API names no folder and no file: a download goes where the window's would (the
// incoming folder); a link must be magnet:, ed2k:// or http(s):// (never a file of this computer); ids
// from the path are looked up in the engine's list, never used to build a path; what a delete
// removes is what the engine's removeDownload removes (DownloadDiskInfo::paths: only what the
// download created, or the file the user resumed it from in place), nothing else.
#include "core/stream/stream_service.h"

#include "core/download_request.h"
#include "core/engine/download.h"
#include "core/engine/engine.h"
#include "core/log.h"
#include "mx/settings_keys.h"
#include "mx/trash.h"
#include "mxhash/encoding.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QSaveFile>
#include <QtCore/QUrlQuery>

#include <algorithm>
#include <cmath>

namespace mx::stream {

using engine::Download;

namespace {

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

struct StateWord {
    TransferState state;
    const char *word;
};
// one word per TransferState, in the enum's order (docs/API.md lists them)
const StateWord kStates[] = {
    {TransferState::Queued, "queued"},           {TransferState::FindingSources, "finding_sources"},
    {TransferState::Connecting, "connecting"},   {TransferState::RemotelyQueued, "remotely_queued"},
    {TransferState::Transferring, "transferring"}, {TransferState::Paused, "paused"},
    {TransferState::Verifying, "verifying"},     {TransferState::Completed, "completed"},
    {TransferState::Seeding, "seeding"},         {TransferState::Error, "error"},
    {TransferState::Cancelled, "cancelled"},     {TransferState::TimedOut, "timed_out"},
};

bool finished(TransferState s)
{
    return s == TransferState::Completed || s == TransferState::Seeding || s == TransferState::Cancelled;
}

QJsonValue iso(const QDateTime &t)
{
    if (!t.isValid()) return QJsonValue::Null;
    return t.toUTC().toString(Qt::ISODate);
}

QJsonValue nullIfNeg(qint64 v)
{
    return v < 0 ? QJsonValue(QJsonValue::Null) : QJsonValue(double(v));
}

QString native(const QString &p) { return QDir::toNativeSeparators(p); }

QJsonArray nativeList(const QStringList &l)
{
    QJsonArray a;
    for (const QString &p : l) a << native(p);
    return a;
}

// the hashes as the links write them: hex for btih / ed2k / sha256 / md5, base32 for sha1 / tth / aich
QJsonObject hashesJson(const FileHashes &h)
{
    QJsonObject o;
    auto hex = [&o](const char *k, const QByteArray &v) {
        if (!v.isEmpty()) o.insert(QLatin1String(k), QString::fromLatin1(v.toHex()));
    };
    auto b32 = [&o](const char *k, const QByteArray &v) {
        if (!v.isEmpty()) o.insert(QLatin1String(k), hash::toBase32(v));
    };
    hex("btih", h.btih);
    hex("ed2k", h.ed2k);
    b32("sha1", h.sha1);
    b32("tth", h.tth);
    b32("aich", h.aich);
    hex("sha256", h.sha256);
    hex("md5", h.md5);
    return o;
}

// the window's list of file types it warns about before a download (ui/kit/launcher.cpp
// isDangerousFile: programs, scripts and archives that can hide one); kept the same here
bool dangerousName(const QString &name)
{
    static const QStringList ext = {QStringLiteral("exe"), QStringLiteral("scr"), QStringLiteral("com"), QStringLiteral("bat"),
                                    QStringLiteral("pif"), QStringLiteral("vbs"), QStringLiteral("cmd"), QStringLiteral("msi"),
                                    QStringLiteral("zip"), QStringLiteral("rar"), QStringLiteral("js"), QStringLiteral("ps1"),
                                    QStringLiteral("lnk"), QStringLiteral("jar"), QStringLiteral("sh")};
    return ext.contains(QFileInfo(name).suffix().toLower());
}

// the request the engine's downloadHit() makes of a hit (DownloadEngine::downloadHitRequest): only a
// real link is the download's URI, a network's own source descriptor stays a source
DownloadRequest requestOfHit(const SearchHit &hit)
{
    DownloadRequest r = requestFromHit(hit);
    const QString u = r.uri.trimmed().toLower();
    const bool link = u.startsWith(QLatin1String("magnet:")) || u.startsWith(QLatin1String("ed2k:"))
                      || u.startsWith(QLatin1String("http://")) || u.startsWith(QLatin1String("https://"))
                      || u.startsWith(QLatin1String("ftp://"));
    if (!link) {
        r.uri.clear();
        for (const SearchSource &s : hit.sources) {
            const QString su = s.uri.trimmed().toLower();
            if (s.extra.value(QStringLiteral("candidate")).toBool()) continue;
            if (su.startsWith(QLatin1String("magnet:")) || su.startsWith(QLatin1String("ed2k:"))) { r.uri = s.uri; break; }
        }
    }
    return r;
}

// a request may not say where files go: the incoming folder, as for the window
bool refusesPlace(HttpResponse &resp, const QJsonObject &body)
{
    for (const char *k : {"saveDir", "path", "folder", "dir", "directory", "target", "name", "fileName"}) {
        if (!body.contains(QLatin1String(k))) continue;
        error(resp, 400, QStringLiteral("not_allowed"),
              QStringLiteral("\"%1\": the API does not choose folders or file names. A download goes to the incoming folder "
                             "under the name its network gives, as in the window.").arg(QLatin1String(k)));
        return true;
    }
    return false;
}

// "files": a list of distinct file indexes (>= 0)
bool parseFiles(const QJsonValue &v, QList<int> *out, QString *why)
{
    if (!v.isArray()) {
        *why = QStringLiteral("\"files\" is a list of file indexes, for example [0, 2].");
        return false;
    }
    for (const QJsonValue &x : v.toArray()) {
        const double d = x.toDouble(-1);
        if (!x.isDouble() || d < 0 || d != double(int(d))) {
            *why = QStringLiteral("\"files\" holds file indexes (whole numbers from 0).");
            return false;
        }
        if (!out->contains(int(d))) *out << int(d);
    }
    if (out->isEmpty()) {
        *why = QStringLiteral("\"files\" is empty: name at least one file (leave \"files\" out for every file).");
        return false;
    }
    std::sort(out->begin(), out->end());
    return true;
}

bool parsePriority(const QJsonValue &v, int *out)
{
    const double d = v.toDouble(9);
    if (!v.isDouble() || (d != -1 && d != 0 && d != 1)) return false;
    *out = int(d);
    return true;
}

} // namespace

// ============================================================================ small parts

QString StreamService::stateWord(TransferState s)
{
    for (const StateWord &w : kStates)
        if (w.state == s) return QLatin1String(w.word);
    return QStringLiteral("queued");
}

bool StreamService::stateFromWord(const QString &w, TransferState *s)
{
    for (const StateWord &x : kStates)
        if (w == QLatin1String(x.word)) {
            *s = x.state;
            return true;
        }
    return false;
}

QList<TransferInfo> StreamService::listedDownloads() const
{
    QList<TransferInfo> out;
    for (const TransferInfo &t : m_engine->downloads()) {
        const Download *d = m_engine->downloadObject(t.id);
        if (d && d->streamRestricts()) continue;   // a stream's own cache: a stream, not a download
        out << t;
    }
    return out;
}

bool StreamService::listed(const QString &id) const
{
    const Download *d = m_engine->downloadObject(id);
    return d && !d->streamRestricts();
}

StreamService::Stream *StreamService::streamOfDownload(const QString &id) const
{
    for (Stream *s : m_streams)
        if (!s->ended && s->downloadId == id) return s;
    return nullptr;
}

StreamService::Removed *StreamService::removedById(const QString &id) const
{
    for (int i = int(m_removed.size()) - 1; i >= 0; --i)
        if (m_removed.at(i)->id == id) return m_removed.at(i);
    return nullptr;
}

void StreamService::promoteStream(Stream *s)
{
    Download *d = m_engine->downloadObject(s->downloadId);
    if (!d || !s->owned || !d->streamRestricts()) return;
    const int file = d->streamFile();
    d->endStream(true, m_engine->incomingDir());   // a normal download from now on, into the incoming folder
    if (!d->isFinished()) d->setStream(true, false, file);   // the stream goes on, joined (its order follows the player)
    s->owned = false;
    s->normal = true;
    s->cacheDir.clear();   // never deleted as a cache now: it is the download's data
    qCInfo(lcCore).noquote() << "streaming API: the cache of stream" << s->id << "is a normal download now:" << s->downloadId;
}

void StreamService::loadOrigins()
{
    m_apiDownloads.clear();
    QFile f(QDir(m_profile).filePath(QStringLiteral("api_downloads.json")));
    if (f.open(QIODevice::ReadOnly)) {
        const QJsonArray a = QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("downloads")).toArray();
        QSet<QString> known;
        for (const TransferInfo &t : m_engine->downloadsBrief()) known.insert(t.id);
        for (const QJsonValue &v : a)
            if (known.contains(v.toString())) m_apiDownloads.insert(v.toString());
        if (m_apiDownloads.size() != a.size()) saveOrigins();
    }
}

void StreamService::saveOrigins()
{
    if (m_profile.isEmpty()) return;
    const QString path = QDir(m_profile).filePath(QStringLiteral("api_downloads.json"));
    if (m_apiDownloads.isEmpty()) {
        QFile::remove(path);
        return;
    }
    QStringList ids(m_apiDownloads.begin(), m_apiDownloads.end());
    ids.sort();
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write(QJsonDocument(QJsonObject{{QStringLiteral("downloads"), QJsonArray::fromStringList(ids)}}).toJson(QJsonDocument::Compact));
    f.commit();
}

void StreamService::markApiDownload(const QString &id)
{
    if (id.isEmpty() || m_apiDownloads.contains(id)) return;
    m_apiDownloads.insert(id);
    saveOrigins();
}

QString StreamService::downloadWarning(const QString &fileName, const QString &suspect) const
{
    QStringList w;
    const bool quiet = m_settings && m_settings->b(keys::transfers::SuppressDangerousWarning);
    if (dangerousName(fileName) && !quiet)
        w << QStringLiteral("'%1' is a program or script, or a file that can hide one: such files may carry harmful code. "
                            "OctoMX downloads it; it never opens it by itself.").arg(fileName);
    if (!suspect.isEmpty()) w << QStringLiteral("It looks like a fake: %1.").arg(suspect);
    return w.join(QLatin1Char(' '));
}

bool StreamService::hitDownloadable(const SearchHit &h, QString *why) const
{
    DownloadRequest r = requestOfHit(h);
    // a network that only said "I have a result" names no place to fetch it from (the search adds such
    // a row to every result): not a source
    QList<SearchSource> usable;
    for (const SearchSource &s : std::as_const(r.sources))
        if (!s.uri.trimmed().isEmpty() || !s.user.id.isEmpty() || !s.user.address.isEmpty()) usable << s;
    r.sources = usable;
    QString err;
    if (resolveDownloadRequest(r, &err)) return true;
    if (why)
        *why = err == QLatin1String("Nothing to download") ? QStringLiteral("No network named a source of this result and no hash of it is known.")
                                                          : err;
    return false;
}

QJsonArray StreamService::typesJson() const
{
    static const QHash<QString, QString> what = {
        {QStringLiteral("any"), QStringLiteral("every file")},
        {QStringLiteral("audio"), QStringLiteral("audio files; audio:min:<kbps>, audio:eq:<kbps>, audio:max:<kbps> also limit the bit rate where a network tells it")},
        {QStringLiteral("video"), QStringLiteral("video files")},
        {QStringLiteral("pictures"), QStringLiteral("pictures")},
        {QStringLiteral("documents"), QStringLiteral("documents: e-books, comics, office files, web pages, subtitles")},
        {QStringLiteral("software"), QStringLiteral("programs, installers, packages and disc images")},
        {QStringLiteral("archives"), QStringLiteral("archives")},
        {QStringLiteral("discimages"), QStringLiteral("CD / DVD images; every network is asked for every type (an image can hold anything) and the extension decides")},
        {QStringLiteral("torrents"), QStringLiteral(".torrent files")},
        {QStringLiteral("mp3"), QStringLiteral("MP3 audio only; mp3:min:<kbps>, mp3:eq:<kbps>, mp3:max:<kbps> also limit the bit rate")},
        {QStringLiteral("lossless"), QStringLiteral("lossless audio")},
        {QStringLiteral("servers"), QStringLiteral("server lists")},
        {QStringLiteral("clr"), QStringLiteral("colour schemes")},
        {QStringLiteral("custom"), QStringLiteral("custom:<ext>,<ext>...: files with one of these extensions")},
    };
    QJsonArray a;
    for (const QString &k : FileTypeFilter::kinds()) {
        QJsonObject o{{QStringLiteral("code"), k}, {QStringLiteral("matches"), what.value(k)}};
        if (k != QLatin1String("custom") && k != QLatin1String("any")) {
            const FileTypeFilter f = FileTypeFilter::fromCode(k);
            QStringList ext = f.requiredExtensions();
            if (ext.isEmpty()) ext = extensionsForMediaType(f.mediaType());
            o.insert(QStringLiteral("extensions"), QJsonArray::fromStringList(ext));
        }
        a << o;
    }
    return a;
}

QJsonObject StreamService::downloadTotals(const QList<TransferInfo> &all) const
{
    QJsonObject by;
    for (const StateWord &w : kStates) by.insert(QLatin1String(w.word), 0);
    double down = 0, up = 0;
    for (const TransferInfo &t : all) {
        const QString w = stateWord(t.state);
        by.insert(w, by.value(w).toInt() + 1);
        down += t.rateDownBps;
        up += t.rateUpBps;
    }
    return QJsonObject{{QStringLiteral("count"), int(all.size())}, {QStringLiteral("byState"), by},
                       {QStringLiteral("speedBps"), down}, {QStringLiteral("uploadBps"), up}};
}

QJsonObject StreamService::downloadsStatusJson() const { return downloadTotals(listedDownloads()); }

// ============================================================================ a download as JSON

QJsonObject StreamService::downloadJson(const TransferInfo &t, bool full, bool withSources) const
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), t.id);
    o.insert(QStringLiteral("url"), QStringLiteral("/api/v1/downloads/%1").arg(t.id));
    o.insert(QStringLiteral("name"), t.name);
    o.insert(QStringLiteral("state"), stateWord(t.state));
    o.insert(QStringLiteral("statusText"), t.statusText);
    o.insert(QStringLiteral("error"), t.errorText.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(t.errorText));
    const bool known = t.size > 0;
    o.insert(QStringLiteral("size"), double(qMax<qint64>(0, t.size)));
    o.insert(QStringLiteral("sizeKnown"), known);
    o.insert(QStringLiteral("done"), double(qMax<qint64>(0, t.done)));
    double progress = known ? qBound(0.0, double(t.done) / double(t.size), 1.0) : 0.0;
    if (finished(t.state) && t.state != TransferState::Cancelled) progress = 1.0;
    o.insert(QStringLiteral("progress"), std::round(progress * 10000.0) / 10000.0);
    o.insert(QStringLiteral("speedBps"), t.rateDownBps);
    o.insert(QStringLiteral("uploadBps"), t.rateUpBps);
    o.insert(QStringLiteral("etaSec"), nullIfNeg(t.etaSec));
    o.insert(QStringLiteral("sources"), t.sourcesTotal);
    o.insert(QStringLiteral("sourcesActive"), t.sourcesActive);
    // peers and seeds: BitTorrent tells them (connected now, and the swarm as trackers / the DHT report it)
    QJsonValue peers = QJsonValue::Null, seeds = QJsonValue::Null, peersTotal = QJsonValue::Null, seedsTotal = QJsonValue::Null;
    const Download *d = m_engine->downloadObject(t.id);
    if (d && m_torrents && !d->torrentId().isEmpty() && m_torrents->torrents().contains(d->torrentId())) {
        const TorrentSnapshot sn = m_torrents->snapshot(d->torrentId());
        peers = sn.peers;
        seeds = sn.seeds;
        peersTotal = nullIfNeg(sn.peersTotal);
        seedsTotal = nullIfNeg(sn.seedsTotal);
    }
    o.insert(QStringLiteral("peers"), peers);
    o.insert(QStringLiteral("seeds"), seeds);
    o.insert(QStringLiteral("peersTotal"), peersTotal);
    o.insert(QStringLiteral("seedsTotal"), seedsTotal);
    QJsonArray nets;
    for (Net n : t.networks) nets << netName(n);
    o.insert(QStringLiteral("networks"), nets);
    o.insert(QStringLiteral("priority"), t.priority);
    o.insert(QStringLiteral("paused"), d ? d->isPausedByUser() : t.state == TransferState::Paused);
    o.insert(QStringLiteral("bundle"), t.isBundle);
    if (t.isBundle) {
        if (t.files.isEmpty()) {
            o.insert(QStringLiteral("files"), QJsonValue::Null);   // the torrent's file list has not arrived yet
        } else {
            const int cap = full ? 10000 : 20;
            QJsonArray fl;
            for (const BundleFile &f : t.files) {
                if (fl.size() >= cap) break;
                fl << QJsonObject{{QStringLiteral("index"), f.index}, {QStringLiteral("path"), f.path},
                                  {QStringLiteral("size"), double(f.size)}, {QStringLiteral("done"), nullIfNeg(f.done)},
                                  {QStringLiteral("wanted"), f.wanted}};
            }
            o.insert(QStringLiteral("files"), fl);
            if (t.files.size() > cap) o.insert(QStringLiteral("filesNotShown"), int(t.files.size() - cap));
        }
    }
    o.insert(QStringLiteral("origin"), m_apiDownloads.contains(t.id) ? QStringLiteral("api") : QStringLiteral("user"));
    const Stream *s = streamOfDownload(t.id);
    o.insert(QStringLiteral("stream"), s ? QJsonValue(s->id) : QJsonValue(QJsonValue::Null));
    o.insert(QStringLiteral("added"), iso(t.added));
    o.insert(QStringLiteral("completed"), iso(t.completed));
    if (!full) return o;
    o.insert(QStringLiteral("path"), t.savePath.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(native(t.savePath)));
    o.insert(QStringLiteral("hashes"), hashesJson(t.hashes));
    if (!t.hashes.isEmpty()) o.insert(QStringLiteral("magnet"), t.hashes.toMagnet(t.name, t.size));
    o.insert(QStringLiteral("diagnosis"), t.diagnosis.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(t.diagnosis));
    o.insert(QStringLiteral("sequential"), t.sequential);
    o.insert(QStringLiteral("firstLastFirst"), t.firstLastFirst);
    if (m_settings)
        o.insert(QStringLiteral("limits"),
                 QJsonObject{{QStringLiteral("downloadsAtOnce"), m_settings->i(keys::transfers::MaxDownloads)},
                             {QStringLiteral("downloadLimitKBs"), m_settings->i(keys::bandwidth::DownLimitKBs)},
                             {QStringLiteral("uploadLimitKBs"), m_settings->i(keys::bandwidth::UpLimitKBs)}});
    if (withSources) {
        // what a program needs to judge a source: its network, state, bytes, speed and client; never
        // another person's address or user name
        QJsonArray a;
        for (const TransferSource &src : t.sources) {
            if (a.size() >= 500) break;
            a << QJsonObject{{QStringLiteral("network"), netName(src.net)}, {QStringLiteral("status"), src.statusText},
                             {QStringLiteral("bytes"), double(src.bytes)}, {QStringLiteral("speedBps"), src.rateBps},
                             {QStringLiteral("client"), src.client.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(src.client)}};
        }
        o.insert(QStringLiteral("sourceList"), a);
    }
    return o;
}

QJsonObject StreamService::removedJson(const Removed *r) const
{
    QJsonObject o{{QStringLiteral("id"), r->id},
                  {QStringLiteral("name"), r->name},
                  {QStringLiteral("removed"), !r->rowPending},
                  {QStringLiteral("finished"), r->finished},
                  {QStringLiteral("state"), QStringLiteral("removed")},
                  {QStringLiteral("files"), r->files},
                  {QStringLiteral("pending"), r->pending},
                  {QStringLiteral("kept"), nativeList(r->kept)},
                  {QStringLiteral("trashed"), nativeList(r->trashed)},
                  {QStringLiteral("deleted"), nativeList(r->deleted)},
                  {QStringLiteral("failed"), nativeList(r->failed)},
                  {QStringLiteral("errors"), QJsonArray::fromStringList(r->errors)},
                  {QStringLiteral("download"), r->last}};
    if (!r->inPlaceFile.isEmpty()) o.insert(QStringLiteral("inPlaceFile"), native(r->inPlaceFile));
    if (!r->streamEnded.isEmpty()) o.insert(QStringLiteral("streamEnded"), r->streamEnded);
    const bool meanwhile = !r->rowPending && r->finished && !r->wasFinished;
    if (meanwhile) o.insert(QStringLiteral("finishedWhileRemoved"), true);
    QString msg;
    if (r->rowPending)
        msg = QStringLiteral("It is being moved to the incoming folder (it had just finished): it leaves the list when the move ends "
                             "(read this download again for the outcome).");
    else if (r->files == QLatin1String("keep"))
        msg = meanwhile ? QStringLiteral("It finished while it was being removed: removed from the list, the finished file stays in the incoming folder.")
              : r->finished ? QStringLiteral("Removed from the list; its file stays.")
                            : QStringLiteral("Removed from the list; its files stay (a partial download continues when the same file is downloaded again).");
    else if (r->pending)
        msg = QStringLiteral("Removed from the list; its files are being deleted (read this download again for the outcome).");
    else if (!r->failed.isEmpty())
        msg = QStringLiteral("Removed from the list; %1 of its files could not be removed (see failed and errors).").arg(r->failed.size());
    else
        msg = r->files == QLatin1String("trash") ? QStringLiteral("Removed from the list; its files are in the %1.").arg(trash::placeName())
                                                 : QStringLiteral("Removed from the list; its files are deleted.");
    if (meanwhile && r->files != QLatin1String("keep"))
        msg = QStringLiteral("It finished while it was being removed: the finished file is what was removed. ") + msg;
    o.insert(QStringLiteral("message"), msg);
    return o;
}

void StreamService::filesDeleted(const QString &id, const QStringList &trashed, const QStringList &deleted, const QStringList &failed,
                                 const QStringList &errors)
{
    bool any = false;
    for (Removed *r : std::as_const(m_removed)) {
        if (r->id != id || !r->filesPending || r->rowPending) continue;
        r->filesPending = false;
        r->pending = false;
        r->trashed = trashed;
        r->deleted = deleted;
        r->failed = failed;
        r->errors = errors;
        any = true;
    }
    if (any)
        qCInfo(lcCore).noquote() << "streaming API: the files of download" << id << "are removed:" << trashed.size() << "to the trash,"
                                 << deleted.size() << "deleted," << failed.size() << "failed";
}

// [api-2] the engine removed the row: what really happened (a download that was finishing may have
// finished in that moment; its files are where the engine says)
void StreamService::downloadRemoved(const QString &id, bool finished, const QStringList &kept, const QString &inPlaceFile)
{
    for (Removed *r : std::as_const(m_removed)) {
        if (r->id != id || !r->rowPending) continue;
        r->rowPending = false;
        r->finished = finished;
        r->kept = kept;
        r->inPlaceFile = inPlaceFile;
        r->pending = r->filesPending;
    }
}

StreamService::Removed *StreamService::beginRemoval(const TransferInfo &t, const QString &files)
{
    auto *r = new Removed;
    r->id = t.id;
    r->name = t.name;
    r->files = files;
    r->removedMs = m_clock.elapsed();
    r->last = downloadJson(t, true, false);
    r->wasFinished = finished(t.state);
    r->rowPending = true;
    r->filesPending = files != QLatin1String("keep");
    r->pending = true;
    m_removed << r;   // before the engine is asked: its outcome may come at once
    return r;
}

// ============================================================================ the calls

void StreamService::downloadsCall(const HttpRequest &req, HttpResponse &resp, const QList<QByteArray> &seg, const QByteArray &m,
                                  const QJsonObject &body)
{
    const QUrlQuery query(QString::fromUtf8(req.query));
    auto wrongMethod = [&](const char *allow) {
        resp.setHeader("Allow", allow);
        error(resp, 405, QStringLiteral("method"), QStringLiteral("This call takes %1.").arg(QLatin1String(allow)));
    };
    const int n = int(seg.size());
    // ---- /downloads
    if (n == 1) {
        if (m == "POST") return startDownload(resp, body);
        if (m == "DELETE") {
            // clear the finished ones off the list (their files stay), like the window's Clear Completed;
            // the upload list is not touched
            const QString st = query.queryItemValue(QStringLiteral("state"));
            if (st != QLatin1String("completed")) {
                error(resp, 400, QStringLiteral("state"),
                      QStringLiteral("DELETE /api/v1/downloads takes ?state=completed (it clears the finished downloads off the list, "
                                     "their files stay). One download: DELETE /api/v1/downloads/<id>."));
                return;
            }
            QJsonArray ids;
            for (const TransferInfo &t : listedDownloads()) {
                if (!finished(t.state)) continue;
                beginRemoval(t, QStringLiteral("keep"));
                if (Stream *s = streamOfDownload(t.id)) endStream(s, QStringLiteral("its download was cleared off the list through the API"), false);
                m_engine->removeDownload(t.id, FileRemoval::Keep);
                ids << t.id;
            }
            json(resp, 200, QJsonObject{{QStringLiteral("cleared"), int(ids.size())}, {QStringLiteral("ids"), ids}});
            return;
        }
        if (m != "GET") return wrongMethod("GET, POST, DELETE");
        const QList<TransferInfo> all = listedDownloads();
        QList<TransferState> want;
        const QString st = query.queryItemValue(QStringLiteral("state"));
        for (const QString &w : st.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            TransferState s;
            if (!stateFromWord(w.trimmed(), &s)) {
                QStringList words;
                for (const StateWord &x : kStates) words << QLatin1String(x.word);
                error(resp, 400, QStringLiteral("state"), QStringLiteral("Unknown state \"%1\": one of %2.").arg(w.trimmed(), words.join(QStringLiteral(", "))));
                return;
            }
            want << s;
        }
        const int offset = qMax(0, query.queryItemValue(QStringLiteral("offset")).toInt());
        int limit = query.hasQueryItem(QStringLiteral("limit")) ? query.queryItemValue(QStringLiteral("limit")).toInt() : 100;
        limit = qBound(1, limit, 500);
        QJsonArray a;
        int count = 0;
        for (const TransferInfo &t : all) {
            if (!want.isEmpty() && !want.contains(t.state)) continue;
            if (count++ < offset || a.size() >= limit) continue;
            a << downloadJson(t, false, false);
        }
        json(resp, 200, QJsonObject{{QStringLiteral("count"), count}, {QStringLiteral("offset"), offset}, {QStringLiteral("limit"), limit},
                                    {QStringLiteral("downloads"), a}, {QStringLiteral("totals"), downloadTotals(all)}});
        return;
    }
    // ---- /downloads/pause, /downloads/resume: all of them
    if (n == 2 && (seg.at(1) == "pause" || seg.at(1) == "resume")) {
        if (m != "POST") return wrongMethod("POST");
        const bool pause = seg.at(1) == "pause";
        int changed = 0;
        for (const TransferInfo &t : listedDownloads()) {
            if (finished(t.state)) continue;
            const Download *d = m_engine->downloadObject(t.id);
            if (!d || d->isPausedByUser() == pause) continue;
            if (pause) m_engine->pause(t.id);
            else m_engine->resume(t.id);
            ++changed;
        }
        json(resp, 200, QJsonObject{{pause ? QStringLiteral("paused") : QStringLiteral("resumed"), changed},
                                    {QStringLiteral("totals"), downloadTotals(listedDownloads())}});
        return;
    }
    // ---- /downloads/<id>...
    const QString id = QString::fromLatin1(seg.at(1));
    if (!listed(id)) {
        if (const Download *d = id.isEmpty() ? nullptr : m_engine->downloadObject(id)) {
            const Stream *s = streamOfDownload(id);
            Q_UNUSED(d)
            error(resp, 404, QStringLiteral("not_found"),
                  QStringLiteral("This is the cache of a stream, not a download of the list: see /api/v1/streams/%1.").arg(s ? s->id : QString()),
                  s ? QJsonObject{{QStringLiteral("stream"), s->id}} : QJsonObject());
            return;
        }
        if (const Removed *r = removedById(id)) {
            QJsonObject o = removedJson(r);
            o.insert(QStringLiteral("error"), QStringLiteral("removed"));
            json(resp, 410, o);
            return;
        }
        error(resp, 404, QStringLiteral("not_found"), QStringLiteral("No download with this id."));
        return;
    }
    if (n == 2) {
        if (m == "GET") {
            const bool src = query.queryItemValue(QStringLiteral("sources")) == QLatin1String("1")
                             || query.queryItemValue(QStringLiteral("sources")) == QLatin1String("true");
            json(resp, 200, downloadJson(m_engine->transfer(id), true, src));
            return;
        }
        if (m == "POST") return changeDownload(resp, id, body);
        if (m == "DELETE") return removeDownloadCall(resp, id, query.queryItemValue(QStringLiteral("files")));
        return wrongMethod("GET, POST, DELETE");
    }
    if (n == 3 && (seg.at(2) == "pause" || seg.at(2) == "resume" || seg.at(2) == "sources")) {
        if (m != "POST") return wrongMethod("POST");
        const TransferInfo t = m_engine->transfer(id);
        QString note;
        if (seg.at(2) == "pause") {
            if (!finished(t.state)) m_engine->pause(id);
        } else if (seg.at(2) == "resume") {
            if (t.state == TransferState::Error || t.state == TransferState::TimedOut) m_engine->retry(id);   // the window's Resume of a failed one
            else if (!finished(t.state)) m_engine->resume(id);
        } else {
            if (finished(t.state)) {
                note = QStringLiteral("It is finished: it needs no sources.");
            } else {
                m_engine->findMoreSources(id);
                note = QStringLiteral("Every network is asked for more sources now; new ones show in a few seconds.");
            }
        }
        QJsonObject o = downloadJson(m_engine->transfer(id), true, false);
        if (!note.isEmpty()) o.insert(QStringLiteral("message"), note);
        json(resp, 200, o);
        return;
    }
    error(resp, 404, QStringLiteral("not_found"), QStringLiteral("Not found."));
}

void StreamService::startDownload(HttpResponse &resp, const QJsonObject &body)
{
    if (refusesPlace(resp, body)) return;
    const bool hasLink = body.contains(QStringLiteral("link"));
    const bool hasHit = body.contains(QStringLiteral("search")) || body.contains(QStringLiteral("result"));
    if (hasLink == hasHit) {
        error(resp, 400, QStringLiteral("what"),
              hasLink ? QStringLiteral("Give either \"link\" or \"search\" and \"result\", not both.")
                      : QStringLiteral("Give \"search\" and \"result\" (a search result's key), or \"link\" (a magnet:, ed2k:// or http(s):// link)."));
        return;
    }
    bool paused = false;
    if (body.contains(QStringLiteral("paused"))) {
        if (!body.value(QStringLiteral("paused")).isBool()) {
            error(resp, 400, QStringLiteral("paused"), QStringLiteral("\"paused\" is true or false."));
            return;
        }
        paused = body.value(QStringLiteral("paused")).toBool();
    }
    int priority = 0;
    if (body.contains(QStringLiteral("priority")) && !parsePriority(body.value(QStringLiteral("priority")), &priority)) {
        error(resp, 400, QStringLiteral("priority"), QStringLiteral("\"priority\" is -1 (low), 0 (normal) or 1 (high)."));
        return;
    }
    QList<int> files;
    if (body.contains(QStringLiteral("files"))) {
        QString why;
        if (!parseFiles(body.value(QStringLiteral("files")), &files, &why)) {
            error(resp, 400, QStringLiteral("files"), why);
            return;
        }
    }
    QSet<QString> before, kept;
    for (const TransferInfo &t : m_engine->downloadsBrief()) before.insert(t.id);
    for (const TransferInfo &t : m_engine->keptDownloads()) kept.insert(t.id);
    QString name, suspect;
    DownloadRequest req;
    bool viaHit = false;
    SearchHit hit;
    if (hasLink) {
        const QString link = body.value(QStringLiteral("link")).toString().trimmed();
        const QString lower = link.toLower();
        if (!(lower.startsWith(QLatin1String("magnet:?")) || lower.startsWith(QLatin1String("ed2k://"))
              || lower.startsWith(QLatin1String("http://")) || lower.startsWith(QLatin1String("https://")))
            || link.size() > 8000) {
            error(resp, 400, QStringLiteral("link"),
                  QStringLiteral("\"link\" is a magnet:, ed2k:// or http(s):// link. The API never opens a file of this computer."));
            return;
        }
        if (!files.isEmpty()) {
            error(resp, 400, QStringLiteral("files"),
                  QStringLiteral("\"files\" needs the file list of a search result. Start the link without it and choose the files "
                                 "with POST /api/v1/downloads/<id> once the download knows its file list."));
            return;
        }
        req.uri = link;
        req.startPaused = paused;
    } else {
        const quint32 sid = quint32(body.value(QStringLiteral("search")).toInt());
        const QString key = body.value(QStringLiteral("result")).toString();
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
        const SearchHit *found = nullptr;
        for (const SearchHit &h : hits)
            if (hitKeyOf(h) == key) { found = &h; break; }
        if (!found) {
            error(resp, 404, QStringLiteral("not_found"),
                  QStringLiteral("No result with this key (\"result\"). Results can merge while a search runs: read them again."));
            return;
        }
        hit = *found;
        name = hit.fileName;
        suspect = hit.suspect;
        QString why;
        if (!hitDownloadable(hit, &why)) {
            error(resp, 409, QStringLiteral("no_sources"), why);
            return;
        }
        if (!files.isEmpty()) {
            if (!hit.isBundle) {
                error(resp, 400, QStringLiteral("files"), QStringLiteral("This result is one file, not a bundle: leave \"files\" out."));
                return;
            }
            if (hit.files.isEmpty()) {
                m_search->requestBundleFiles(se->sid, hit.key);
                error(resp, 409, QStringLiteral("files_unknown"),
                      QStringLiteral("The file list of this torrent is not known yet. It was asked for: start again in a few seconds, "
                                     "or leave \"files\" out to download every file."));
                return;
            }
            for (int f : std::as_const(files)) {
                bool ok = false;
                for (const BundleFile &b : std::as_const(hit.files)) ok = ok || b.index == f;
                if (!ok) {
                    error(resp, 400, QStringLiteral("file"), QStringLiteral("This torrent has no file %1.").arg(f));
                    return;
                }
            }
            req = requestOfHit(hit);
            req.wantedFiles = files;
            req.startPaused = paused;
        } else {
            viaHit = true;   // downloadHit: the download keeps following the search result's sources
        }
    }
    // the reason a download did not start, in the engine's words
    QString notStarted;
    const QMetaObject::Connection c = connect(m_engine, &TransferService::downloadNotStarted, this,
                                              [&notStarted](const QString &, const QString &why) { notStarted = why; });
    const QString id = viaHit ? m_engine->downloadHit(hit) : m_engine->download(req);
    disconnect(c);
    Download *d = id.isEmpty() ? nullptr : m_engine->downloadObject(id);
    if (!d) {
        error(resp, 409, QStringLiteral("not_started"), notStarted.isEmpty() ? QStringLiteral("The download could not be started.") : notStarted);
        return;
    }
    bool existed = before.contains(id);
    QString fromStream;
    if (d->streamRestricts()) {
        // the file is streamed with its own cache: that cache becomes this download (the stream goes on)
        if (Stream *s = streamOfDownload(id)) {
            fromStream = s->id;
            promoteStream(s);
        } else {
            d->endStream(true, m_engine->incomingDir());
        }
        existed = false;
    }
    if (!existed) {
        markApiDownload(id);
        if (viaHit && paused) m_engine->pause(id);   // in the same turn of the event loop: nothing was fetched yet
        if (priority != 0) m_engine->setPriority(id, priority);
    }
    const TransferInfo t = m_engine->transfer(id);
    QJsonObject o = downloadJson(t, true, false);
    o.insert(QStringLiteral("existing"), existed);
    if (!existed && kept.contains(id)) o.insert(QStringLiteral("continued"), true);   // removed earlier with its data kept
    if (!fromStream.isEmpty()) o.insert(QStringLiteral("fromStream"), fromStream);
    const QString warning = downloadWarning(name.isEmpty() ? t.name : name, suspect);
    if (!warning.isEmpty()) o.insert(QStringLiteral("warning"), warning);
    if (existed) o.insert(QStringLiteral("message"), QStringLiteral("This file is in the list already: nothing was changed (POST /api/v1/downloads/%1 changes it).").arg(id));
    qCInfo(lcCore).noquote() << "streaming API: download" << id << t.name << (existed ? "(in the list already)" : "started");
    json(resp, existed ? 200 : 201, o);
    emit stateChanged();
}

void StreamService::changeDownload(HttpResponse &resp, const QString &id, const QJsonObject &body)
{
    if (refusesPlace(resp, body)) return;
    const TransferInfo t = m_engine->transfer(id);
    const bool hasPrio = body.contains(QStringLiteral("priority")), hasFiles = body.contains(QStringLiteral("files"));
    const bool hasSeq = body.contains(QStringLiteral("sequential")), hasFl = body.contains(QStringLiteral("firstLastFirst"));
    if (!hasPrio && !hasFiles && !hasSeq && !hasFl) {
        error(resp, 400, QStringLiteral("nothing"), QStringLiteral("Nothing to change: give \"priority\", \"files\", \"sequential\" or \"firstLastFirst\"."));
        return;
    }
    int priority = t.priority;
    if (hasPrio && !parsePriority(body.value(QStringLiteral("priority")), &priority)) {
        error(resp, 400, QStringLiteral("priority"), QStringLiteral("\"priority\" is -1 (low), 0 (normal) or 1 (high)."));
        return;
    }
    for (const char *k : {"sequential", "firstLastFirst"})
        if (body.contains(QLatin1String(k)) && !body.value(QLatin1String(k)).isBool()) {
            error(resp, 400, QLatin1String(k), QStringLiteral("\"%1\" is true or false.").arg(QLatin1String(k)));
            return;
        }
    QList<int> files;
    if (hasFiles) {
        QString why;
        if (!parseFiles(body.value(QStringLiteral("files")), &files, &why)) {
            error(resp, 400, QStringLiteral("files"), why);
            return;
        }
        if (!t.isBundle) {
            error(resp, 400, QStringLiteral("files"), QStringLiteral("This download is one file, not a bundle: it has no files to choose."));
            return;
        }
        if (t.files.isEmpty()) {
            error(resp, 409, QStringLiteral("files_unknown"),
                  QStringLiteral("The file list of this torrent has not arrived yet: choose its files when \"files\" is a list."));
            return;
        }
        for (int f : std::as_const(files)) {
            bool ok = false;
            for (const BundleFile &b : t.files) ok = ok || b.index == f;
            if (!ok) {
                error(resp, 400, QStringLiteral("file"), QStringLiteral("This torrent has no file %1.").arg(f));
                return;
            }
        }
    }
    if (hasPrio) m_engine->setPriority(id, priority);
    if (hasFiles) m_engine->setWantedFiles(id, files);
    if (hasSeq) m_engine->setSequentialDownload(id, body.value(QStringLiteral("sequential")).toBool());
    if (hasFl) m_engine->setFirstLastPiecesFirst(id, body.value(QStringLiteral("firstLastFirst")).toBool());
    TransferInfo now = m_engine->transfer(id);
    if (hasFiles)   // a torrent reports its new file priorities a moment later: the answer says the new choice now
        for (BundleFile &f : now.files) f.wanted = files.contains(f.index);
    json(resp, 200, downloadJson(now, true, false));
}

void StreamService::removeDownloadCall(HttpResponse &resp, const QString &id, const QString &filesIn)
{
    const QString files = filesIn.isEmpty() ? QStringLiteral("keep") : filesIn;
    FileRemoval mode = FileRemoval::Keep;
    if (files == QLatin1String("trash")) mode = FileRemoval::Trash;
    else if (files == QLatin1String("delete")) mode = FileRemoval::Delete;
    else if (files != QLatin1String("keep")) {
        error(resp, 400, QStringLiteral("files"), QStringLiteral("?files= is keep (the default), trash or delete."));
        return;
    }
    // [api-2] removed already and waiting for its move to the incoming folder: no second removal (the
    // engine would ignore it); the answer is the first one's, with the mode that is in force
    QJsonObject extra;
    const Removed *first = nullptr;
    for (const Removed *p : std::as_const(m_removed))
        if (p->id == id && p->rowPending) first = p;
    if (first && first->files != files) {
        extra.insert(QStringLiteral("filesAsked"), files);
        extra.insert(QStringLiteral("note"),
                     QStringLiteral("This download is being removed already with files=%1, asked first: files=%2 of this call is not "
                                    "applied.").arg(first->files, files));
    }
    if (!first && mode != FileRemoval::Keep && !(m_settings && m_settings->b(keys::api::AllowDelete))) {
        error(resp, 403, QStringLiteral("delete_not_allowed"),
              QStringLiteral("Programs may not delete downloaded files here: the user has not allowed it (Settings, Local API, "
                             "\"Programs may delete downloaded files\"). Without ?files the download leaves the list and its files stay."));
        return;
    }
    if (!first) {
        const TransferInfo t = m_engine->transfer(id);
        Removed *r = beginRemoval(t, files);
        // a stream that plays this download ends first: the player gets 410 from now on
        if (Stream *s = streamOfDownload(id)) {
            r->streamEnded = s->id;
            endStream(s, QStringLiteral("its download was removed through the API"), false);
        }
        qCInfo(lcCore).noquote() << "streaming API: download" << id << t.name << "removed from the list, files:" << files;
        // what is kept, deleted or trashed comes from the engine (downloadRemoved, downloadFilesDeleted):
        // a download that is finishing may finish in this moment, or leave the list only when its move ends
        m_engine->removeDownload(id, mode);
        emit stateChanged();
    }
    // the outcome arrives once the engine let the files go (a torrent: after libtorrent closed them)
    QPointer<StreamService> self(this);
    const qint64 since = m_clock.elapsed();
    auto fill = [self, id, since, extra](HttpResponse &x) -> bool {
        if (!self) {
            error(x, 503, QStringLiteral("closed"), QStringLiteral("OctoMX is closing."));
            return true;
        }
        const Removed *r = self->removedById(id);
        if (!r) {
            error(x, 500, QStringLiteral("lost"), QStringLiteral("The record of this removal is gone."));
            return true;
        }
        if (r->pending && self->m_clock.elapsed() - since < self->m_tune.deleteWaitMs) return false;
        QJsonObject o = self->removedJson(r);
        for (auto it = extra.begin(); it != extra.end(); ++it) o.insert(it.key(), it.value());
        json(x, r->pending ? 202 : 200, o);
        return true;
    };
    if (!fill(resp)) resp.deferred = fill;
}

} // namespace mx::stream

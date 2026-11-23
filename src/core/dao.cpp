// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/dao.h"
#include "core/database.h"
#include "core/log.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtSql/QSqlError>
#include <QtSql/QSqlQuery>
#include <QtSql/QSqlRecord>

namespace mx {

namespace {

qint64 toMs(const QDateTime &t) { return t.isValid() ? t.toMSecsSinceEpoch() : 0; }
QDateTime fromMs(const QVariant &v)
{
    const qint64 ms = v.toLongLong();
    return ms > 0 ? QDateTime::fromMSecsSinceEpoch(ms) : QDateTime();
}
QVariant blob(const QByteArray &b) { return b.isEmpty() ? QVariant(QMetaType(QMetaType::QByteArray)) : QVariant(b); }
qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

QString hashesToJson(const FileHashes &h)
{
    return QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(h.toVariant())).toJson(QJsonDocument::Compact));
}
FileHashes hashesFromJson(const QString &s)
{
    return FileHashes::fromVariant(QJsonDocument::fromJson(s.toUtf8()).object().toVariantMap());
}

QString filesToJson(const QList<BundleFile> &files)
{
    QJsonArray a;
    for (const BundleFile &f : files)
        a.append(QJsonObject{{QStringLiteral("p"), f.path}, {QStringLiteral("s"), double(f.size)}, {QStringLiteral("i"), f.index}});
    return QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact));
}
QList<BundleFile> filesFromJson(const QString &s)
{
    QList<BundleFile> out;
    const QJsonArray a = QJsonDocument::fromJson(s.toUtf8()).array();
    for (const QJsonValue &v : a) {
        const QJsonObject o = v.toObject();
        BundleFile f;
        f.path = o.value(QStringLiteral("p")).toString();
        f.size = qint64(o.value(QStringLiteral("s")).toDouble());
        f.index = o.value(QStringLiteral("i")).toInt(-1);
        out << f;
    }
    return out;
}

QString mapToJson(const QVariantMap &m)
{
    if (m.isEmpty()) return QString();
    return QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(m)).toJson(QJsonDocument::Compact));
}
QVariantMap mapFromJson(const QString &s)
{
    if (s.isEmpty()) return {};
    return QJsonDocument::fromJson(s.toUtf8()).object().toVariantMap();
}

QString userKey(const UserRef &u) { return u.key(); }
// QString() binds as SQL NULL; NOT NULL text columns need an empty string instead
QString nn(const QString &s) { return s.isNull() ? QStringLiteral("") : s; }

} // namespace

QByteArray packHashList(const QList<QByteArray> &hashes)
{
    QByteArray out;
    for (const QByteArray &h : hashes) out += h;
    return out;
}

QList<QByteArray> unpackHashList(const QByteArray &blob, int hashSize)
{
    QList<QByteArray> out;
    if (hashSize <= 0) return out;
    for (int i = 0; i + hashSize <= blob.size(); i += hashSize) out << blob.mid(i, hashSize);
    return out;
}

QString intListToString(const QList<int> &l)
{
    QStringList s;
    for (int i : l) s << QString::number(i);
    return s.join(QLatin1Char(','));
}

QList<int> intListFromString(const QString &s)
{
    QList<int> out;
    for (const QString &p : s.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        bool ok = false;
        const int v = p.trimmed().toInt(&ok);
        if (ok) out << v;
    }
    return out;
}

// ================================================================ downloads
static DownloadRecord readDownload(const QSqlQuery &q)
{
    DownloadRecord r;
    const QSqlRecord rec = q.record();
    auto v = [&](const char *name) { return q.value(rec.indexOf(QLatin1String(name))); };
    r.id = v("id").toString();
    r.name = v("name").toString();
    r.size = v("size").toLongLong();
    r.hashes = hashesFromJson(v("hashes").toString());
    r.state = v("state").toInt();
    r.statusText = v("status_text").toString();
    r.errorText = v("error_text").toString();
    r.saveDir = v("save_dir").toString();
    r.savePath = v("save_path").toString();
    r.uri = v("uri").toString();
    r.wantedFiles = intListFromString(v("wanted").toString());
    r.priority = v("priority").toInt();
    r.isBundle = v("is_bundle").toBool();
    r.files = filesFromJson(v("files").toString());
    r.done = v("done").toLongLong();
    r.uploaded = v("uploaded").toLongLong();
    r.added = fromMs(v("added"));
    r.completed = fromMs(v("completed"));
    r.engineState = v("engine_state").toByteArray();
    r.sortOrder = v("sort_order").toInt();
    return r;
}

QList<DownloadRecord> DownloadsDao::all() const
{
    QList<DownloadRecord> out;
    QSqlQuery q = m_db->exec(QStringLiteral("SELECT * FROM downloads ORDER BY sort_order, added"));
    while (q.next()) out << readDownload(q);
    return out;
}

bool DownloadsDao::get(const QString &id, DownloadRecord *out) const
{
    QSqlQuery q = m_db->exec(QStringLiteral("SELECT * FROM downloads WHERE id=?"), {id});
    if (!q.next()) return false;
    if (out) *out = readDownload(q);
    return true;
}

bool DownloadsDao::upsert(const DownloadRecord &r)
{
    m_db->flush(); // keep ordering with deferred progress writes
    bool ok = false;
    m_db->exec(QStringLiteral(
                   "INSERT INTO downloads (id,name,size,hashes,btih,ed2k,sha1,state,status_text,error_text,save_dir,"
                   "save_path,uri,wanted,priority,is_bundle,files,done,uploaded,added,completed,engine_state,sort_order)"
                   " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"
                   " ON CONFLICT(id) DO UPDATE SET name=excluded.name,size=excluded.size,hashes=excluded.hashes,"
                   "btih=excluded.btih,ed2k=excluded.ed2k,sha1=excluded.sha1,state=excluded.state,"
                   "status_text=excluded.status_text,error_text=excluded.error_text,save_dir=excluded.save_dir,"
                   "save_path=excluded.save_path,uri=excluded.uri,wanted=excluded.wanted,priority=excluded.priority,"
                   "is_bundle=excluded.is_bundle,files=excluded.files,done=excluded.done,uploaded=excluded.uploaded,"
                   "added=excluded.added,completed=excluded.completed,engine_state=excluded.engine_state,"
                   "sort_order=excluded.sort_order"),
               {r.id, nn(r.name), r.size, hashesToJson(r.hashes), blob(r.hashes.btih), blob(r.hashes.ed2k),
                blob(r.hashes.sha1), r.state, r.statusText, r.errorText, r.saveDir, r.savePath, r.uri,
                intListToString(r.wantedFiles), r.priority, r.isBundle ? 1 : 0, filesToJson(r.files), r.done,
                r.uploaded, toMs(r.added.isValid() ? r.added : QDateTime::currentDateTime()), toMs(r.completed),
                blob(r.engineState), r.sortOrder},
               &ok);
    return ok;
}

bool DownloadsDao::remove(const QString &id)
{
    m_db->flush();
    bool ok = false;
    m_db->exec(QStringLiteral("DELETE FROM downloads WHERE id=?"), {id}, &ok);
    return ok;
}

void DownloadsDao::updateProgress(const QString &id, int state, qint64 done, const QString &statusText)
{
    m_db->deferWrite([=](QSqlDatabase &db) {
        QSqlQuery q(db);
        q.prepare(QStringLiteral("UPDATE downloads SET state=?, done=?, status_text=? WHERE id=?"));
        q.addBindValue(state);
        q.addBindValue(done);
        q.addBindValue(statusText);
        q.addBindValue(id);
        q.exec();
    });
}

void DownloadsDao::setEngineState(const QString &id, const QByteArray &b)
{
    m_db->deferWrite([=](QSqlDatabase &db) {
        QSqlQuery q(db);
        q.prepare(QStringLiteral("UPDATE downloads SET engine_state=? WHERE id=?"));
        q.addBindValue(blob(b));
        q.addBindValue(id);
        q.exec();
    });
}

bool DownloadsDao::findByHashes(const FileHashes &h, DownloadRecord *out) const
{
    if (h.isEmpty()) return false;
    // candidate rows by the indexed kinds, then the exact FileHashes rule
    QStringList where;
    QVariantList binds;
    if (!h.btih.isEmpty()) { where << QStringLiteral("btih=?"); binds << h.btih; }
    if (!h.ed2k.isEmpty()) { where << QStringLiteral("ed2k=?"); binds << h.ed2k; }
    if (!h.sha1.isEmpty()) { where << QStringLiteral("sha1=?"); binds << h.sha1; }
    QSqlQuery q = where.isEmpty() ? m_db->exec(QStringLiteral("SELECT * FROM downloads"))
                                  : m_db->exec(QStringLiteral("SELECT * FROM downloads WHERE ") + where.join(QStringLiteral(" OR ")), binds);
    while (q.next()) {
        DownloadRecord r = readDownload(q);
        if (r.hashes.matches(h)) {
            if (out) *out = r;
            return true;
        }
    }
    if (!where.isEmpty()) { // other kinds (tth, aich, ...) are only in the JSON column
        QSqlQuery all = m_db->exec(QStringLiteral("SELECT * FROM downloads"));
        while (all.next()) {
            DownloadRecord r = readDownload(all);
            if (r.hashes.matches(h)) {
                if (out) *out = r;
                return true;
            }
        }
    }
    return false;
}

QList<SourceRecord> DownloadsDao::sources(const QString &downloadId) const
{
    QList<SourceRecord> out;
    QSqlQuery q = m_db->exec(QStringLiteral("SELECT net,uri,user_id,user_name,address,extra,added,last_seen,failures"
                                            " FROM download_sources WHERE download_id=? ORDER BY id"),
                             {downloadId});
    while (q.next()) {
        SourceRecord r;
        r.downloadId = downloadId;
        r.source.net = Net(q.value(0).toInt());
        r.source.uri = q.value(1).toString();
        r.source.user.net = r.source.net;
        r.source.user.id = q.value(2).toString();
        r.source.user.name = q.value(3).toString();
        r.source.user.address = q.value(4).toString();
        QVariantMap extra = mapFromJson(q.value(5).toString());
        r.source.connection = extra.take(QStringLiteral("_connection")).toString();
        r.source.extra = extra;
        r.added = fromMs(q.value(6));
        r.lastSeen = fromMs(q.value(7));
        r.failures = q.value(8).toInt();
        out << r;
    }
    return out;
}

bool DownloadsDao::addSources(const QString &downloadId, const QList<SearchSource> &sources)
{
    m_db->flush();
    return m_db->transaction([&](QSqlDatabase &db) {
        QSqlQuery q(db);
        q.prepare(QStringLiteral("INSERT INTO download_sources (download_id,net,uri,user_id,user_name,address,extra,added,last_seen)"
                                 " VALUES (?,?,?,?,?,?,?,?,?) ON CONFLICT(download_id,net,uri,user_id) DO UPDATE SET"
                                 " user_name=excluded.user_name, address=excluded.address, extra=excluded.extra,"
                                 " last_seen=excluded.last_seen"));
        const qint64 now = nowMs();
        for (const SearchSource &s : sources) {
            QVariantMap extra = s.extra;
            if (!s.connection.isEmpty()) extra.insert(QStringLiteral("_connection"), s.connection);
            q.addBindValue(downloadId);
            q.addBindValue(int(s.net));
            q.addBindValue(nn(s.uri));
            q.addBindValue(nn(s.user.id));
            q.addBindValue(s.user.name);
            q.addBindValue(s.user.address);
            q.addBindValue(mapToJson(extra));
            q.addBindValue(now);
            q.addBindValue(now);
            if (!q.exec()) {
                qCWarning(lcDb) << "addSources:" << q.lastError().text();
                return false;
            }
        }
        return true;
    });
}

bool DownloadsDao::removeSource(const QString &downloadId, Net net, const QString &uri, const QString &userId)
{
    bool ok = false;
    m_db->exec(QStringLiteral("DELETE FROM download_sources WHERE download_id=? AND net=? AND uri=? AND user_id=?"),
               {downloadId, int(net), nn(uri), nn(userId)}, &ok);
    return ok;
}

void DownloadsDao::markSourceFailed(const QString &downloadId, Net net, const QString &uri, const QString &userId)
{
    m_db->deferWrite([=](QSqlDatabase &db) {
        QSqlQuery q(db);
        q.prepare(QStringLiteral("UPDATE download_sources SET failures=failures+1 WHERE download_id=? AND net=? AND uri=? AND user_id=?"));
        q.addBindValue(downloadId);
        q.addBindValue(int(net));
        q.addBindValue(nn(uri));
        q.addBindValue(nn(userId));
        q.exec();
    });
}

// ================================================================ library
QList<SharedFolder> LibraryDao::folders() const
{
    QList<SharedFolder> out;
    QSqlQuery q = m_db->exec(QStringLiteral("SELECT path, recursive FROM shared_folders ORDER BY added, path"));
    while (q.next()) {
        SharedFolder f;
        f.path = q.value(0).toString();
        f.recursive = q.value(1).toBool();
        out << f;
    }
    return out;
}

bool LibraryDao::addFolder(const QString &path, bool recursive)
{
    bool ok = false;
    m_db->exec(QStringLiteral("INSERT INTO shared_folders (path, recursive, added) VALUES (?,?,?)"
                              " ON CONFLICT(path) DO UPDATE SET recursive=excluded.recursive"),
               {path, recursive ? 1 : 0, nowMs()}, &ok);
    return ok;
}

bool LibraryDao::removeFolder(const QString &path)
{
    bool ok = false;
    m_db->exec(QStringLiteral("DELETE FROM shared_folders WHERE path=?"), {path}, &ok);
    return ok;
}

QStringList LibraryDao::excluded() const
{
    QStringList out;
    QSqlQuery q = m_db->exec(QStringLiteral("SELECT path FROM library_excluded"));
    while (q.next()) out << q.value(0).toString();
    return out;
}

bool LibraryDao::setExcluded(const QString &path, bool excluded)
{
    bool ok = false;
    if (excluded)
        m_db->exec(QStringLiteral("INSERT OR IGNORE INTO library_excluded (path, added) VALUES (?,?)"), {path, nowMs()}, &ok);
    else
        m_db->exec(QStringLiteral("DELETE FROM library_excluded WHERE path=?"), {path}, &ok);
    return ok;
}

static LibraryRecord readLibrary(const QSqlQuery &q)
{
    const QSqlRecord rec = q.record();
    auto v = [&](const char *name) { return q.value(rec.indexOf(QLatin1String(name))); };
    LibraryRecord r;
    r.path = v("path").toString();
    r.size = v("size").toLongLong();
    r.mtimeMs = v("mtime").toLongLong();
    r.hashes.ed2k = v("ed2k").toByteArray();
    r.hashes.aich = v("aich").toByteArray();
    r.hashes.sha1 = v("sha1").toByteArray();
    r.hashes.tth = v("tth").toByteArray();
    r.hashes.sha256 = v("sha256").toByteArray();
    r.hashes.md5 = v("md5").toByteArray();
    r.ed2kParts = unpackHashList(v("ed2k_parts").toByteArray(), 16);
    r.aichParts = unpackHashList(v("aich_parts").toByteArray(), 20);
    r.durationSec = v("duration").toInt();
    r.bitrateKbps = v("bitrate").toInt();
    r.sampleRateHz = v("samplerate").toInt();
    r.hashedAt = fromMs(v("hashed_at"));
    r.uploaded = v("uploaded").toLongLong();
    r.requests = v("requests").toInt();
    return r;
}

bool LibraryDao::lookup(const QString &path, qint64 size, qint64 mtimeMs, LibraryRecord *out) const
{
    QSqlQuery q = m_db->exec(QStringLiteral("SELECT * FROM library_files WHERE path=? AND size=? AND mtime=?"),
                             {path, size, mtimeMs});
    if (!q.next()) return false;
    if (out) *out = readLibrary(q);
    return true;
}

void LibraryDao::store(const LibraryRecord &r)
{
    m_db->deferWrite([r](QSqlDatabase &db) {
        QSqlQuery q(db);
        q.prepare(QStringLiteral(
            "INSERT INTO library_files (path,size,mtime,ed2k,aich,sha1,tth,sha256,md5,ed2k_parts,aich_parts,"
            "duration,bitrate,samplerate,hashed_at,uploaded,requests) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"
            " ON CONFLICT(path) DO UPDATE SET size=excluded.size, mtime=excluded.mtime, ed2k=excluded.ed2k,"
            " aich=excluded.aich, sha1=excluded.sha1, tth=excluded.tth, sha256=excluded.sha256, md5=excluded.md5,"
            " ed2k_parts=excluded.ed2k_parts, aich_parts=excluded.aich_parts, duration=excluded.duration,"
            " bitrate=excluded.bitrate, samplerate=excluded.samplerate, hashed_at=excluded.hashed_at"));
        q.addBindValue(r.path);
        q.addBindValue(r.size);
        q.addBindValue(r.mtimeMs);
        q.addBindValue(blob(r.hashes.ed2k));
        q.addBindValue(blob(r.hashes.aich));
        q.addBindValue(blob(r.hashes.sha1));
        q.addBindValue(blob(r.hashes.tth));
        q.addBindValue(blob(r.hashes.sha256));
        q.addBindValue(blob(r.hashes.md5));
        q.addBindValue(blob(packHashList(r.ed2kParts)));
        q.addBindValue(blob(packHashList(r.aichParts)));
        q.addBindValue(r.durationSec);
        q.addBindValue(r.bitrateKbps);
        q.addBindValue(r.sampleRateHz);
        q.addBindValue(toMs(r.hashedAt.isValid() ? r.hashedAt : QDateTime::currentDateTime()));
        q.addBindValue(r.uploaded);
        q.addBindValue(r.requests);
        if (!q.exec()) qCWarning(lcDb) << "library store:" << q.lastError().text();
    });
}

void LibraryDao::removeFiles(const QStringList &paths)
{
    if (paths.isEmpty()) return;
    m_db->deferWrite([paths](QSqlDatabase &db) {
        QSqlQuery q(db);
        q.prepare(QStringLiteral("DELETE FROM library_files WHERE path=?"));
        for (const QString &p : paths) {
            q.addBindValue(p);
            q.exec();
        }
    });
}

void LibraryDao::addUpload(const QString &path, qint64 bytes)
{
    m_db->deferWrite([=](QSqlDatabase &db) {
        QSqlQuery q(db);
        q.prepare(QStringLiteral("UPDATE library_files SET uploaded=uploaded+?, requests=requests+1 WHERE path=?"));
        q.addBindValue(bytes);
        q.addBindValue(path);
        q.exec();
    });
}

QList<LibraryRecord> LibraryDao::all() const
{
    QList<LibraryRecord> out;
    QSqlQuery q = m_db->exec(QStringLiteral("SELECT * FROM library_files"));
    while (q.next()) out << readLibrary(q);
    return out;
}

QHash<QString, LibraryRecord> LibraryDao::loadAll(const QString &connectionName)
{
    QHash<QString, LibraryRecord> out;
    QSqlDatabase db = QSqlDatabase::database(connectionName, false);
    if (!db.isOpen()) return out;
    QSqlQuery q(db);
    q.setForwardOnly(true);
    if (!q.exec(QStringLiteral("SELECT * FROM library_files"))) return out;
    while (q.next()) {
        LibraryRecord r = readLibrary(q);
        out.insert(r.path, r);
    }
    return out;
}

// ================================================================ hash links
namespace {
struct KindCol { const char *col; QByteArray FileHashes::*member; };
const KindCol kKinds[] = {
    {"ed2k", &FileHashes::ed2k}, {"aich", &FileHashes::aich}, {"sha1", &FileHashes::sha1},
    {"tth", &FileHashes::tth}, {"sha256", &FileHashes::sha256}, {"md5", &FileHashes::md5},
    {"bt_root", &FileHashes::btFileRoot},
};

HashLinkRecord readLink(const QSqlQuery &q)
{
    const QSqlRecord rec = q.record();
    auto v = [&](const char *name) { return q.value(rec.indexOf(QLatin1String(name))); };
    HashLinkRecord r;
    r.id = v("id").toLongLong();
    r.size = v("size").toLongLong();
    r.name = v("name").toString();
    r.origin = v("origin").toString();
    r.updated = fromMs(v("updated"));
    r.hashes.btih = v("btih").toByteArray();
    r.hashes.btFileIndex = v("bt_index").toInt();
    for (const KindCol &k : kKinds) r.hashes.*(k.member) = v(k.col).toByteArray();
    return r;
}
} // namespace

QList<HashLinkRecord> HashLinksDao::find(const FileHashes &h, qint64 size) const
{
    QList<HashLinkRecord> out;
    QStringList where;
    QVariantList binds;
    for (const KindCol &k : kKinds) {
        const QByteArray &v = h.*(k.member);
        if (!v.isEmpty()) {
            where << QStringLiteral("%1=?").arg(QLatin1String(k.col));
            binds << v;
        }
    }
    if (!h.btih.isEmpty()) {
        where << QStringLiteral("(btih=? AND bt_index=?)");
        binds << h.btih << h.btFileIndex;
    }
    if (where.isEmpty()) return out;
    QSqlQuery q = m_db->exec(QStringLiteral("SELECT * FROM hash_links WHERE ") + where.join(QStringLiteral(" OR ")), binds);
    while (q.next()) {
        HashLinkRecord r = readLink(q);
        if (size >= 0 && r.size >= 0 && r.size != size) continue;
        if (r.hashes.conflicts(h)) continue;
        out << r;
    }
    return out;
}

HashLinkRecord HashLinksDao::upsert(const FileHashes &h, qint64 size, const QString &name, const QString &origin)
{
    m_db->flush();
    HashLinkRecord merged;
    merged.hashes = h;
    merged.size = size;
    merged.name = name;
    merged.origin = origin;
    const QList<HashLinkRecord> rows = find(h, size);
    QList<qint64> obsolete;
    for (const HashLinkRecord &r : rows) {
        if (r.hashes.conflicts(merged.hashes)) continue;
        merged.hashes.mergeFrom(r.hashes);
        if (merged.size < 0) merged.size = r.size;
        if (merged.name.isEmpty()) merged.name = r.name;
        if (merged.id == 0) merged.id = r.id;
        else obsolete << r.id;
    }
    merged.updated = QDateTime::currentDateTime();
    m_db->transaction([&](QSqlDatabase &db) {
        QSqlQuery q(db);
        for (qint64 id : obsolete) {
            q.prepare(QStringLiteral("DELETE FROM hash_links WHERE id=?"));
            q.addBindValue(id);
            q.exec();
        }
        if (merged.id) {
            q.prepare(QStringLiteral("UPDATE hash_links SET size=?, name=?, btih=?, bt_index=?, bt_root=?, ed2k=?, aich=?,"
                                     " sha1=?, tth=?, sha256=?, md5=?, origin=?, updated=? WHERE id=?"));
        } else {
            q.prepare(QStringLiteral("INSERT INTO hash_links (size, name, btih, bt_index, bt_root, ed2k, aich, sha1, tth,"
                                     " sha256, md5, origin, updated) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)"));
        }
        const FileHashes &m = merged.hashes;
        q.addBindValue(merged.size);
        q.addBindValue(merged.name);
        q.addBindValue(blob(m.btih));
        q.addBindValue(m.btFileIndex);
        q.addBindValue(blob(m.btFileRoot));
        q.addBindValue(blob(m.ed2k));
        q.addBindValue(blob(m.aich));
        q.addBindValue(blob(m.sha1));
        q.addBindValue(blob(m.tth));
        q.addBindValue(blob(m.sha256));
        q.addBindValue(blob(m.md5));
        q.addBindValue(merged.origin);
        q.addBindValue(toMs(merged.updated));
        if (merged.id) q.addBindValue(merged.id);
        if (!q.exec()) {
            qCWarning(lcDb) << "hash link upsert:" << q.lastError().text();
            return false;
        }
        if (!merged.id) merged.id = q.lastInsertId().toLongLong();
        return true;
    });
    return merged;
}

FileHashes HashLinksDao::expand(const FileHashes &h, qint64 size) const
{
    FileHashes out = h;
    for (const HashLinkRecord &r : find(h, size))
        if (!r.hashes.conflicts(out)) out.mergeFrom(r.hashes);
    return out;
}

int HashLinksDao::count() const
{
    QSqlQuery q = m_db->exec(QStringLiteral("SELECT COUNT(*) FROM hash_links"));
    return q.next() ? q.value(0).toInt() : 0;
}

// ================================================================ users / chat / search history
QList<HotlistEntry> UserListsDao::hotlist() const
{
    QList<HotlistEntry> out;
    QSqlQuery q = m_db->exec(QStringLiteral("SELECT net,user_id,name,address,note,files,connection,last_seen"
                                            " FROM hotlist ORDER BY sort_order, added"));
    while (q.next()) {
        HotlistEntry e;
        e.user.net = Net(q.value(0).toInt());
        e.user.id = q.value(1).toString();
        e.user.name = q.value(2).toString();
        e.user.address = q.value(3).toString();
        e.note = q.value(4).toString();
        e.files = q.value(5).toInt();
        e.connection = q.value(6).toString();
        e.lastSeen = fromMs(q.value(7));
        out << e;
    }
    return out;
}

bool UserListsDao::addHotlist(const HotlistEntry &e)
{
    bool ok = false;
    m_db->exec(QStringLiteral("INSERT INTO hotlist (ukey,net,user_id,name,address,note,files,connection,last_seen,added)"
                              " VALUES (?,?,?,?,?,?,?,?,?,?) ON CONFLICT(ukey) DO UPDATE SET name=excluded.name,"
                              " address=excluded.address"),
               {userKey(e.user), int(e.user.net), nn(e.user.id), e.user.name, e.user.address, e.note, e.files,
                e.connection, toMs(e.lastSeen), nowMs()},
               &ok);
    return ok;
}

bool UserListsDao::updateHotlist(const HotlistEntry &e)
{
    bool ok = false;
    m_db->exec(QStringLiteral("UPDATE hotlist SET name=?, address=?, note=?, files=?, connection=?, last_seen=? WHERE ukey=?"),
               {e.user.name, e.user.address, e.note, e.files, e.connection, toMs(e.lastSeen), userKey(e.user)}, &ok);
    return ok;
}

bool UserListsDao::removeHotlist(const UserRef &u)
{
    bool ok = false;
    m_db->exec(QStringLiteral("DELETE FROM hotlist WHERE ukey=?"), {userKey(u)}, &ok);
    return ok;
}

QList<UserRef> UserListsDao::ignored() const
{
    QList<UserRef> out;
    QSqlQuery q = m_db->exec(QStringLiteral("SELECT net,user_id,name,address FROM ignore_list ORDER BY added"));
    while (q.next()) {
        UserRef u;
        u.net = Net(q.value(0).toInt());
        u.id = q.value(1).toString();
        u.name = q.value(2).toString();
        u.address = q.value(3).toString();
        out << u;
    }
    return out;
}

bool UserListsDao::setIgnored(const UserRef &u, bool on)
{
    bool ok = false;
    if (on)
        m_db->exec(QStringLiteral("INSERT INTO ignore_list (ukey,net,user_id,name,address,added) VALUES (?,?,?,?,?,?)"
                                  " ON CONFLICT(ukey) DO UPDATE SET name=excluded.name"),
                   {userKey(u), int(u.net), nn(u.id), u.name, u.address, nowMs()}, &ok);
    else
        m_db->exec(QStringLiteral("DELETE FROM ignore_list WHERE ukey=?"), {userKey(u)}, &ok);
    return ok;
}

QStringList UserListsDao::favouriteRooms() const
{
    QStringList out;
    QSqlQuery q = m_db->exec(QStringLiteral("SELECT room FROM chat_favourites ORDER BY sort_order, added"));
    while (q.next()) out << q.value(0).toString();
    return out;
}

bool UserListsDao::setFavouriteRoom(const QString &room, bool fav)
{
    bool ok = false;
    if (fav)
        m_db->exec(QStringLiteral("INSERT OR IGNORE INTO chat_favourites (room, added) VALUES (?,?)"), {room, nowMs()}, &ok);
    else
        m_db->exec(QStringLiteral("DELETE FROM chat_favourites WHERE room=?"), {room}, &ok);
    return ok;
}

QStringList UserListsDao::searchHistory(int limit) const
{
    QStringList out;
    QSqlQuery q = m_db->exec(QStringLiteral("SELECT text FROM search_history ORDER BY last_used DESC LIMIT ?"), {limit});
    while (q.next()) out << q.value(0).toString();
    return out;
}

bool UserListsDao::addSearchHistory(const QString &text, int keep)
{
    const QString t = text.trimmed();
    if (t.isEmpty()) return false;
    bool ok = false;
    m_db->exec(QStringLiteral("INSERT INTO search_history (text, last_used, uses) VALUES (?,?,1)"
                              " ON CONFLICT(text) DO UPDATE SET last_used=excluded.last_used, uses=uses+1"),
               {t, nowMs()}, &ok);
    m_db->exec(QStringLiteral("DELETE FROM search_history WHERE text NOT IN"
                              " (SELECT text FROM search_history ORDER BY last_used DESC LIMIT ?)"),
               {qMax(0, keep)});
    return ok;
}

bool UserListsDao::clearSearchHistory()
{
    bool ok = false;
    m_db->exec(QStringLiteral("DELETE FROM search_history"), {}, &ok);
    return ok;
}

// ================================================================ kv
QByteArray KvStore::get(const QString &ns, const QString &key, const QByteArray &def) const
{
    QSqlQuery q = m_db->exec(QStringLiteral("SELECT value FROM kv WHERE ns=? AND key=?"), {ns, key});
    return q.next() ? q.value(0).toByteArray() : def;
}

bool KvStore::put(const QString &ns, const QString &key, const QByteArray &value)
{
    bool ok = false;
    m_db->exec(QStringLiteral("INSERT INTO kv (ns,key,value,updated) VALUES (?,?,?,?)"
                              " ON CONFLICT(ns,key) DO UPDATE SET value=excluded.value, updated=excluded.updated"),
               {ns, key, value, nowMs()}, &ok);
    return ok;
}

bool KvStore::remove(const QString &ns, const QString &key)
{
    bool ok = false;
    m_db->exec(QStringLiteral("DELETE FROM kv WHERE ns=? AND key=?"), {ns, key}, &ok);
    return ok;
}

QStringList KvStore::keys(const QString &ns) const
{
    QStringList out;
    QSqlQuery q = m_db->exec(QStringLiteral("SELECT key FROM kv WHERE ns=? ORDER BY key"), {ns});
    while (q.next()) out << q.value(0).toString();
    return out;
}

// ================================================================ stats
qint64 StatsDao::get(const QString &key) const
{
    QSqlQuery q = m_db->exec(QStringLiteral("SELECT value FROM stats WHERE key=?"), {key});
    return q.next() ? q.value(0).toLongLong() : 0;
}

bool StatsDao::set(const QString &key, qint64 value)
{
    bool ok = false;
    m_db->exec(QStringLiteral("INSERT INTO stats (key,value) VALUES (?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value"),
               {key, value}, &ok);
    return ok;
}

void StatsDao::add(const QString &key, qint64 delta)
{
    if (delta == 0) return;
    m_db->deferWrite([=](QSqlDatabase &db) {
        QSqlQuery q(db);
        q.prepare(QStringLiteral("INSERT INTO stats (key,value) VALUES (?,?) ON CONFLICT(key) DO UPDATE SET value=value+excluded.value"));
        q.addBindValue(key);
        q.addBindValue(delta);
        q.exec();
    });
}

} // namespace mx

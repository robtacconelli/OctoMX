// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX core: data access objects over core/database.h. All methods run on the main thread
// (the Database's connection). Writes are immediate unless documented as deferred.
#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QVariantMap>

#include "mx/types.h"

namespace mx {

class Database;

// helpers shared by the DAOs (and usable by the engine)
QByteArray packHashList(const QList<QByteArray> &hashes);            // concatenation
QList<QByteArray> unpackHashList(const QByteArray &blob, int hashSize);
QString intListToString(const QList<int> &l);                        // "1,2,5"
QList<int> intListFromString(const QString &s);

// ---------------------------------------------------------------- downloads
struct DownloadRecord {
    QString    id;
    QString    name;
    qint64     size = 0;
    FileHashes hashes;
    int        state = 0;            // mx::TransferState
    QString    statusText;
    QString    errorText;
    QString    saveDir;              // requested directory (empty = incoming dir)
    QString    savePath;             // final absolute path once known
    QString    uri;                  // original link / .torrent path
    QList<int> wantedFiles;
    int        priority = 0;
    bool       isBundle = false;
    QList<BundleFile> files;
    qint64     done = 0;
    qint64     uploaded = 0;
    QDateTime  added;
    QDateTime  completed;
    QByteArray engineState;          // opaque blob owned by the download engine
    int        sortOrder = 0;
};

struct SourceRecord {
    QString     downloadId;
    SearchSource source;             // net, user, uri, connection, extra ...
    QDateTime   added;
    QDateTime   lastSeen;
    int         failures = 0;
};

class DownloadsDao {
public:
    explicit DownloadsDao(Database *db) : m_db(db) {}
    QList<DownloadRecord> all() const;                      // ordered by sort_order, added
    bool get(const QString &id, DownloadRecord *out) const;
    bool upsert(const DownloadRecord &r);
    bool remove(const QString &id);                         // cascades to sources
    // deferred (batched) progress / state updates
    void updateProgress(const QString &id, int state, qint64 done, const QString &statusText);
    void setEngineState(const QString &id, const QByteArray &blob);   // deferred
    // first download whose hashes match (FileHashes::matches); false if none
    bool findByHashes(const FileHashes &h, DownloadRecord *out) const;

    QList<SourceRecord> sources(const QString &downloadId) const;
    bool addSources(const QString &downloadId, const QList<SearchSource> &sources); // insert or refresh
    bool removeSource(const QString &downloadId, Net net, const QString &uri, const QString &userId);
    void markSourceFailed(const QString &downloadId, Net net, const QString &uri, const QString &userId); // deferred
private:
    Database *m_db;
};

// ---------------------------------------------------------------- library + hash cache
struct LibraryRecord {
    QString    path;
    qint64     size = 0;
    qint64     mtimeMs = 0;
    FileHashes hashes;               // ed2k, aich, sha1, tth, sha256, md5
    QList<QByteArray> ed2kParts;     // empty for single-part files
    QList<QByteArray> aichParts;
    int        durationSec = -1;
    int        bitrateKbps = -1;
    int        sampleRateHz = -1;
    QDateTime  hashedAt;
    qint64     uploaded = 0;
    int        requests = 0;
};

class LibraryDao {
public:
    explicit LibraryDao(Database *db) : m_db(db) {}
    QList<SharedFolder> folders() const;
    bool addFolder(const QString &path, bool recursive);
    bool removeFolder(const QString &path);
    QStringList excluded() const;
    bool setExcluded(const QString &path, bool excluded);
    // hash cache
    bool lookup(const QString &path, qint64 size, qint64 mtimeMs, LibraryRecord *out) const;
    void store(const LibraryRecord &r);                      // deferred
    void removeFiles(const QStringList &paths);              // deferred
    void addUpload(const QString &path, qint64 bytes);       // deferred: uploaded += bytes, requests++
    QList<LibraryRecord> all() const;
    // cache lookup from a worker thread with its own connection (see Database::openThreadConnection)
    static QHash<QString, LibraryRecord> loadAll(const QString &connectionName);
private:
    Database *m_db;
};

// ---------------------------------------------------------------- hash links
struct HashLinkRecord {
    qint64     id = 0;
    FileHashes hashes;
    qint64     size = -1;
    QString    name;
    QString    origin;               // "library", "download", "overlay", ...
    QDateTime  updated;
};

class HashLinksDao {
public:
    explicit HashLinksDao(Database *db) : m_db(db) {}
    // Rows sharing any hash with h (and not conflicting); size < 0 = any size.
    QList<HashLinkRecord> find(const FileHashes &h, qint64 size = -1) const;
    // Merge into the row(s) matching any hash (combining all known hashes), or insert a new row.
    // Returns the merged record.
    HashLinkRecord upsert(const FileHashes &h, qint64 size, const QString &name, const QString &origin);
    // All hashes known for h (h merged with every linked row).
    FileHashes expand(const FileHashes &h, qint64 size = -1) const;
    int count() const;
private:
    Database *m_db;
};

// ---------------------------------------------------------------- users, chat, search history
class UserListsDao {
public:
    explicit UserListsDao(Database *db) : m_db(db) {}
    QList<HotlistEntry> hotlist() const;
    bool addHotlist(const HotlistEntry &e);
    bool updateHotlist(const HotlistEntry &e);
    bool removeHotlist(const UserRef &u);
    QList<UserRef> ignored() const;
    bool setIgnored(const UserRef &u, bool on);
    QStringList favouriteRooms() const;
    bool setFavouriteRoom(const QString &room, bool fav);
    QStringList searchHistory(int limit = 100) const;       // most recent first
    bool addSearchHistory(const QString &text, int keep);    // trims to `keep` entries
    bool clearSearchHistory();
private:
    Database *m_db;
};

// ---------------------------------------------------------------- generic per-plugin blob store
class KvStore {
public:
    explicit KvStore(Database *db) : m_db(db) {}
    QByteArray get(const QString &ns, const QString &key, const QByteArray &def = QByteArray()) const;
    bool put(const QString &ns, const QString &key, const QByteArray &value);
    bool remove(const QString &ns, const QString &key);
    QStringList keys(const QString &ns) const;
private:
    Database *m_db;
};

// ---------------------------------------------------------------- all-time statistics
class StatsDao {
public:
    explicit StatsDao(Database *db) : m_db(db) {}
    qint64 get(const QString &key) const;
    bool set(const QString &key, qint64 value);
    void add(const QString &key, qint64 delta);              // deferred
private:
    Database *m_db;
};

} // namespace mx

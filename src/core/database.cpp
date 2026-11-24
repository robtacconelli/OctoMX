// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/database.h"
#include "core/log.h"
#include "mx/durable_file.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QUuid>
#include <QtSql/QSqlError>

namespace mx {

namespace {

// Versioned migrations: index i upgrades user_version i → i+1. Never edit a released step; append.
const QList<QStringList> &migrations()
{
    static const QList<QStringList> m = {
        // ---- v1
        {
            QStringLiteral(
                "CREATE TABLE downloads ("
                " id TEXT PRIMARY KEY, name TEXT NOT NULL DEFAULT '', size INTEGER NOT NULL DEFAULT 0,"
                " hashes TEXT NOT NULL DEFAULT '{}', btih BLOB, ed2k BLOB, sha1 BLOB,"
                " state INTEGER NOT NULL DEFAULT 0, status_text TEXT, error_text TEXT,"
                " save_dir TEXT, save_path TEXT, uri TEXT, wanted TEXT, priority INTEGER NOT NULL DEFAULT 0,"
                " is_bundle INTEGER NOT NULL DEFAULT 0, files TEXT, done INTEGER NOT NULL DEFAULT 0,"
                " uploaded INTEGER NOT NULL DEFAULT 0,"
                " added INTEGER, completed INTEGER, engine_state BLOB, sort_order INTEGER NOT NULL DEFAULT 0)"),
            QStringLiteral("CREATE INDEX downloads_btih ON downloads(btih)"),
            QStringLiteral("CREATE INDEX downloads_ed2k ON downloads(ed2k)"),
            QStringLiteral("CREATE INDEX downloads_sha1 ON downloads(sha1)"),
            QStringLiteral(
                "CREATE TABLE download_sources ("
                " id INTEGER PRIMARY KEY AUTOINCREMENT,"
                " download_id TEXT NOT NULL REFERENCES downloads(id) ON DELETE CASCADE,"
                " net INTEGER NOT NULL, uri TEXT NOT NULL DEFAULT '', user_id TEXT NOT NULL DEFAULT '',"
                " user_name TEXT, address TEXT, extra TEXT, added INTEGER, last_seen INTEGER,"
                " failures INTEGER NOT NULL DEFAULT 0,"
                " UNIQUE(download_id, net, uri, user_id))"),
            QStringLiteral("CREATE INDEX download_sources_dl ON download_sources(download_id)"),
            QStringLiteral("CREATE TABLE shared_folders (path TEXT PRIMARY KEY, recursive INTEGER NOT NULL DEFAULT 1,"
                           " added INTEGER)"),
            QStringLiteral("CREATE TABLE library_excluded (path TEXT PRIMARY KEY, added INTEGER)"),
            QStringLiteral(
                "CREATE TABLE library_files ("
                " path TEXT PRIMARY KEY, size INTEGER NOT NULL, mtime INTEGER NOT NULL,"
                " ed2k BLOB, aich BLOB, sha1 BLOB, tth BLOB, sha256 BLOB, md5 BLOB,"
                " ed2k_parts BLOB, aich_parts BLOB,"
                " duration INTEGER NOT NULL DEFAULT -1, bitrate INTEGER NOT NULL DEFAULT -1,"
                " samplerate INTEGER NOT NULL DEFAULT -1, hashed_at INTEGER,"
                " uploaded INTEGER NOT NULL DEFAULT 0, requests INTEGER NOT NULL DEFAULT 0)"),
            QStringLiteral("CREATE INDEX library_files_ed2k ON library_files(ed2k)"),
            QStringLiteral("CREATE INDEX library_files_sha1 ON library_files(sha1)"),
            QStringLiteral("CREATE INDEX library_files_tth ON library_files(tth)"),
            QStringLiteral("CREATE INDEX library_files_aich ON library_files(aich)"),
            QStringLiteral("CREATE INDEX library_files_sha256 ON library_files(sha256)"),
            QStringLiteral(
                "CREATE TABLE hash_links ("
                " id INTEGER PRIMARY KEY AUTOINCREMENT, size INTEGER NOT NULL DEFAULT -1, name TEXT,"
                " btih BLOB, bt_index INTEGER NOT NULL DEFAULT -1, bt_root BLOB, ed2k BLOB, aich BLOB,"
                " sha1 BLOB, tth BLOB, sha256 BLOB, md5 BLOB, origin TEXT, updated INTEGER)"),
            QStringLiteral("CREATE INDEX hash_links_btih ON hash_links(btih)"),
            QStringLiteral("CREATE INDEX hash_links_ed2k ON hash_links(ed2k)"),
            QStringLiteral("CREATE INDEX hash_links_sha1 ON hash_links(sha1)"),
            QStringLiteral("CREATE INDEX hash_links_tth ON hash_links(tth)"),
            QStringLiteral("CREATE INDEX hash_links_aich ON hash_links(aich)"),
            QStringLiteral("CREATE INDEX hash_links_sha256 ON hash_links(sha256)"),
            QStringLiteral("CREATE INDEX hash_links_md5 ON hash_links(md5)"),
            QStringLiteral("CREATE INDEX hash_links_bt_root ON hash_links(bt_root)"),
            QStringLiteral(
                "CREATE TABLE hotlist (ukey TEXT PRIMARY KEY, net INTEGER NOT NULL, user_id TEXT NOT NULL,"
                " name TEXT, address TEXT, note TEXT, files INTEGER NOT NULL DEFAULT 0, connection TEXT,"
                " last_seen INTEGER, added INTEGER, sort_order INTEGER NOT NULL DEFAULT 0)"),
            QStringLiteral(
                "CREATE TABLE ignore_list (ukey TEXT PRIMARY KEY, net INTEGER NOT NULL, user_id TEXT NOT NULL,"
                " name TEXT, address TEXT, added INTEGER)"),
            QStringLiteral("CREATE TABLE search_history (text TEXT PRIMARY KEY, last_used INTEGER NOT NULL,"
                           " uses INTEGER NOT NULL DEFAULT 1)"),
            QStringLiteral("CREATE TABLE chat_favourites (room TEXT PRIMARY KEY, added INTEGER,"
                           " sort_order INTEGER NOT NULL DEFAULT 0)"),
            QStringLiteral("CREATE TABLE kv (ns TEXT NOT NULL, key TEXT NOT NULL, value BLOB, updated INTEGER,"
                           " PRIMARY KEY(ns, key))"),
            QStringLiteral("CREATE TABLE stats (key TEXT PRIMARY KEY, value INTEGER NOT NULL DEFAULT 0)"),
        },
    };
    return m;
}

void applyPragmas(QSqlDatabase &db)
{
    QSqlQuery q(db);
    q.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    q.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
    q.exec(QStringLiteral("PRAGMA foreign_keys=ON"));
    q.exec(QStringLiteral("PRAGMA busy_timeout=5000"));
    q.exec(QStringLiteral("PRAGMA temp_store=MEMORY"));
}

} // namespace

Database::Database(QObject *parent)
    : QObject(parent)
{
    m_flushTimer.setSingleShot(true);
    m_flushTimer.setInterval(200);
    connect(&m_flushTimer, &QTimer::timeout, this, &Database::flush);
}

Database::~Database()
{
    close();
}

int Database::latestSchemaVersion()
{
    return migrations().size();
}

bool Database::open(const QString &path, QString *error)
{
    close();
    m_recovered = NotNeeded;
    QString why;
    if (openOnce(path, &why) && healthy(&why)) return true;
    // [safe-1] damaged: kept aside for a look, the last good copy comes back (the model: the settings
    // file's .bak); a copy that fails too gives way to a new database
    closeConnection(false);
    const QString bad = path + QStringLiteral(".bad"), bak = path + QStringLiteral(".bak");
    qCCritical(lcDb) << "the database" << path << "is damaged (" << why << "): kept as" << bad;
    for (const QString &x : {QString(), QStringLiteral("-wal"), QStringLiteral("-shm")}) {
        QFile::remove(bad + x);
        QFile::rename(path + x, bad + x);
    }
    if (QFileInfo::exists(bak) && QFile::copy(bak, path)) {
        QString why2;
        if (openOnce(path, &why2) && healthy(&why2)) {
            m_recovered = FromBackup;
            qCWarning(lcDb) << "the database was restored from its last good copy" << bak;
            return true;
        }
        closeConnection(false);
        qCCritical(lcDb) << "the last good copy is damaged too (" << why2 << "): starting with a new database";
        QFile::remove(path);
    }
    m_recovered = Fresh;
    const bool ok = openOnce(path, error);
    if (ok) qCWarning(lcDb) << "started a new database" << path;
    return ok;
}

// [safe-1] PRAGMA quick_check: the pages and the structure, quick enough for every start
bool Database::healthy(QString *why)
{
    QSqlQuery q(db());
    if (!q.exec(QStringLiteral("PRAGMA quick_check")) || !q.next()) {
        *why = q.lastError().text();
        return false;
    }
    const QString r = q.value(0).toString();
    if (r == QLatin1String("ok")) return true;
    *why = r.left(200);
    return false;
}

bool Database::openOnce(const QString &path, QString *error)
{
    closeConnection(false);
    m_path = path;
    m_conn = QStringLiteral("mxdb-main-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_conn);
    db.setDatabaseName(path);
    if (!db.open()) {
        if (error) *error = db.lastError().text();
        qCWarning(lcDb) << "cannot open" << path << db.lastError().text();
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_conn);
        m_conn.clear();
        return false;
    }
    applyPragmas(db);
    if (!migrate(error)) {
        closeConnection(false);
        return false;
    }
    qCInfo(lcDb) << "opened" << path << "schema version" << schemaVersion();
    emit opened();
    return true;
}

bool Database::isOpen() const
{
    return !m_conn.isEmpty() && QSqlDatabase::database(m_conn, false).isOpen();
}

void Database::close()
{
    closeConnection(true);
}

void Database::closeConnection(bool backup)
{
    if (m_conn.isEmpty()) return;
    if (backup) flush();
    {
        QSqlDatabase db = QSqlDatabase::database(m_conn, false);
        if (db.isOpen()) {
            if (backup) {
                QSqlQuery(db).exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)"));
                // [safe-1] the last good copy (a consistent snapshot, synced, then put in place)
                const QString bak = m_path + QStringLiteral(".bak"), tmp = bak + QStringLiteral(".new");
                QFile::remove(tmp);
                QSqlQuery v(db);
                v.prepare(QStringLiteral("VACUUM INTO ?"));
                v.addBindValue(tmp);
                if (v.exec() && durable::syncPath(tmp)) {
                    QFile::remove(bak);
                    QFile::rename(tmp, bak);
                    durable::syncDir(QFileInfo(bak).absolutePath());
                } else {
                    qCWarning(lcDb) << "cannot write the database copy" << tmp << v.lastError().text();
                    QFile::remove(tmp);
                }
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(m_conn);
    m_conn.clear();
}

QSqlDatabase Database::db() const
{
    return QSqlDatabase::database(m_conn, false);
}

int Database::schemaVersion() const
{
    QSqlQuery q(db());
    if (q.exec(QStringLiteral("PRAGMA user_version")) && q.next()) return q.value(0).toInt();
    return -1;
}

bool Database::migrate(QString *error)
{
    QSqlDatabase d = db();
    const int have = schemaVersion();
    const auto &steps = migrations();
    if (have < 0) {   // [safe-1] not readable: not a database, or a damaged one
        if (error) *error = QStringLiteral("cannot read the schema version: %1").arg(d.lastError().text());
        return false;
    }
    if (have > steps.size()) {
        qCWarning(lcDb) << "database schema" << have << "is newer than this build (" << steps.size()
                        << "); continuing read/write with known tables";
        return true;
    }
    for (int v = have; v < steps.size(); ++v) {
        if (!d.transaction()) {
            if (error) *error = d.lastError().text();
            return false;
        }
        QSqlQuery q(d);
        for (const QString &sql : steps.at(v)) {
            if (!q.exec(sql)) {
                if (error) *error = QStringLiteral("migration %1: %2 (%3)").arg(v + 1).arg(q.lastError().text(), sql);
                qCWarning(lcDb) << "migration failed" << v + 1 << q.lastError().text() << sql;
                d.rollback();
                return false;
            }
        }
        if (!q.exec(QStringLiteral("PRAGMA user_version=%1").arg(v + 1)) || !d.commit()) {
            if (error) *error = d.lastError().text();
            d.rollback();
            return false;
        }
        qCInfo(lcDb) << "migrated database to version" << v + 1;
    }
    return true;
}

bool Database::transaction(const std::function<bool(QSqlDatabase &)> &fn)
{
    QSqlDatabase d = db();
    if (!d.isOpen()) return false;
    if (!d.transaction()) return false;
    if (fn(d)) return d.commit();
    d.rollback();
    return false;
}

void Database::deferWrite(const std::function<void(QSqlDatabase &)> &fn)
{
    m_pending.append(fn);
    if (!m_flushTimer.isActive()) m_flushTimer.start();
}

void Database::flush()
{
    m_flushTimer.stop();
    if (m_pending.isEmpty()) return;
    const auto pending = std::move(m_pending);
    m_pending.clear();
    QSqlDatabase d = db();
    if (!d.isOpen()) return;
    const bool tx = d.transaction();
    for (const auto &fn : pending) fn(d);
    if (tx && !d.commit()) {
        qCWarning(lcDb) << "deferred commit failed" << d.lastError().text();
        d.rollback();
    }
}

QSqlQuery Database::exec(const QString &sql, const QVariantList &binds, bool *ok) const
{
    QSqlQuery q(db());
    bool r = q.prepare(sql);
    if (r) {
        for (const QVariant &b : binds) q.addBindValue(b);
        r = q.exec();
    }
    if (!r) qCWarning(lcDb) << "query failed:" << q.lastError().text() << sql;
    if (ok) *ok = r;
    return q;
}

QSqlDatabase Database::openThreadConnection(const QString &dbPath, const QString &connectionName)
{
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    db.setDatabaseName(dbPath);
    if (!db.open()) {
        qCWarning(lcDb) << "thread connection failed" << db.lastError().text();
        return QSqlDatabase();
    }
    applyPragmas(db);
    return db;
}

void Database::closeThreadConnection(const QString &connectionName)
{
    {
        QSqlDatabase db = QSqlDatabase::database(connectionName, false);
        if (db.isOpen()) db.close();
    }
    QSqlDatabase::removeDatabase(connectionName);
}

} // namespace mx

// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX core: SQLite database <profile>/octomx.db (winmx.db in a legacy profile; QtSql, WAL, foreign keys, migrations).
//
// Threading model: the Database object and its main connection live on the main thread; every
// DAO (core/dao.h) uses that connection. Statements are small and indexed (WAL, synchronous=NORMAL),
// so they take well under a millisecond. Frequent writes (download progress, statistics, hash
// results) go through deferWrite(): they are queued and executed together in ONE transaction
// ~200 ms later (or on flush()/close()). Worker threads that need to READ (e.g. the library
// scanner looking up the hash cache) open their own connection with openThreadConnection().
#pragma once

#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QTimer>
#include <QtSql/QSqlDatabase>
#include <QtSql/QSqlQuery>

#include <functional>

namespace mx {

class Database : public QObject {
    Q_OBJECT
public:
    explicit Database(QObject *parent = nullptr);
    ~Database() override;

    bool open(const QString &path, QString *error = nullptr);  // creates + migrates
    void close();                                               // flushes deferred writes
    bool isOpen() const;
    QString path() const { return m_path; }
    QSqlDatabase db() const;                                    // main-thread connection

    int schemaVersion() const;                                  // PRAGMA user_version
    static int latestSchemaVersion();

    // Run fn inside a transaction (commit if it returns true, roll back otherwise).
    bool transaction(const std::function<bool(QSqlDatabase &)> &fn);
    // Queue a write; all queued writes run in one transaction after `delayMs` (default 200).
    void deferWrite(const std::function<void(QSqlDatabase &)> &fn);
    void flush();                                               // run queued writes now

    // Prepare + bind positional values + exec on the main connection. Logs failures.
    QSqlQuery exec(const QString &sql, const QVariantList &binds = {}, bool *ok = nullptr) const;

    // A separate connection for the calling (worker) thread. Close it with closeThreadConnection()
    // before the thread ends. Returns an invalid QSqlDatabase on failure.
    static QSqlDatabase openThreadConnection(const QString &dbPath, const QString &connectionName);
    static void closeThreadConnection(const QString &connectionName);

signals:
    void opened();

private:
    bool migrate(QString *error);
    QString m_path;
    QString m_conn;
    QList<std::function<void(QSqlDatabase &)>> m_pending;
    QTimer m_flushTimer;
};

} // namespace mx

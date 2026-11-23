// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX engine tests: one "profile": settings, database, upload tracker, optional library,
// plugins and a DownloadEngine that can be torn down and rebuilt on the same profile.
#pragma once

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

#include <memory>

#include "core/database.h"
#include "core/engine/engine.h"
#include "core/library.h"
#include "core/upload_tracker.h"
#include "engine_fakes.h"
#include "mx/settings_keys.h"
#include "mx/simple_settings.h"
#include "mxhash/ed2k.h"

namespace mx::etest {

inline QString testRoot()
{
    QString d = qEnvironmentVariable("MX_ENGINE_TEST_DIR");
    if (d.isEmpty()) d = QCoreApplication::applicationDirPath() + QStringLiteral("/engine-test-data");
    d = QDir::cleanPath(QDir::fromNativeSeparators(d));
    QDir().mkpath(d);
    return d;
}

inline QByteArray readFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

inline engine::Tuning fastTuning()
{
    engine::Tuning t;
    t.requestTimeoutMs = 3000;
    t.slowForMs = 1500;
    t.slowHoldMs = 3000;
    t.reservationIdleMs = 1500;
    t.reservationMaxMs = 20000;
    t.sourceRetryBaseMs = 300;
    t.persistIntervalMs = 300;
    t.findSourcesBaseMs = 60000;
    return t;
}

class Env {
public:
    explicit Env(const QString &name, bool withLibrary = false) : withLibrary(withLibrary)
    {
        root = testRoot() + QLatin1Char('/') + name;
        QDir(root).removeRecursively();
        QDir().mkpath(root);
        settings.setValue(QString(keys::transfers::IncomingDir), root + QStringLiteral("/incoming"));
        settings.setValue(QString(keys::transfers::IncompleteDir), root + QStringLiteral("/incomplete"));
        settings.setValue(QString(keys::transfers::MinFreeSpaceMB), 0);
        db = std::make_unique<Database>();
        QString err;
        QVERIFY2(db->open(root + QStringLiteral("/octomx.db"), &err), qPrintable(err));
        uploads = std::make_unique<UploadTracker>(&settings);
        if (withLibrary) library = std::make_unique<Library>(&settings, db.get());
    }
    ~Env()
    {
        engineDown(true);
        if (library) library->stop(2000);
        library.reset();
        uploads.reset();
        if (db) db->close();
        qDeleteAll(plugins);
    }

    TestPlugin *addPlugin(Net net, int caps)
    {
        auto *p = new TestPlugin(net, caps);
        p->start();
        plugins << p;
        return p;
    }
    void engineUp(TorrentBackend *backend = nullptr, NetworkPlugin *torrentPlugin = nullptr)
    {
        EngineContext ctx;
        ctx.settings = &settings;
        ctx.db = db.get();
        ctx.library = library.get();
        ctx.uploads = uploads.get();
        for (NetworkPlugin *p : std::as_const(plugins)) ctx.plugins << p;
        if (torrentPlugin && !ctx.plugins.contains(torrentPlugin)) ctx.plugins << torrentPlugin;
        ctx.torrents = backend;
        ctx.torrentPlugin = torrentPlugin;
        ctx.profileDir = root;
        engine = std::make_unique<DownloadEngine>(ctx);
        engine->setTuning(tuning);
        completed = std::make_unique<QSignalSpy>(engine.get(), &TransferService::downloadCompleted);
        if (library && !libraryStarted) { library->start(); libraryStarted = true; }
        engine->start();
    }
    // clean = shutdown() first; otherwise a kill-style teardown (only what was persisted survives)
    void engineDown(bool clean)
    {
        if (!engine) return;
        if (clean) engine->shutdown();
        completed.reset();
        engine.reset();
        if (!clean) db->flush();   // what the DB already queued reaches disk anyway (WAL)
        QCoreApplication::processEvents();
    }
    TransferInfo info(const QString &id) const { return engine->transfer(id); }
    bool waitState(const QString &id, TransferState st, int ms)
    {
        return QTest::qWaitFor([&] { return engine->transfer(id).state == st; }, ms);
    }
    bool waitDone(const QString &id, int ms) { return waitState(id, TransferState::Completed, ms); }

    QString root;
    bool withLibrary = false;
    bool libraryStarted = false;
    SimpleSettings settings;
    std::unique_ptr<Database> db;
    std::unique_ptr<UploadTracker> uploads;
    std::unique_ptr<Library> library;
    QList<TestPlugin *> plugins;
    std::unique_ptr<DownloadEngine> engine;
    std::unique_ptr<QSignalSpy> completed;
    engine::Tuning tuning = fastTuning();
};

inline FileHashes hashesOf(const QByteArray &data, int kinds)
{
    FileHashes h;
    if (kinds & 1) h.sha1 = QCryptographicHash::hash(data, QCryptographicHash::Sha1);
    if (kinds & 2) {
        hash::Ed2kHasher e;
        e.update(data);
        h.ed2k = e.finish();
    }
    if (kinds & 4) h.sha256 = QCryptographicHash::hash(data, QCryptographicHash::Sha256);
    return h;
}

inline QList<QByteArray> ed2kParts(const QByteArray &data)
{
    hash::Ed2kHasher e;
    e.update(data);
    e.finish();
    return e.partHashes();
}

} // namespace mx::etest

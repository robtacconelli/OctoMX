// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/library.h"
#include "core/dao.h"
#include "core/database.h"
#include "core/log.h"
#include "core/media_info.h"

#include "mx/settings_keys.h"
#include "mxhash/multi_hasher.h"

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDeadlineTimer>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QLocale>
#include <QtCore/QRegularExpression>
#include <QtCore/QStandardPaths>
#include <QtCore/QUuid>

namespace mx {

struct Library::ScanItem {
    QString path;
    qint64 size = 0;
    qint64 mtimeMs = 0;
    bool cached = false;
    LibraryRecord rec;
};

struct Library::ScanResult {
    QList<ScanItem> items;
    QStringList dirs;
    QStringList stale;
};

struct Library::HashResult {
    QString path;
    bool ok = false;
    QString error;
    qint64 size = 0;
    qint64 mtimeMs = 0;
    FileHashes hashes;
    QList<QByteArray> ed2kParts;
    QList<QByteArray> aichParts;
    MediaInfo media;
};

namespace {

QString normPath(const QString &p)
{
    if (p.isEmpty()) return p;
    return QDir::cleanPath(QDir::fromNativeSeparators(QFileInfo(p).absoluteFilePath()));
}

bool isUnder(const QString &path, const QString &dir)
{
    if (dir.isEmpty()) return false;
    if (path == dir) return true;
    const QString d = dir.endsWith(QLatin1Char('/')) ? dir : dir + QLatin1Char('/');
    return path.startsWith(d);
}

QList<QRegularExpression> compilePatterns(const QStringList &patterns)
{
    QList<QRegularExpression> out;
    for (const QString &p : patterns) {
        if (p.trimmed().isEmpty()) continue;
        out << QRegularExpression(QRegularExpression::wildcardToRegularExpression(p.trimmed()),
                                  QRegularExpression::CaseInsensitiveOption);
    }
    return out;
}

QString humanSize(qint64 bytes)
{
    return QLocale::c().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

void addIndex(QHash<QByteArray, QString> &idx, char kind, const QByteArray &h, const QString &path)
{
    if (!h.isEmpty()) idx.insert(kind + h, path);
}

} // namespace

Library::Library(Settings *settings, Database *db, QObject *parent)
    : LibraryService(parent)
    , m_settings(settings)
    , m_db(db)
    , m_dao(new LibraryDao(db))
{
    // [ui-f] once: store the new Shared File Types ids instead of the WinMX 3.53 ones (same files shared)
    const QVariant types = m_settings->value(keys::library::SharedTypes);
    if (types.isValid() && isLegacySharedTypeList(types.toStringList()))
        m_settings->setValue(keys::library::SharedTypes,
                             normalizeSharedTypeIds(types.toStringList(), m_settings->b(keys::library::ShareUnlistedTypes)));
    m_scanPool.setMaxThreadCount(1);
    m_hashPool.setMaxThreadCount(qBound(1, m_settings->i(keys::library::HashThreads), 8));
    m_hashPool.setThreadPriority(QThread::LowPriority);
    m_scanPool.setThreadPriority(QThread::LowPriority);

    m_updateTimer.setSingleShot(true);
    m_updateTimer.setInterval(500);
    connect(&m_updateTimer, &QTimer::timeout, this, [this] { emit filesChanged(); emit foldersChanged(); });
    m_pushTimer.setSingleShot(true);
    m_pushTimer.setInterval(2000);
    connect(&m_pushTimer, &QTimer::timeout, this, [this] { emit sharedFilesUpdated(sharedFiles()); });
    m_watchDebounce.setSingleShot(true);
    m_watchDebounce.setInterval(3000);
    connect(&m_watchDebounce, &QTimer::timeout, this, &Library::startScan);
    connect(&m_rescanTimer, &QTimer::timeout, this, &Library::startScan);
    connect(&m_scanWatcher, &QFutureWatcher<ScanResult *>::finished, this, &Library::onScanDone);

    connect(m_settings, &Settings::changed, this, [this](const QString &key, const QVariant &) {
        if (!m_started || m_stopped) return;
        if (key == keys::transfers::ShareCompleted || key == keys::transfers::IncomingDir
            || key == keys::library::ExcludePatterns || key == keys::library::ShareHidden
            || key == keys::library::SharedTypes || key == keys::library::OtherExtensions
            || key == keys::library::ShareUnlistedTypes) {
            emit foldersChanged();
            startScan();
        } else if (key == keys::library::RescanIntervalMin) {
            const int min = m_settings->i(keys::library::RescanIntervalMin);
            if (min > 0) m_rescanTimer.start(min * 60 * 1000);
            else m_rescanTimer.stop();
        } else if (key == keys::library::HashThreads) {
            m_hashPool.setMaxThreadCount(qBound(1, m_settings->i(keys::library::HashThreads), 8));
            pumpHashQueue();
        } else if (key == keys::library::WatchFolders) {
            startScan(); // rebuilds the watcher list
        }
    });
}

Library::~Library()
{
    stop(5000);
    if (m_scanning) {
        m_scanWatcher.waitForFinished();
        delete m_scanWatcher.result();
    }
    delete m_dao;
}

void Library::start()
{
    if (m_started) return;
    m_started = true;
    m_stopped = false;
    m_cancel = false;
    m_db->flush();   // pending hash-cache writes of an earlier instance
    m_folders.clear();
    for (const SharedFolder &f : m_dao->folders()) {
        SharedFolder c = f;
        c.path = normPath(f.path);
        m_folders << c;
    }
    for (const QString &p : m_dao->excluded()) m_excluded.insert(normPath(p));
    // seed the in-memory list from the cache so plugins get the share list immediately
    {
        QWriteLocker l(&m_lock);
        const QList<SharedFolder> folders = effectiveFolders();
        for (const LibraryRecord &r : m_dao->all()) {
            bool under = false;
            for (const SharedFolder &f : folders) {
                if (isUnder(r.path, f.path)) { under = true; break; }
            }
            if (!under || r.hashes.ed2k.isEmpty()) continue;
            Entry e;
            e.file.path = r.path;
            e.file.name = QFileInfo(r.path).fileName();
            e.file.size = r.size;
            e.file.mtime = QDateTime::fromMSecsSinceEpoch(r.mtimeMs);
            e.file.type = mediaTypeForFileName(e.file.name);
            e.file.durationSec = r.durationSec;
            e.file.bitrateKbps = r.bitrateKbps;
            e.file.sampleRateHz = r.sampleRateHz;
            dropNonAudioMedia(e.file.type, &e.file.durationSec, &e.file.bitrateKbps, &e.file.sampleRateHz);
            e.file.hashes = r.hashes;
            e.file.hashed = true;
            e.file.uploadedBytes = r.uploaded;
            e.file.requests = r.requests;
            e.ed2kParts = r.ed2kParts;
            e.aichParts = r.aichParts;
            m_entries.insert(r.path, e);
            indexEntryLocked(e);
        }
    }
    const int min = m_settings->i(keys::library::RescanIntervalMin);
    if (min > 0) m_rescanTimer.start(min * 60 * 1000);
    scheduleUpdate();
    startScan();
}

void Library::stop(int waitMs)
{
    if (m_stopped) return;
    m_stopped = true;
    m_cancel = true;
    m_hashQueue.clear();
    m_queued.clear();
    m_hashPool.clear();
    m_rescanTimer.stop();
    m_watchDebounce.stop();
    if (!m_hashPool.waitForDone(waitMs))
        qCWarning(lcLibrary) << "hash workers did not stop in time";
    m_scanPool.waitForDone(waitMs);
    if (m_pushTimer.isActive()) {
        m_pushTimer.stop();
        emit sharedFilesUpdated(sharedFiles());
    }
    // deliver results that were queued by workers before they stopped, then persist them
    QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
    m_db->flush();
    delete m_watcher;
    m_watcher = nullptr;
}

bool Library::waitIdle(int timeoutMs)
{
    QDeadlineTimer dl(timeoutMs);
    while (!dl.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (!m_scanning && !m_rescanPending && m_inFlight == 0 && m_hashQueue.isEmpty())
            return true;
        QThread::msleep(5);
    }
    return false;
}

// ---------------------------------------------------------------- folders
QList<SharedFolder> Library::effectiveFolders() const
{
    QList<SharedFolder> out = m_folders;
    if (m_settings->b(keys::transfers::ShareCompleted)) {
        const QString inc = normPath(m_settings->s(keys::transfers::IncomingDir));
        bool covered = false;
        for (const SharedFolder &f : out) {
            if (f.path == inc || (f.recursive && isUnder(inc, f.path))) { covered = true; break; }
        }
        if (!covered && !inc.isEmpty()) {
            SharedFolder f;
            f.path = inc;
            f.recursive = true;
            out << f;
        }
    }
    return out;
}

bool Library::isUnderSharedFolder(const QString &path) const
{
    if (!typeAllowed(QFileInfo(path).fileName())) return false;   // [glue-3] Shared File Types
    for (const SharedFolder &f : effectiveFolders()) {
        if (f.recursive ? isUnder(path, f.path) : QFileInfo(path).absolutePath() == f.path) {
            for (const QString &ex : m_excluded)
                if (isUnder(path, ex)) return false;
            return true;
        }
    }
    return false;
}

QList<SharedFolder> Library::folders() const
{
    QList<SharedFolder> out = effectiveFolders();
    QReadLocker l(&m_lock);
    for (SharedFolder &f : out) {
        for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it) {
            if (!it->shared) continue;
            if (isUnder(it.key(), f.path)) {
                ++f.fileCount;
                f.totalBytes += it->file.size;
            }
        }
    }
    return out;
}

void Library::addFolder(const QString &path, bool recursive)
{
    const QString p = normPath(path);
    if (p.isEmpty()) return;
    bool found = false;
    for (SharedFolder &f : m_folders) {
        if (f.path == p) { f.recursive = recursive; found = true; }
    }
    if (!found) {
        SharedFolder f;
        f.path = p;
        f.recursive = recursive;
        m_folders << f;
    }
    m_dao->addFolder(p, recursive);
    emit foldersChanged();
    startScan();
}

void Library::removeFolder(const QString &path)
{
    const QString p = normPath(path);
    bool removed = false;
    for (int i = m_folders.size() - 1; i >= 0; --i) {
        if (m_folders.at(i).path == p) { m_folders.removeAt(i); removed = true; }
    }
    if (removed) m_dao->removeFolder(p);
    if (m_settings->b(keys::transfers::ShareCompleted) && normPath(m_settings->s(keys::transfers::IncomingDir)) == p)
        m_settings->setValue(keys::transfers::ShareCompleted, false);
    emit foldersChanged();
    startScan();
}

void Library::rescan()
{
    startScan();
}

void Library::unshareFile(const QString &path)
{
    const QString p = normPath(path);
    if (p.isEmpty()) return;
    m_excluded.insert(p);
    m_dao->setExcluded(p, true);
    {
        QWriteLocker l(&m_lock);
        for (auto it = m_entries.begin(); it != m_entries.end();) {
            if (it->shared && isUnder(it.key(), p)) {
                unindexLocked(*it);
                it = m_entries.erase(it);
            } else {
                ++it;
            }
        }
    }
    m_hashQueue.removeIf([&](const QString &q) { return isUnder(q, p); });
    scheduleUpdate();
}

// [ui-j] deleted from the disk (Shared Files ▸ Delete File...): gone from the index at once, no exclusion
void Library::forgetPaths(const QStringList &paths)
{
    QStringList ps;
    for (const QString &x : paths)
        if (const QString p = normPath(x); !p.isEmpty()) ps << p;
    if (ps.isEmpty()) return;
    auto gone = [&ps](const QString &q) {
        for (const QString &p : ps)
            if (isUnder(q, p)) return true;
        return false;
    };
    {
        QWriteLocker l(&m_lock);
        for (auto it = m_entries.begin(); it != m_entries.end();) {
            if (it->shared && gone(it.key())) {
                unindexLocked(*it);
                it = m_entries.erase(it);
            } else {
                ++it;
            }
        }
    }
    m_hashQueue.removeIf([&](const QString &q) { return gone(q); });
    scheduleUpdate();
}

void Library::reshareFile(const QString &path)
{
    const QString p = normPath(path);
    if (m_excluded.remove(p)) {
        m_dao->setExcluded(p, false);
        startScan();
    }
}

QStringList Library::excludedPaths() const
{
    QStringList l = m_excluded.values();
    l.sort();
    return l;
}

// ---------------------------------------------------------------- [glue-3] types, sub folders, media
QStringList Library::sharedTypes() const
{
    const QVariant v = m_settings->value(keys::library::SharedTypes);
    if (!v.isValid() || v.isNull()) return sharedTypeIds();
    // [ui-f] the WinMX 3.53 ids of older profiles (mp3 ogg otheraudio archive) mean the new groups
    return normalizeSharedTypeIds(v.toStringList(), m_settings->b(keys::library::ShareUnlistedTypes));
}

void Library::setSharedTypes(const QStringList &typeIds)
{
    const QStringList ids = normalizeSharedTypeIds(typeIds, m_settings->b(keys::library::ShareUnlistedTypes));
    m_settings->setValue(keys::library::SharedTypes, ids);   // → rescan via Settings::changed
}

QString Library::otherExtensions() const
{
    return m_settings->s(keys::library::OtherExtensions);
}

void Library::setOtherExtensions(const QString &extensions)
{
    m_settings->setValue(keys::library::OtherExtensions, extensions.simplified());
}

bool Library::typeAllowed(const QString &fileName) const
{
    return fileMatchesSharedTypes(fileName, sharedTypes(), otherExtensions(),
                                  m_settings->b(keys::library::ShareUnlistedTypes));
}

void Library::excludeFolder(const QString &path, bool excluded)
{
    if (excluded) {
        unshareFile(path);
    } else {
        reshareFile(path);
    }
    emit foldersChanged();
}

QStringList Library::excludedFolders() const
{
    QStringList out;
    for (const QString &p : m_excluded)
        if (!QFileInfo(p).isFile()) out << p;   // directories (also ones that are gone right now)
    out.sort();
    return out;
}

QStringList Library::standardMediaFolders()
{
    QStringList out;
    const QStandardPaths::StandardLocation locs[] = {QStandardPaths::MusicLocation, QStandardPaths::MoviesLocation,
                                                     QStandardPaths::PicturesLocation, QStandardPaths::DownloadLocation};
    for (QStandardPaths::StandardLocation l : locs)
        for (const QString &p : QStandardPaths::standardLocations(l)) {
            const QString n = normPath(p);
            if (!n.isEmpty() && !out.contains(n)) out << n;
        }
    return out;
}

QStringList Library::suggestMediaFolders() const
{
    if (!m_suggestHome) return {};   // [core-5] an isolated (test / --profile) run never offers $HOME folders
    const QString home = normPath(QDir::homePath());
    QStringList out;
    for (const QString &p : standardMediaFolders()) {
        if (p == home || !QFileInfo(p).isDir()) continue;   // a missing XDG dir falls back to $HOME
        bool shared = false;
        for (const SharedFolder &f : effectiveFolders())
            if (f.path == p || (f.recursive && isUnder(p, f.path))) shared = true;
        if (!shared) out << p;
    }
    return out;
}

// ---------------------------------------------------------------- scanning
void Library::startScan()
{
    if (!m_started || m_stopped) return;
    if (m_scanning) {
        m_rescanPending = true;
        return;
    }
    m_scanning = true;
    m_rescanPending = false;
    m_current.clear();
    emitProgress(QString());
    const QList<SharedFolder> folders = effectiveFolders();
    const QSet<QString> excluded = m_excluded;
    const QList<QRegularExpression> patterns = compilePatterns(m_settings->sl(keys::library::ExcludePatterns));
    const bool hidden = m_settings->b(keys::library::ShareHidden);
    const QStringList typeIds = sharedTypes();
    const QString otherExt = otherExtensions();
    const bool unlisted = m_settings->b(keys::library::ShareUnlistedTypes);
    const QString dbPath = m_db->path();
    std::atomic<bool> *cancel = &m_cancel;
    m_db->flush(); // make our deferred hash results visible to the scanner's connection

    m_scanWatcher.setFuture(QtConcurrent::run(&m_scanPool, [=]() -> ScanResult * {
        auto *res = new ScanResult;
        const QString conn = QStringLiteral("mxlib-scan-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        QHash<QString, LibraryRecord> cache;
        if (!dbPath.isEmpty()) {
            QSqlDatabase db = Database::openThreadConnection(dbPath, conn);
            if (db.isValid()) cache = LibraryDao::loadAll(conn);
            db = QSqlDatabase();
            Database::closeThreadConnection(conn);
        }
        QSet<QString> seen;
        auto isExcluded = [&](const QString &p) {
            for (const QString &ex : excluded)
                if (isUnder(p, ex)) return true;
            return false;
        };
        for (const SharedFolder &folder : folders) {
            if (cancel->load()) break;
            if (!QFileInfo(folder.path).isDir()) continue;
            QStringList stack{folder.path};
            while (!stack.isEmpty() && !cancel->load()) {
                const QString dir = stack.takeLast();
                if (res->dirs.contains(dir)) continue;
                res->dirs << dir;
                QDir::Filters filters = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System;
                if (hidden) filters |= QDir::Hidden;
                QDirIterator it(dir, filters);
                while (it.hasNext()) {
                    it.next();
                    const QFileInfo fi = it.fileInfo();
                    const QString path = QDir::cleanPath(fi.absoluteFilePath());
                    if (isExcluded(path)) continue;
                    // dot-files count as hidden on every platform (not only where the OS says so)
                    if (!hidden && (fi.isHidden() || fi.fileName().startsWith(QLatin1Char('.')))) continue;
                    if (fi.isDir()) {
                        if (folder.recursive && !fi.isSymLink()) stack << path;
                        continue;
                    }
                    if (!fi.isFile() || !fi.isReadable()) continue;
                    if (!fileMatchesSharedTypes(fi.fileName(), typeIds, otherExt, unlisted)) continue;   // [glue-3]
                    bool skip = false;
                    for (const QRegularExpression &re : patterns) {
                        if (re.match(fi.fileName()).hasMatch()) { skip = true; break; }
                    }
                    if (skip || seen.contains(path)) continue;
                    seen.insert(path);
                    ScanItem item;
                    item.path = path;
                    item.size = fi.size();
                    item.mtimeMs = fi.lastModified().toMSecsSinceEpoch();
                    const auto c = cache.constFind(path);
                    if (c != cache.constEnd() && c->size == item.size && c->mtimeMs == item.mtimeMs
                        && !c->hashes.ed2k.isEmpty()) {
                        item.cached = true;
                        item.rec = c.value();
                    }
                    res->items << item;
                }
            }
        }
        if (!cancel->load()) {
            for (auto it = cache.constBegin(); it != cache.constEnd(); ++it) {
                if (seen.contains(it.key())) continue;
                // keep cache rows of files that still exist (hashed on request, outside the shares)
                if (!QFileInfo::exists(it.key())) res->stale << it.key();
            }
        }
        return res;
    }));
}

void Library::onScanDone()
{
    ScanResult *r = m_scanWatcher.result();
    m_scanning = false;
    if (!r) return;
    if (m_stopped) {
        delete r;
        return;
    }
    QSet<QString> found;
    QStringList toQueue;
    int cachedCount = 0;
    {
        QWriteLocker l(&m_lock);
        for (const ScanItem &item : std::as_const(r->items)) {
            found.insert(item.path);
            auto it = m_entries.find(item.path);
            if (it != m_entries.end() && it->file.size == item.size
                && it->file.mtime.toMSecsSinceEpoch() == item.mtimeMs) {
                it->shared = true;
                if (!it->file.hashed && !m_queued.contains(item.path)) toQueue << item.path;
                continue;
            }
            Entry e;
            e.file.path = item.path;
            e.file.name = QFileInfo(item.path).fileName();
            e.file.size = item.size;
            e.file.mtime = QDateTime::fromMSecsSinceEpoch(item.mtimeMs);
            e.file.type = mediaTypeForFileName(e.file.name);
            if (item.cached) {
                e.file.hashes = item.rec.hashes;
                e.file.hashed = true;
                e.file.durationSec = item.rec.durationSec;
                e.file.bitrateKbps = item.rec.bitrateKbps;
                e.file.sampleRateHz = item.rec.sampleRateHz;
                dropNonAudioMedia(e.file.type, &e.file.durationSec, &e.file.bitrateKbps, &e.file.sampleRateHz);
                e.file.uploadedBytes = item.rec.uploaded;
                e.file.requests = item.rec.requests;
                e.ed2kParts = item.rec.ed2kParts;
                e.aichParts = item.rec.aichParts;
                ++cachedCount;
            }
            if (it != m_entries.end()) unindexLocked(*it);
            m_entries.insert(item.path, e);
            if (e.file.hashed) indexEntryLocked(e);
            else toQueue << item.path;
        }
        for (auto it = m_entries.begin(); it != m_entries.end();) {
            if (it->shared && !found.contains(it.key())) {
                unindexLocked(*it);
                it = m_entries.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const QString &p : std::as_const(toQueue)) {
        if (m_queued.contains(p)) continue;
        m_queued.insert(p);
        m_hashQueue << p;
        ++m_hashTotal;
    }
    m_dao->removeFiles(r->stale);
    updateWatcher(r->dirs);
    qCInfo(lcLibrary) << "scan done:" << r->items.size() << "files," << cachedCount << "from cache,"
                      << toQueue.size() << "to hash," << r->stale.size() << "stale cache rows";
    delete r;
    scheduleUpdate();
    if (m_rescanPending) {
        startScan();
        return;
    }
    pumpHashQueue();
    if (m_inFlight == 0 && m_hashQueue.isEmpty()) {
        m_hashTotal = m_hashDone = 0;
        emitProgress(QString());
        emit scanFinished();
    }
}

void Library::updateWatcher(const QStringList &dirs)
{
    if (!m_settings->b(keys::library::WatchFolders)) {
        delete m_watcher;
        m_watcher = nullptr;
        return;
    }
    if (!m_watcher) {
        m_watcher = new QFileSystemWatcher(this);
        connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_watchDebounce.start(); });
    }
    const QStringList want = dirs.mid(0, 2000);
    const QSet<QString> wantSet(want.begin(), want.end());
    const QStringList have = m_watcher->directories();
    QStringList rm;
    for (const QString &d : have)
        if (!wantSet.contains(d)) rm << d;
    if (!rm.isEmpty()) m_watcher->removePaths(rm);
    const QSet<QString> haveSet(have.begin(), have.end());
    QStringList add;
    for (const QString &d : want)
        if (!haveSet.contains(d)) add << d;
    if (!add.isEmpty()) m_watcher->addPaths(add);
}

// ---------------------------------------------------------------- hashing
void Library::requestHash(const QString &path)
{
    const QString p = normPath(path);
    if (p.isEmpty() || m_stopped) return;
    const QFileInfo fi(p);
    {
        QReadLocker l(&m_lock);
        const auto it = m_entries.constFind(p);
        if (it != m_entries.constEnd() && it->file.hashed && it->file.size == fi.size()
            && it->file.mtime.toMSecsSinceEpoch() == fi.lastModified().toMSecsSinceEpoch()) {
            const SharedFile f = it->file;
            QMetaObject::invokeMethod(this, [this, f] { emit fileHashed(f); }, Qt::QueuedConnection);
            return;
        }
    }
    // the DB hash cache (path + size + mtime): e.g. hashes the engine stored with addHashed()
    if (deliverFromCache(p, true)) {
        m_hashQueue.removeAll(p);
        if (m_queued.remove(p)) --m_hashTotal;
        return;
    }
    m_priority.insert(p);
    m_hashQueue.removeAll(p);
    m_hashQueue.prepend(p);
    if (!m_queued.contains(p)) {
        m_queued.insert(p);
        ++m_hashTotal;
    }
    pumpHashQueue();
}

bool Library::deliverFromCache(const QString &p, bool priority)
{
    const QFileInfo fi(p);
    LibraryRecord rec;
    if (!fi.exists() || !m_dao->lookup(p, fi.size(), fi.lastModified().toMSecsSinceEpoch(), &rec) || rec.hashes.ed2k.isEmpty())
        return false;
    HashResult r;
    r.path = p;
    r.ok = true;
    r.size = rec.size;
    r.mtimeMs = rec.mtimeMs;
    r.hashes = rec.hashes;
    r.ed2kParts = rec.ed2kParts;
    r.aichParts = rec.aichParts;
    r.media.durationSec = rec.durationSec;
    r.media.bitrateKbps = rec.bitrateKbps;
    r.media.sampleRateHz = rec.sampleRateHz;
    dropNonAudioMedia(mediaTypeForFileName(p), &r.media.durationSec, &r.media.bitrateKbps, &r.media.sampleRateHz);
    if (priority) m_priority.insert(p);
    ++m_inFlight;
    ++m_hashTotal;
    QMetaObject::invokeMethod(this, [this, r] { onHashed(r); }, Qt::QueuedConnection);
    return true;
}

void Library::addHashed(const QString &path, const FileHashes &hashes, const QList<QByteArray> &ed2kParts,
                        const QList<QByteArray> &aichParts)
{
    const QString p = normPath(path);
    const QFileInfo fi(p);
    if (p.isEmpty() || !fi.isFile() || hashes.ed2k.isEmpty() || m_stopped) {
        requestHash(path);
        return;
    }
    LibraryRecord rec;
    rec.path = p;
    rec.size = fi.size();
    rec.mtimeMs = fi.lastModified().toMSecsSinceEpoch();
    rec.hashes = hashes;
    rec.hashes.btih.clear();
    rec.hashes.btFileIndex = -1;
    rec.ed2kParts = ed2kParts;
    rec.aichParts = aichParts;
    rec.hashedAt = QDateTime::currentDateTime();
    m_dao->store(rec);
    m_db->flush();   // visible to requestHash() below and to the scanner's own connection
    requestHash(p);
}

void Library::pumpHashQueue()
{
    if (m_stopped) return;
    const int maxInFlight = qMax(1, m_hashPool.maxThreadCount()) * 2;
    while (m_inFlight < maxInFlight && !m_hashQueue.isEmpty()) {
        const QString path = m_hashQueue.takeFirst();
        m_queued.remove(path);
        if (deliverFromCache(path, m_priority.contains(path))) {   // stored meanwhile (addHashed)
            --m_hashTotal;   // deliverFromCache counted it again
            continue;
        }
        ++m_inFlight;
        m_current = QFileInfo(path).fileName();
        emitProgress(m_current);
        std::atomic<bool> *cancel = &m_cancel;
        m_hashPool.start([this, path, cancel] {
            HashResult r;
            r.path = path;
            const QFileInfo before(path);
            r.size = before.size();
            r.mtimeMs = before.lastModified().toMSecsSinceEpoch();
            hash::MultiHasher h(hash::MultiHasher::Default);
            QString err;
            ++m_hashRuns;
            r.ok = hash::MultiHasher::hashFile(path, h, cancel, {}, &err);
            if (r.ok) {
                const QFileInfo after(path);
                if (after.size() != r.size || after.lastModified().toMSecsSinceEpoch() != r.mtimeMs) {
                    r.ok = false;
                    err = QStringLiteral("file changed while hashing");
                }
            }
            if (r.ok) {
                r.hashes = h.hashes();
                r.ed2kParts = h.ed2kPartHashes();
                r.aichParts = h.aichPartHashes();
                if (mediaTypeForFileName(path) == MediaType::Audio) r.media = readMediaInfo(path);
            }
            r.error = err;
            QMetaObject::invokeMethod(this, [this, r] { onHashed(r); }, Qt::QueuedConnection);
        });
    }
}

void Library::onHashed(const HashResult &r)
{
    --m_inFlight;
    ++m_hashDone;
    const bool priority = m_priority.remove(r.path);
    if (r.ok) {   // also while stopping: finished work is still worth caching
        SharedFile file;
        bool keep = false;
        {
            QWriteLocker l(&m_lock);
            auto it = m_entries.find(r.path);
            if (it == m_entries.end() && priority) {
                Entry e;
                e.file.path = r.path;
                e.file.name = QFileInfo(r.path).fileName();
                e.file.type = mediaTypeForFileName(e.file.name);
                e.shared = isUnderSharedFolder(r.path);
                it = m_entries.insert(r.path, e);
            } else if (it != m_entries.end() && priority && !it->shared) {
                it->shared = isUnderSharedFolder(r.path);
            }
            if (it != m_entries.end()) {
                unindexLocked(*it);
                it->file.size = r.size;
                it->file.mtime = QDateTime::fromMSecsSinceEpoch(r.mtimeMs);
                it->file.hashes = r.hashes;
                it->file.hashed = true;
                it->file.durationSec = r.media.durationSec;
                it->file.bitrateKbps = r.media.bitrateKbps;
                it->file.sampleRateHz = r.media.sampleRateHz;
                it->ed2kParts = r.ed2kParts;
                it->aichParts = r.aichParts;
                indexEntryLocked(*it);
                file = it->file;
                keep = true;
            }
        }
        if (keep || priority) {
            LibraryRecord rec;
            rec.path = r.path;
            rec.size = r.size;
            rec.mtimeMs = r.mtimeMs;
            rec.hashes = r.hashes;
            rec.ed2kParts = r.ed2kParts;
            rec.aichParts = r.aichParts;
            rec.durationSec = r.media.durationSec;
            rec.bitrateKbps = r.media.bitrateKbps;
            rec.sampleRateHz = r.media.sampleRateHz;
            rec.hashedAt = QDateTime::currentDateTime();
            m_dao->store(rec);
            emit fileHashed(file);
        }
    } else if (!r.ok && !m_cancel) {
        qCInfo(lcLibrary) << "could not hash" << r.path << r.error;
        if (priority) {
            SharedFile f;
            f.path = r.path;
            f.name = QFileInfo(r.path).fileName();
            emit fileHashed(f); // hashed == false tells the requester it failed
        }
    }
    scheduleUpdate();
    if (m_inFlight == 0 && m_hashQueue.isEmpty()) {
        const int total = m_hashTotal;
        m_hashTotal = m_hashDone = 0;
        m_current.clear();
        if (!m_scanning) {
            emit scanProgress(total, total, QString());
            emit scanFinished();
        }
    } else {
        emitProgress(m_current);
    }
    pumpHashQueue();
}

void Library::emitProgress(const QString &current)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!current.isEmpty() && now - m_lastProgressMs < 200) return;
    m_lastProgressMs = now;
    emit scanProgress(m_hashDone, m_hashTotal, current);
}

bool Library::isScanning() const
{
    return m_scanning || m_inFlight > 0 || !m_hashQueue.isEmpty();
}

QString Library::scanStatus() const
{
    if (m_inFlight > 0 || !m_hashQueue.isEmpty())
        return QStringLiteral("Hashing %1 of %2: %3").arg(qMin(m_hashDone + 1, m_hashTotal)).arg(m_hashTotal).arg(m_current);
    if (m_scanning) return QStringLiteral("Scanning shared folders...");
    return QStringLiteral("Sharing %1 (%2)").arg(countNoun(fileCount(), QStringLiteral("file"))).arg(humanSize(totalBytes()));
}

void Library::scheduleUpdate()
{
    if (!m_updateTimer.isActive()) m_updateTimer.start();
    if (!m_pushTimer.isActive()) m_pushTimer.start(); // at most one push per 2 s
}

// ---------------------------------------------------------------- index
void Library::indexEntryLocked(const Entry &e)
{
    const FileHashes &h = e.file.hashes;
    addIndex(m_index, 'e', h.ed2k, e.file.path);
    addIndex(m_index, 'a', h.aich, e.file.path);
    addIndex(m_index, 's', h.sha1, e.file.path);
    addIndex(m_index, 't', h.tth, e.file.path);
    addIndex(m_index, '2', h.sha256, e.file.path);
    addIndex(m_index, 'm', h.md5, e.file.path);
}

void Library::unindexLocked(const Entry &e)
{
    const FileHashes &h = e.file.hashes;
    const std::pair<char, const QByteArray *> kinds[] = {{'e', &h.ed2k}, {'a', &h.aich}, {'s', &h.sha1},
                                                         {'t', &h.tth}, {'2', &h.sha256}, {'m', &h.md5}};
    for (const auto &k : kinds) {
        if (k.second->isEmpty()) continue;
        const QByteArray key = k.first + *k.second;
        const auto it = m_index.constFind(key);
        if (it != m_index.constEnd() && it.value() == e.file.path) m_index.remove(key);
    }
}

QString Library::findPathLocked(const FileHashes &h) const
{
    const std::pair<char, const QByteArray *> kinds[] = {{'e', &h.ed2k}, {'s', &h.sha1}, {'t', &h.tth},
                                                         {'2', &h.sha256}, {'a', &h.aich}, {'m', &h.md5}};
    for (const auto &k : kinds) {
        if (k.second->isEmpty()) continue;
        const auto it = m_index.constFind(k.first + *k.second);
        if (it == m_index.constEnd()) continue;
        const auto e = m_entries.constFind(it.value());
        if (e != m_entries.constEnd() && e->shared && e->file.hashes.matches(h)) return it.value();
    }
    return QString();
}

// ---------------------------------------------------------------- LibraryService lists
QList<SharedFile> Library::files(const QString &folder) const
{
    const QString f = folder.isEmpty() ? QString() : normPath(folder);
    QList<SharedFile> out;
    QReadLocker l(&m_lock);
    out.reserve(m_entries.size());
    for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it) {
        if (!it->shared) continue;
        if (!f.isEmpty() && !isUnder(it.key(), f)) continue;
        out << it->file;
    }
    std::sort(out.begin(), out.end(), [](const SharedFile &a, const SharedFile &b) { return a.path < b.path; });
    return out;
}

int Library::fileCount() const
{
    QReadLocker l(&m_lock);
    int n = 0;
    for (const Entry &e : m_entries)
        if (e.shared) ++n;
    return n;
}

qint64 Library::totalBytes() const
{
    QReadLocker l(&m_lock);
    qint64 n = 0;
    for (const Entry &e : m_entries)
        if (e.shared) n += e.file.size;
    return n;
}

// ---------------------------------------------------------------- LibraryAccess
QList<SharedFile> Library::sharedFiles() const
{
    QList<SharedFile> out;
    QReadLocker l(&m_lock);
    for (const Entry &e : m_entries)
        if (e.shared && e.file.hashed) out << e.file;
    return out;
}

bool Library::findByHash(const FileHashes &h, SharedFile *out) const
{
    PartialFinder finder;
    {
        QReadLocker l(&m_lock);
        const QString p = findPathLocked(h);
        if (!p.isEmpty()) {
            if (out) *out = m_entries.value(p).file;
            return true;
        }
        finder = m_partialFinder;
    }
    return finder ? finder(h, out) : false;
}

QByteArray Library::read(const FileHashes &h, qint64 offset, qint64 length)
{
    QString path;
    PartialReader reader;
    {
        QReadLocker l(&m_lock);
        path = findPathLocked(h);
        reader = m_partialReader;
    }
    if (path.isEmpty()) return reader ? reader(h, offset, length) : QByteArray();
    if (offset < 0 || length <= 0) return {};
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    if (offset >= f.size()) return {};
    if (!f.seek(offset)) return {};
    return f.read(qMin<qint64>(length, 64 * 1024 * 1024));
}

QList<QByteArray> Library::ed2kPartHashes(const FileHashes &h) const
{
    QReadLocker l(&m_lock);
    const QString p = findPathLocked(h);
    return p.isEmpty() ? QList<QByteArray>() : m_entries.value(p).ed2kParts;
}

QList<QByteArray> Library::aichPartHashes(const FileHashes &h) const
{
    QReadLocker l(&m_lock);
    const QString p = findPathLocked(h);
    return p.isEmpty() ? QList<QByteArray>() : m_entries.value(p).aichParts;
}

void Library::recordUpload(const FileHashes &h, qint64 bytes)
{
    QString path;
    {
        QWriteLocker l(&m_lock);
        path = findPathLocked(h);
        if (path.isEmpty()) return;
        Entry &e = m_entries[path];
        e.file.uploadedBytes += bytes;
        e.file.requests += 1;
    }
    m_dao->addUpload(path, bytes);
    scheduleUpdate();
}

void Library::setPartialProvider(PartialFinder finder, PartialReader reader)
{
    QWriteLocker l(&m_lock);
    m_partialFinder = std::move(finder);
    m_partialReader = std::move(reader);
}

} // namespace mx

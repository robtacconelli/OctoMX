// SPDX-License-Identifier: GPL-3.0-or-later
// [safe-1] Files that survive a stop at any moment: sync to the disk, a copy that is never seen
// half done, and the test seams of the closing / crash / power cut harness (tests/engine,
// tst_engine_crash).
//
// The rule they serve (docs/status/safe-1.md): the stored record of a download never claims a
// byte that is not on the disk. Data is written, then synced (syncFile), then recorded.
#pragma once

#include <QtCore/QString>

#include <atomic>

class QFileDevice;

namespace mx::durable {

// fdatasync (Linux), fsync (macOS, other Unix), FlushFileBuffers (Windows) of an open file.
// A disk that acknowledges a flush it did not do cannot be helped here.
bool syncFile(QFileDevice &f);
// The same for a file somebody else writes (libtorrent's files): opened, synced, closed. A file
// that does not exist counts as synced.
bool syncPath(const QString &path);
// The folder entry after a rename or a new file (POSIX); nothing to do on Windows.
bool syncDir(const QString &dir);
// Copy `from` to `to` (which must not exist) through a temporary name beside `to`
// (copyTempName(to)): write, sync, rename, sync the folder. A stop at any moment leaves either no
// `to` or a complete one, at worst the temporary file. `from` is not removed.
bool copyFile(const QString &from, const QString &to, QString *error, const std::atomic<bool> *cancel = nullptr);
QString copyTempName(const QString &to);

// ---- test seams; inactive (and free) unless the environment variable is set when the program starts
// MX_TEST_SLOW_WRITE_MS=<n>: every part-file write takes n ms longer (a slow or busy disk)
int testWriteDelayMs();
// MX_TEST_POWERCUT=1: what is written through journalWrite() / announced with noteWritten() is
// remembered per file until noteSynced(); simulatePowerCut() then overwrites every range that was
// not synced with zeros (what a power cut loses), and the test kills the process.
bool powerCutTest();
// MX_TEST_POWERCUT_LIE=1 (with MX_TEST_POWERCUT): a disk that acknowledges a sync it did not do: the
// journal forgets nothing at noteSynced(), so the cut also zeroes what the record claims as synced
bool powerCutLies();
// seek + write + journal, under the journal's lock while the power cut test runs (so a cut never
// lands between a write and its note). Returns the bytes written (-1 on error).
qint64 journalWrite(QFileDevice &f, const QString &path, qint64 off, const QByteArray &data);
void noteWritten(const QString &path, qint64 off, qint64 len);
void noteSynced(const QString &path);
void noteRenamed(const QString &from, const QString &to);
// Zero every unsynced range; returns the number of bytes zeroed. The journal's lock stays taken
// (no more writes can land) until the process ends: call it right before killing the process.
qint64 simulatePowerCut(QString *report = nullptr);
// The journal outlives the process (a power cut shortly after a quit loses what the quit did not
// sync): saveJournal() when the program ends, loadJournal() when it starts.
void saveJournal(const QString &file);
void loadJournal(const QString &file);

} // namespace mx::durable

// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX core: merging of search hits from several networks (rules: docs/ARCHITECTURE.md §Search).
//  1. same file when a.hashes.matches(b.hashes) (some kind equal, none conflicting). Two hits of the
//     same whole torrent (same btih, no file index) are one row also when the indexes disagree about
//     it being a bundle (the known file count / list decides, see 4.).
//  2. [merge-1] no shared hash kind and sameFile(): exactly the same byte size (>= minCandidateSize,
//     default 1 MiB), the same non-empty extension, and sameName() (see below) → merged as a
//     *candidate*: the new sources get extra["candidate"]=true and extra["hashes"]=<their
//     FileHashes::toVariant()>. This holds for EVERY pair of networks (eD2k, Kad, Gnutella, the
//     overlay, HTTP, single-file torrents), in both directions. A torrent counts as one file when
//     its file list has one real entry (pad files ignored), when an index reported one file, or when
//     nothing says it has several and its name has an extension (also a trailing extension word:
//     "Sintel 2010 720p mkv" is "sintel 2010 720p.mkv", torrent indexes often lose the dots).
//     A later hash that matches a candidate's hash (another index listing the same torrent with a
//     rounded size) joins the same row as a candidate too.
//  3. [merge-1] inner files: when a bundle's file list is known, every inner file that is sameFile()
//     as a plain-file row makes the torrent a source of that row (derived source, extra["innerOf"] =
//     bundle key, ["insideBundle"] = torrent name, ["bundlePath"], ["btFileIndex"], candidate=true);
//     the bundle row stays and lists those files in SearchHit::innerMatches.
//  4. never when hashes conflict; a torrent whose file list / count later shows several files is
//     split out of the row again (setBundleFiles(), or a hit that brings the list), its inner files
//     then follow rule 3.
// sameName(): lower-case words of letters and digits (two characters or more, the extension word
// not counted); at least 60 % of the shorter name's words occur in the other name; names without
// such words must be equal after normalisation. The same rule decides the engine's background
// "Search by name and size" (Download::nameSearchHits): one rule, one place.
// The merged hit keeps the best metadata (the longest name, any known duration/bitrate/freq/path).
// SearchHit::key is fixed when the hit is first created: ed2k:<hex> / sha1:<hex> / btih:<hex>[:idx]
// / size+name:<size>:<normalised name>, made unique with "#n" if needed (a row rebuilt after a split
// keeps its key).
// When a new hit links two (or more) existing rows (shares a hash kind with each, no conflicts),
// those rows are merged into the oldest one; the absorbed keys are reported in Result::removedKeys.
// acrossNetworks = false restores the strict rule (exact normalised names, plain files only, a bundle
// never meets a non-bundle): setting search/mergeByNameAndSize.
#pragma once

#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QMultiHash>
#include <QtCore/QSet>

#include "mx/types.h"

namespace mx {

class SearchMerger {
public:
    explicit SearchMerger(qint64 minCandidateSize = 1024 * 1024, bool acrossNetworks = true)
        : m_minCandidate(minCandidateSize), m_across(acrossNetworks) {}

    struct Result {
        int index = -1;       // index into hits(), -1 if dropped (allowNew == false)
        bool added = false;   // true: a new hit; false: merged into an existing one
        bool candidate = false;
        QStringList removedKeys;   // rows absorbed into hits()[index] or gone after a split (indices of later rows shift)
        QStringList changedKeys;   // [merge-1] other rows that changed (inner-file links, rebuilt rows)
    };
    Result add(const SearchHit &hit, bool allowNew = true);
    const QList<SearchHit> &hits() const { return m_hits; }
    SearchHit &hitAt(int i) { return m_hits[i]; }
    int size() const { return m_hits.size(); }
    int indexOfKey(const QString &key) const { return m_keys.value(key, -1); }
    bool remove(const QString &key);   // [ui-f] drop a row (a late file list contradicts the search type)

    // [merge-1] the file list of a torrent learned later (index, Rats, metadata): every row with that
    // torrent is rebuilt (a single-file match that turns out to be a folder is split again, inner
    // files are linked). Keys of rows that no longer exist / rows that changed.
    struct Update {
        QStringList removedKeys;
        QStringList changedKeys;
    };
    Update setBundleFiles(const QByteArray &btih, const QList<BundleFile> &files);
    // [merge-1] bundle rows without a file list that may hold the file of row `idx` (or, for a bundle
    // row, itself when a plain row neighbours it by name and size): worth one file-list request
    QStringList bundlesWorthFileList(int idx) const;

    static QString normalizedName(const QString &fileName);   // "artist - title.mp3" → "artist title.mp3"
    static QString baseKey(const SearchHit &hit);              // key before uniquification
    static bool sharesHashKind(const FileHashes &a, const FileHashes &b);
    // [merge-1] THE name-and-size rule (search list and the engine's background search)
    static QString probableExtension(const QString &name);    // "a.MKV" / "a 720p mkv" → "mkv"; "" = none
    static QStringList nameWords(const QString &name);        // words without the extension word
    static bool sameName(const QString &a, const QString &b);
    static bool sameFile(const QString &nameA, qint64 sizeA, const QString &nameB, qint64 sizeB,
                         qint64 minSize = 1024 * 1024);
    static bool isPadFile(const QString &path);
    static QList<BundleFile> realFiles(const QList<BundleFile> &files);   // without pad files

private:
    struct Ident {            // what a hit is as one file, for rule 2
        bool valid = false;
        QString name;
        qint64 size = 0;
        bool probableSingle = false;   // a bundle-flagged torrent that is probably one file
    };
    Ident identOf(const SearchHit &h) const;
    SearchHit normalise(const SearchHit &h) const;
    bool learnTorrentFacts(const SearchHit &h);   // file count / list of a torrent; true = changed
    bool nameMatches(int idx, const Ident &id) const;
    int addInternal(const SearchHit &h, bool allowNew, const QString &preferKey, Result *res);
    void mergeInto(int idx, const SearchHit &h, bool candidate);
    static bool hasDerived(const SearchHit &h);
    void indexRow(int idx);
    void rebuildIndexes();
    QStringList rebuildRowsWith(const QByteArray &btih, QStringList *changed);
    void relinkAll(QStringList *changed);
    QStringList hashKeys(const FileHashes &h) const;
    QString uniqueKey(const QString &base) const;

    qint64 m_minCandidate;
    bool m_across = true;
    QList<SearchHit> m_hits;
    struct Part {
        SearchHit hit;
        bool candidate = false;
    };
    QList<QList<Part>> m_parts;             // [merge-1] the (normalised) hits each row was made of
    QMultiHash<QString, int> m_hashIndex;   // "e:<raw>" ... → hit index (trusted row hashes)
    QMultiHash<QString, int> m_candIndex;   // [merge-1] hashes of candidate sources → hit index
    QMultiHash<qint64, int> m_bySize;       // [merge-1] size of a one-file row → hit index
    QHash<QString, int> m_keys;             // used keys
    QHash<QByteArray, QList<BundleFile>> m_btFiles;   // [merge-1] known file lists per torrent
    QHash<QByteArray, int> m_btCount;                 // known file counts (Rats "files", index lists)
};

} // namespace mx

// SPDX-License-Identifier: GPL-3.0-or-later
// A small, bounded Kad storing index: what other nodes publish to us when we are one of the ~10
// nodes closest to a keyword or file hash. Same semantics as eMule's CIndexed in miniature:
// keyword entries (24 h lifetime, publish IP tracking → trust value / TAG_PUBLISHINFO, AICH votes)
// and source entries (5 h), load percentage in PUBLISH_RES, restrictive keyword searches evaluated
// against the stored tags. Everything is capped; when full we still answer "success" with load 100
// (eMule's behaviour under full load, so publishers spread to other nodes).
#pragma once

#include "kad_wire.h"

#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QStringList>

namespace mx::ed2k::kad {

struct KadIndexLimits {
    int maxKeywordEntries = 20000;   // whole index
    int maxPerKeyword = 2000;
    int maxSourceKeys = 5000;
    int maxSourcesPerFile = 200;
};

class KadIndex {
public:
    explicit KadIndex(const KadIndexLimits &lim = {}) : m_lim(lim) {}

    // returns false if the entry is invalid; *load set either way
    bool addKeyword(const KadId &keyword, const KadId &file, quint32 publisherIp, const TagList &tags,
                    qint64 now, quint8 *load);
    bool addSource(const KadId &file, const KadId &source, quint32 ip, quint16 udpPort, const TagList &tags,
                   qint64 now, quint8 *load);
    // results for SEARCH_KEY_REQ (tree: restrictive search terms, may be empty) / SEARCH_SOURCE_REQ
    QList<SearchResEntry> keywordResults(const KadId &keyword, const QByteArray &tree, int start, int max, qint64 now);
    QList<SearchResEntry> sourceResults(const KadId &file, quint64 size, int start, int max, qint64 now);
    void clean(qint64 now);

    int keywordEntries() const { return m_keywordTotal; }
    int keywordKeys() const { return int(m_keywords.size()); }
    int sourceEntries() const { return m_sourceTotal; }
    int sourceKeys() const { return int(m_sources.size()); }

    // parses a search tree (eMule format) and evaluates it against a tag list; exposed for tests
    static bool treeMatches(const QByteArray &tree, const QString &fileName, const TagList &tags, bool *valid = nullptr);

private:
    struct KeyEntry {
        KadId file;
        TagList tags;                   // without name/size/AICH (kept separately)
        QString name;
        quint64 size = 0;
        QHash<quint32, qint64> publishers;   // ip -> last publish (s)
        QHash<QString, int> names;           // filename -> publish count
        QByteArray aich;
        int aichVotes = 0;
        qint64 expires = 0;
    };
    struct SourceEntry {
        KadId source;
        quint32 ip = 0;
        quint16 tcpPort = 0, udpPort = 0;
        quint64 size = 0;
        TagList tags;
        qint64 expires = 0;
    };
    double trust(const KeyEntry &e) const;

    KadIndexLimits m_lim;
    QHash<KadId, QHash<KadId, KeyEntry>> m_keywords;
    QHash<KadId, QList<SourceEntry>> m_sources;
    QHash<quint32, int> m_publishSubnets;     // /24 -> keyword entries published from it
    int m_keywordTotal = 0, m_sourceTotal = 0;
    qint64 m_lastClean = 0;
};

} // namespace mx::ed2k::kad

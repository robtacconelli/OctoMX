// SPDX-License-Identifier: GPL-3.0-or-later
// Small helpers shared by the overlay module (private).
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QDateTime>
#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QLoggingCategory>
#include <QtCore/QSet>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <deque>

#include "mx/types.h"

Q_DECLARE_LOGGING_CATEGORY(lcOverlay)
Q_DECLARE_LOGGING_CATEGORY(lcRats)

namespace mx::ovl {

inline qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

// ---- hostile-JSON accessors: never throw, bound sizes, wrong type → default
QString jStr(const QJsonObject &o, const char *key, int maxLen = 1024);
qint64  jInt(const QJsonObject &o, const char *key, qint64 def = 0);
bool    jBool(const QJsonObject &o, const char *key, bool def = false);
QJsonObject jObj(const QJsonObject &o, const char *key);
QJsonArray  jArr(const QJsonObject &o, const char *key, int maxItems = 10000);
bool isHex(const QString &s, int len = -1);   // lower/upper hex, optional exact length

// ---- hashes <-> JSON (hex strings; unknown/invalid entries dropped)
QJsonObject hashesToJson(const FileHashes &h);
FileHashes  hashesFromJson(const QJsonObject &o);

// ---- encodings of control messages ('J' = JSON, 'Z' = zlib JSON); bounded decode
QByteArray encodeControl(const QJsonObject &o, bool allowCompress = true);
bool decodeControl(const QByteArray &bytes, QJsonObject *out, int maxInflated = 4 * 1024 * 1024);

// ---- names
QString normaliseRoomName(const QString &name);   // lower case, trimmed, inner whitespace collapsed
QString cleanText(const QString &s, int maxLen);  // strip control characters, bound length
QString cleanName(const QString &s);              // screen name (no spaces/control, ≤ 32)
QString idSuffix(const QString &userIdHex);       // 5 decimal digits derived from the id
QString randomId();                               // 16 hex chars

// ---- keyword matching (all words of `query` occur in `text`, case-insensitive)
QStringList queryWords(const QString &q);
bool matchesWords(const QString &text, const QStringList &words);
bool looksAdult(const QString &name);

// ---- per-key token bucket (rate limit per sender / per peer)
class RateGate {
public:
    RateGate(double perSecond = 1.0, double burst = 5.0) : m_rate(perSecond), m_burst(burst) {}
    void configure(double perSecond, double burst) { m_rate = perSecond; m_burst = burst; }
    bool allow(const QString &key, double cost = 1.0);
    void prune();
private:
    struct B { double tokens; qint64 last; };
    double m_rate, m_burst;
    QHash<QString, B> m_b;
};

// ---- bounded "seen" set for dedupe (FIFO eviction)
class SeenSet {
public:
    explicit SeenSet(int cap = 4096) : m_cap(cap) {}
    bool insert(const QString &id);   // false if already present
    bool contains(const QString &id) const { return m_set.contains(id); }
private:
    int m_cap;
    QSet<QString> m_set;
    std::deque<QString> m_order;
};

QString humanSize(qint64 bytes);

} // namespace mx::ovl

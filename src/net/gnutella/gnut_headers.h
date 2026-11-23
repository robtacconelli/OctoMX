// SPDX-License-Identifier: GPL-3.0-or-later
// Gnutella module: header blocks shared by the 0.6 handshake and HTTP: a bounded incremental
// reader ("line\r\nName: value\r\n...\r\n\r\n") and a case-insensitive header map.
#pragma once

#include "gnut_util.h"

#include <QtCore/QByteArray>
#include <QtCore/QList>
#include <QtCore/QPair>
#include <QtCore/QString>

namespace mx::gnut {

struct HeaderBlock {
    QString firstLine;                       // "GNUTELLA/0.6 200 OK", "GET / HTTP/1.1", "HTTP/1.1 206 Partial Content"
    QList<QPair<QString, QString>> headers;  // in order; continuation lines folded
    // status lines: code + reason ("GNUTELLA/0.6 503 Full" → 503, "Full"); request lines: method/target
    int code = 0;
    QString reason;
    QString method, target, version;
    bool isRequest = false;

    QString value(const QString &name) const;          // all values joined with ", " (case-insensitive)
    QString first(const QString &name) const;          // the first value
    bool has(const QString &name) const;
    void add(const QString &name, const QString &v) { headers << qMakePair(name, v); }
    QByteArray serialize() const;                      // firstLine + headers + empty line
};

// Parse a complete header block (without or with the final empty line).
bool parseHeaderBlock(const QByteArray &block, HeaderBlock *out);

// Incremental, bounded reader: feed bytes; complete() once the empty line was seen. Bytes after
// the block stay in remainder(). Fails on > maxBytes or > maxLines or a line > maxLineLen.
class HeaderReader {
public:
    explicit HeaderReader(int maxBytes = 16384, int maxLines = 100, int maxLineLen = 4096)
        : m_maxBytes(maxBytes), m_maxLines(maxLines), m_maxLine(maxLineLen) {}
    // Returns bytes consumed; check complete()/error().
    void feed(const QByteArray &data);
    bool complete() const { return m_complete; }
    bool error() const { return m_error; }
    QString errorText() const { return m_errorText; }
    const HeaderBlock &block() const { return m_block; }
    QByteArray takeRemainder() { QByteArray r = m_rest; m_rest.clear(); return r; }
    void reset() { *this = HeaderReader(m_maxBytes, m_maxLines, m_maxLine); }
private:
    QByteArray m_buf, m_rest;
    HeaderBlock m_block;
    int m_maxBytes, m_maxLines, m_maxLine;
    bool m_complete = false, m_error = false;
    QString m_errorText;
};

// "X-Try-Ultrapeers: 1.2.3.4:6346, 5.6.7.8:1234" → hosts (max `max`)
QList<HostPort> parseHostList(const QString &value, int max = 64);

} // namespace mx::gnut

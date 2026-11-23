// SPDX-License-Identifier: GPL-3.0-or-later
#include "gnut_headers.h"

namespace mx::gnut {

QString HeaderBlock::value(const QString &name) const
{
    QStringList v;
    for (const auto &h : headers)
        if (h.first.compare(name, Qt::CaseInsensitive) == 0) v << h.second;
    return v.join(QStringLiteral(", "));
}

QString HeaderBlock::first(const QString &name) const
{
    for (const auto &h : headers)
        if (h.first.compare(name, Qt::CaseInsensitive) == 0) return h.second;
    return QString();
}

bool HeaderBlock::has(const QString &name) const
{
    for (const auto &h : headers)
        if (h.first.compare(name, Qt::CaseInsensitive) == 0) return true;
    return false;
}

QByteArray HeaderBlock::serialize() const
{
    QByteArray b = firstLine.toUtf8() + "\r\n";
    for (const auto &h : headers) b += h.first.toUtf8() + ": " + h.second.toUtf8() + "\r\n";
    b += "\r\n";
    return b;
}

bool parseHeaderBlock(const QByteArray &block, HeaderBlock *out)
{
    QByteArray b = block;
    b.replace("\r\n", "\n");
    QList<QByteArray> lines = b.split('\n');
    while (!lines.isEmpty() && lines.last().isEmpty()) lines.removeLast();
    // skip leading empty lines (some servents send a stray CRLF)
    while (!lines.isEmpty() && lines.first().trimmed().isEmpty()) lines.removeFirst();
    if (lines.isEmpty()) return false;
    out->firstLine = QString::fromUtf8(lines.first()).trimmed();
    const QStringList parts = out->firstLine.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.isEmpty()) return false;
    const QString p0 = parts[0];
    if (p0.startsWith(QLatin1String("GNUTELLA/"), Qt::CaseInsensitive) || p0.startsWith(QLatin1String("HTTP/"), Qt::CaseInsensitive)) {
        out->isRequest = false;
        out->version = p0;
        if (parts.size() >= 2) out->code = parts[1].toInt();
        out->reason = parts.mid(2).join(QLatin1Char(' '));
    } else {
        out->isRequest = true;
        out->method = p0;
        if (parts.size() >= 2) out->target = parts[1];
        if (parts.size() >= 3) out->version = parts.last();
        if (p0 == QLatin1String("GNUTELLA") && parts.size() >= 2) out->version = parts[1];   // "GNUTELLA CONNECT/0.6"
    }
    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray &l = lines[i];
        if (l.isEmpty()) break;
        if ((l[0] == ' ' || l[0] == '\t') && !out->headers.isEmpty()) {
            out->headers.last().second += QLatin1Char(' ') + QString::fromUtf8(l).trimmed();
            continue;
        }
        const int c = int(l.indexOf(':'));
        if (c <= 0) continue;                       // tolerate junk lines
        out->headers << qMakePair(QString::fromUtf8(l.left(c)).trimmed(), QString::fromUtf8(l.mid(c + 1)).trimmed());
    }
    return true;
}

void HeaderReader::feed(const QByteArray &data)
{
    if (m_complete || m_error) { m_rest.append(data); return; }
    m_buf.append(data);
    // find end of headers: "\r\n\r\n" or "\n\n"
    int end = -1, endLen = 0;
    const int a = int(m_buf.indexOf("\r\n\r\n"));
    const int b = int(m_buf.indexOf("\n\n"));
    if (a >= 0 && (b < 0 || a <= b)) { end = a; endLen = 4; }
    else if (b >= 0) { end = b; endLen = 2; }
    const int scanTo = end >= 0 ? end : int(m_buf.size());
    if (scanTo > m_maxBytes) { m_error = true; m_errorText = QStringLiteral("header block too large"); return; }
    // line limits
    int lines = 0, lineStart = 0;
    for (int i = 0; i < scanTo; ++i) {
        if (m_buf[i] == '\n') {
            if (i - lineStart > m_maxLine) { m_error = true; m_errorText = QStringLiteral("header line too long"); return; }
            lineStart = i + 1;
            if (++lines > m_maxLines) { m_error = true; m_errorText = QStringLiteral("too many header lines"); return; }
        }
    }
    if (scanTo - lineStart > m_maxLine) { m_error = true; m_errorText = QStringLiteral("header line too long"); return; }
    if (end < 0) return;
    if (!parseHeaderBlock(m_buf.left(end), &m_block)) {
        m_error = true;
        m_errorText = QStringLiteral("malformed header block");
        return;
    }
    m_rest = m_buf.mid(end + endLen);
    m_buf.clear();
    m_complete = true;
}

QList<HostPort> parseHostList(const QString &value, int max)
{
    QList<HostPort> out;
    const QStringList items = value.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &it : items) {
        if (out.size() >= max) break;
        const HostPort h = HostPort::parse(it.trimmed().section(QLatin1Char(' '), 0, 0), 6346);
        if (h.isValid()) out << h;
    }
    return out;
}

} // namespace mx::gnut

// SPDX-License-Identifier: GPL-3.0-or-later
// Little-endian, bounds-checked byte IO for eD2k packets. Never throws: a Reader that runs out of
// data returns zeros and flips ok() to false, so parsers check ok() once at the end.
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>

namespace mx::ed2k {

class Reader {
public:
    Reader() = default;
    explicit Reader(const QByteArray &b) : m_d(b.constData()), m_n(b.size()) {}
    Reader(QByteArray &&) = delete;   // [mac-1] it keeps a pointer into the bytes: never a temporary
    Reader(const char *d, qsizetype n) : m_d(d), m_n(n) {}

    bool ok() const { return m_ok; }
    void fail() { m_ok = false; m_pos = m_n; }
    qsizetype pos() const { return m_pos; }
    qsizetype size() const { return m_n; }
    qsizetype remaining() const { return m_ok ? m_n - m_pos : 0; }
    bool atEnd() const { return remaining() <= 0; }
    bool need(qsizetype k) { if (!m_ok || k < 0 || m_n - m_pos < k) { fail(); return false; } return true; }

    quint8  u8();
    quint16 u16();
    quint32 u32();
    quint64 u64();
    float   f32();
    QByteArray bytes(qsizetype k);       // empty + fail() if not enough
    QByteArray hash16() { return bytes(16); }
    bool skip(qsizetype k) { if (!need(k)) return false; m_pos += k; return true; }
    QByteArray rest() { return bytes(remaining()); }
    // <len 2><bytes>; decoded as UTF-8 when valid (BOM stripped), else Latin-1.
    QString str16(qsizetype maxLen = 0xFFFF);
    QByteArray raw16(qsizetype maxLen = 0xFFFF);
    const char *cur() const { return m_d + m_pos; }

private:
    const char *m_d = nullptr;
    qsizetype m_n = 0;
    qsizetype m_pos = 0;
    bool m_ok = true;
};

class Writer {
public:
    Writer() { m_b.reserve(64); }
    explicit Writer(qsizetype reserve) { m_b.reserve(reserve); }
    Writer &u8(quint8 v)   { m_b.append(char(v)); return *this; }
    Writer &u16(quint16 v);
    Writer &u32(quint32 v);
    Writer &u64(quint64 v);
    Writer &f32(float v);
    Writer &bytes(const QByteArray &b) { m_b.append(b); return *this; }
    Writer &bytes(const char *d, qsizetype n) { m_b.append(d, n); return *this; }
    Writer &hash16(const QByteArray &h);   // pads/truncates to 16 bytes
    Writer &str16(const QString &s);       // UTF-8 (eMule "raw" UTF-8, no BOM)
    Writer &raw16(const QByteArray &b);
    void put32At(qsizetype at, quint32 v);
    qsizetype size() const { return m_b.size(); }
    const QByteArray &data() const { return m_b; }
    QByteArray take() { return std::move(m_b); }
private:
    QByteArray m_b;
};

// eD2k strings: UTF-8 if it decodes cleanly (optional BOM), else Latin-1.
QString decodeString(const QByteArray &raw);

// IPv4 helpers. eD2k transmits IPs as 4 bytes in network order; read as a little-endian uint32 the
// first octet ends up in the low byte. That uint32 is also the "eD2k ID" of a HighID client.
QString ipToString(quint32 ed2kIp);
quint32 ipFromString(const QString &s, bool *ok = nullptr);   // "a.b.c.d" -> eD2k-order uint32
quint32 ipFromHostOrder(quint32 hostOrder);                   // QHostAddress::toIPv4Address() -> eD2k order
quint32 ipToHostOrder(quint32 ed2kIp);
bool isRoutableIp(quint32 ed2kIp);   // not 0/8, 10/8, 127/8, 169.254/16, 172.16/12, 192.168/16, 224+/4
bool isLoopbackIp(quint32 ed2kIp);

QString toHex(const QByteArray &b);
QByteArray fromHex(const QString &s, int expectBytes);   // empty on error

} // namespace mx::ed2k

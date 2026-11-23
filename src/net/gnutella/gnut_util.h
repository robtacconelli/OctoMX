// SPDX-License-Identifier: GPL-3.0-or-later
// Gnutella module: small helpers: logging, GUIDs, little-endian I/O, IPv4 host/port values,
// keyword splitting, speed labels. Private to mxnet_gnutella.
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QLoggingCategory>
#include <QtCore/QString>
#include <QtCore/QStringList>

namespace mx::gnut {

Q_DECLARE_LOGGING_CATEGORY(lcGnut)

constexpr int GuidSize = 16;
constexpr char VendorCode[] = "OCMX";       // our 4-letter vendor code in query hits / pongs (the legacy code "WMXN" is shown as OctoMX too)

// Random 16-byte message id. modern = byte 8 is 0xFF, byte 15 = 0 (the old "modern servent" mark).
QByteArray randomGuid(bool modern = true);
QString guidHex(const QByteArray &g);                 // 32 upper-case hex chars
QByteArray guidFromHex(const QString &s);             // empty unless 32 hex chars

// ---- little-endian helpers
inline void putU8(QByteArray &b, quint8 v) { b.append(char(v)); }
inline void putU16(QByteArray &b, quint16 v) { b.append(char(v & 0xff)); b.append(char(v >> 8)); }
inline void putU32(QByteArray &b, quint32 v) { for (int i = 0; i < 4; ++i) b.append(char((v >> (8 * i)) & 0xff)); }
inline void putIp(QByteArray &b, quint32 ip) { for (int i = 3; i >= 0; --i) b.append(char((ip >> (8 * i)) & 0xff)); }

// Bounds-checked sequential reader; every read past the end sets !ok() and returns 0 / empty.
class Reader {
public:
    explicit Reader(const QByteArray &b, int pos = 0) : m_b(b), m_pos(pos) {}
    bool ok() const { return m_ok; }
    int pos() const { return m_pos; }
    int left() const { return m_ok ? int(m_b.size()) - m_pos : 0; }
    bool atEnd() const { return left() <= 0; }
    void seek(int p) { if (p < 0 || p > m_b.size()) m_ok = false; else m_pos = p; }
    quint8 u8();
    quint16 u16();
    quint32 u32();
    quint32 ip();                                 // 4 bytes, network order a.b.c.d
    QByteArray bytes(int n);
    QByteArray cstr(int maxLen);                  // up to NUL (consumed); fails if no NUL within maxLen
    const QByteArray &buffer() const { return m_b; }
private:
    const QByteArray &m_b;
    int m_pos = 0;
    bool m_ok = true;
};

// ---- IPv4 host:port
struct HostPort {
    quint32 ip = 0;      // host order (a<<24 | b<<16 | c<<8 | d)
    quint16 port = 0;
    bool isValid() const { return ip != 0 && port != 0; }
    QString toString() const;
    bool operator==(const HostPort &o) const { return ip == o.ip && port == o.port; }
    bool operator!=(const HostPort &o) const { return !(*this == o); }
    quint64 key() const { return (quint64(ip) << 16) | port; }
    static HostPort parse(const QString &s, quint16 defaultPort = 0);   // "a.b.c.d[:port]"
};
inline size_t qHash(const HostPort &h, size_t seed = 0) { return ::qHash(h.key(), seed); }

QString ipToString(quint32 ip);
quint32 ipFromString(const QString &s, bool *ok = nullptr);
bool isPrivateIp(quint32 ip);                   // 10/8, 172.16/12, 192.168/16, 127/8, 169.254/16, 100.64/10
bool isBogonIp(quint32 ip);                     // 0/8, 224+/4 multicast & reserved, 255.255.255.255
bool isRoutableIp(quint32 ip);                  // !private && !bogon
// "ip:port" packed 6 bytes (IP big-endian, port little-endian) as in GGEP IPP / PUSH / ALT
QList<HostPort> unpackHosts6(const QByteArray &data, int max = 200);
QByteArray packHost6(const HostPort &h);

// ---- text
// Lower-case keywords for matching / QRP: split on anything that is not a letter or a digit.
QStringList splitKeywords(const QString &text);
// Connection label for a Gnutella speed field (kbit/s): always one of the modern short labels of
// mx::connectionTypes() ("DSL", "VDSL", "FTTC", "Cable", "FTTB", "FTTH", "2.5 Gbit", ...), never
// the original WinMX dial-up list ("56K", "T1", ...), so the UI needs no special case.
QString speedLabel(quint32 kbps);
// The speed field we send in our own query hits for our connection type (identity/connection);
// speedLabel(speedForConnection(x)) == x for the wired types.
quint32 speedForConnection(const QString &connectionType);
// Vendor code (4 chars) → readable client name ("LIME" → "LimeWire").
QString vendorName(const QByteArray &code);
// Percent-encode for HTTP paths (RFC 3986 unreserved kept).
QByteArray urlEncodePath(const QString &s);

} // namespace mx::gnut

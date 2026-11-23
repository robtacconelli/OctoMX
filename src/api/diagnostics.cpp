// SPDX-License-Identifier: GPL-3.0-or-later
// [diag-1] report helpers: levels, address classification and masking, the plain-text report.
#include "mx/diagnostics.h"

#include <QtCore/QMutex>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtNetwork/QHostAddress>

namespace mx {

QString diagLevelName(DiagLevel l)
{
    switch (l) {
    case DiagLevel::Ok: return QStringLiteral("OK");
    case DiagLevel::Limited: return QStringLiteral("LIMITED");
    case DiagLevel::Broken: return QStringLiteral("BROKEN");
    case DiagLevel::Off: return QStringLiteral("OFF");
    case DiagLevel::Unknown: break;
    }
    return QStringLiteral("UNKNOWN");
}

static int severity(DiagLevel l)
{
    switch (l) {
    case DiagLevel::Off: return 0;
    case DiagLevel::Unknown: return 1;
    case DiagLevel::Ok: return 2;
    case DiagLevel::Limited: return 3;
    case DiagLevel::Broken: return 4;
    }
    return 1;
}

DiagLevel diagWorse(DiagLevel a, DiagLevel b)
{
    return severity(a) >= severity(b) ? a : b;
}

// ------------------------------------------------------------------------------------ addresses
static QString stripPort(QString a)
{
    a = a.trimmed();
    if (a.startsWith(QLatin1Char('['))) {
        const int e = int(a.indexOf(QLatin1Char(']')));
        return e > 0 ? a.mid(1, e - 1) : a.mid(1);
    }
    if (a.count(QLatin1Char(':')) == 1) return a.section(QLatin1Char(':'), 0, 0);
    const int pct = int(a.indexOf(QLatin1Char('%')));
    if (pct > 0) a = a.left(pct);
    return a;
}

AddressKind classifyAddress(const QString &address)
{
    const QString s = stripPort(address);
    // QHostAddress accepts "1" or "1.2" as IPv4 shorthands; insist on the dotted quad
    static const QRegularExpression quad(QStringLiteral("^\\d{1,3}(\\.\\d{1,3}){3}$"));
    if (!s.contains(QLatin1Char(':')) && !quad.match(s).hasMatch()) return AddressKind::Invalid;
    QHostAddress a;
    if (!a.setAddress(s)) return AddressKind::Invalid;
    if (a.protocol() == QAbstractSocket::IPv6Protocol) {
        bool ok = false;
        const QHostAddress v4(a.toIPv4Address(&ok));
        if (ok) return classifyAddress(v4.toString());
    }
    if (a.isLoopback()) return AddressKind::Loopback;
    if (a.isLinkLocal()) return AddressKind::LinkLocal;
    if (a.protocol() == QAbstractSocket::IPv4Protocol) {
        const quint32 v = a.toIPv4Address();
        if ((v & 0xFFC00000u) == 0x64400000u) return AddressKind::CarrierNat;   // 100.64.0.0/10
        if (a.isPrivateUse() || (v >> 24) == 0) return AddressKind::Private;
        if (a.isMulticast() || a.isBroadcast()) return AddressKind::Private;
        return AddressKind::Public;
    }
    if (a.isPrivateUse() || a.isUniqueLocalUnicast() || a.isSiteLocal() || a.isMulticast() || a.isNull())
        return AddressKind::Private;
    return AddressKind::Public;
}

bool isPublicAddress(const QString &address) { return classifyAddress(address) == AddressKind::Public; }

QString AddressMasker::mask(const QString &text)
{
    // IPv4 dotted quads, and IPv6 runs of hex groups with at least two colons
    static const QRegularExpression re(QStringLiteral(
        "(?<![\\w.:-])(\\d{1,3}(?:\\.\\d{1,3}){3})(?![\\w.-])"
        "|(?<![\\w:])((?:[0-9A-Fa-f]{0,4}:){2,7}[0-9A-Fa-f]{0,4})(?![\\w:])"));
    QString out;
    out.reserve(text.size());
    qsizetype last = 0;
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const int g = m.capturedStart(1) >= 0 ? 1 : 2;
        const QString addr = m.captured(g);
        if (g == 2 && addr.count(QLatin1Char(':')) < 2) continue;
        if (classifyAddress(addr) != AddressKind::Public) continue;
        out += text.mid(last, m.capturedStart(g) - last);
        QString &rep = m_map[addr];
        if (rep.isEmpty()) rep = QStringLiteral("ext-%1").arg(m_map.size());
        out += rep;
        last = m.capturedEnd(g);
    }
    out += text.mid(last);
    return out;
}

QString diagGroupIdForDownload(const QString &transferId) { return QStringLiteral("dl.") + transferId; }

// ------------------------------------------------------------------------------------ probe datagrams
namespace {
QMutex g_probeMutex;
QSet<QString> g_probeSeen;   // "nonce:port"
const QByteArray kProbeMagic("WMXNG-PROBE ");
}

QByteArray probeDatagram(const QString &nonce, quint16 port)
{
    return kProbeMagic + nonce.toLatin1() + ' ' + QByteArray::number(port);
}

bool consumeProbeDatagram(const QByteArray &datagram)
{
    if (!datagram.startsWith(kProbeMagic) || datagram.size() > 80) return false;
    const QList<QByteArray> parts = datagram.mid(kProbeMagic.size()).trimmed().split(' ');
    if (parts.size() != 2 || parts.at(0).size() > 40) return true;
    QMutexLocker lock(&g_probeMutex);
    if (g_probeSeen.size() > 256) g_probeSeen.clear();
    g_probeSeen.insert(QString::fromLatin1(parts.at(0)) + QLatin1Char(':') + QString::fromLatin1(parts.at(1)));
    return true;
}

bool probeDatagramSeen(const QString &nonce, quint16 port)
{
    QMutexLocker lock(&g_probeMutex);
    return g_probeSeen.contains(nonce + QLatin1Char(':') + QString::number(port));
}

// ------------------------------------------------------------------------------------ text report
QString diagReportText(const DiagReport &r, bool hideAddresses)
{
    AddressMasker masker;
    auto m = [&](const QString &s) { return hideAddresses ? masker.mask(s) : s; };
    QStringList lines;
    lines << QStringLiteral("OctoMX connection report");
    lines << QStringLiteral("Version:   OctoMX %1").arg(r.version);
    lines << QStringLiteral("System:    %1").arg(r.os);
    lines << QStringLiteral("Generated: %1").arg(r.generated.toString(Qt::ISODate));
    lines << QStringLiteral("Last test: %1").arg(r.lastTest.isValid() ? r.lastTest.toString(Qt::ISODate)
                                                                      : QStringLiteral("not run in this session"));
    lines << QStringLiteral("Public addresses hidden: %1").arg(hideAddresses ? QStringLiteral("yes (ext-1, ext-2, ...)")
                                                                             : QStringLiteral("no"));
    lines << QString();
    lines << QStringLiteral("Summary: [%1] %2").arg(diagLevelName(r.level), m(r.summary));
    bool downloadsHeader = false;
    for (const DiagGroup &g : r.groups) {
        if (!g.downloadId.isEmpty() && !downloadsHeader) {
            downloadsHeader = true;
            lines << QString() << QStringLiteral("==== Downloads ====");
        }
        lines << QString();
        lines << QStringLiteral("== %1: [%2] %3").arg(m(g.title), diagLevelName(g.level), m(g.result));
        if (!g.explain.isEmpty()) lines << QStringLiteral("   %1").arg(m(g.explain));
        for (const DiagItem &it : g.items) {
            QString line = QStringLiteral("  [%1] %2: %3").arg(diagLevelName(it.level), -7).arg(m(it.check), m(it.result));
            if (!it.details.isEmpty()) line += QStringLiteral(" (%1)").arg(m(it.details));
            lines << line;
            if (!it.explain.isEmpty() && it.level != DiagLevel::Ok && it.level != DiagLevel::Off)
                lines << QStringLiteral("            %1").arg(m(it.explain));
        }
    }
    lines << QString();
    lines << QStringLiteral("== Recent warnings and errors (%1 lines, from %2)")
                 .arg(r.logLines.size()).arg(r.logFile.isEmpty() ? QStringLiteral("the log") : QStringLiteral("octomx.log"));
    for (const QString &l : r.logLines) lines << m(l);
    lines << QString();
    return lines.join(QLatin1Char('\n'));
}

} // namespace mx

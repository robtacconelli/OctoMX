// SPDX-License-Identifier: GPL-3.0-or-later
// [diag-1] "probe my ports" between two OctoMX nodes: the helper connects back to the requester's
// own address on the announced ports only (TCP connect, DHT ping, probe datagrams), answers once,
// refuses a second request within a minute and honours diag/answerProbes.
#include "ovl_test_support.h"

#include <QtNetwork/QTcpServer>
#include <QtNetwork/QUdpSocket>

using namespace mx;
using namespace ovltest;

namespace {
int freePort(bool udp)
{
    if (udp) {
        QUdpSocket s;
        s.bind(QHostAddress::LocalHost, 0);
        return s.localPort();
    }
    QTcpServer s;
    s.listen(QHostAddress::LocalHost, 0);
    return s.serverPort();
}

PortProbe probe(const QString &proto, int port, const QString &kind = QString())
{
    PortProbe p;
    p.proto = proto;
    p.port = port;
    p.kind = kind;
    return p;
}

PortProbe resultFor(const QList<PortProbe> &l, const QString &proto, int port)
{
    for (const PortProbe &p : l)
        if (p.proto == proto && p.port == port) return p;
    return {};
}
} // namespace

class TstOverlayProbe : public QObject {
    Q_OBJECT
private slots:
    void twoNodeProbe();
};

void TstOverlayProbe::twoNodeProbe()
{
    Instance A(QStringLiteral("Alice"));
    Instance B(QStringLiteral("Bob"));
    A.start();
    B.start();
    QVERIFY(link(A, B));

    // what the requester (Alice) listens on
    QTcpServer open;
    QVERIFY(open.listen(QHostAddress::AnyIPv4, 0));
    const int closedTcp = freePort(false);
    QUdpSocket datagramSock;   // stands for the eD2k / Gnutella socket: hands datagrams to the probe registry
    QVERIFY(datagramSock.bind(QHostAddress::AnyIPv4, 0));
    int consumed = 0;
    connect(&datagramSock, &QUdpSocket::readyRead, &datagramSock, [&] {
        while (datagramSock.hasPendingDatagrams()) {
            QByteArray d(int(datagramSock.pendingDatagramSize()), 0);
            datagramSock.readDatagram(d.data(), d.size());
            if (consumeProbeDatagram(d)) ++consumed;
        }
    });
    const int silentUdp = freePort(true);
    QUdpSocket dhtNode;        // stands for libtorrent's DHT: answers pings
    QVERIFY(dhtNode.bind(QHostAddress::AnyIPv4, 0));
    connect(&dhtNode, &QUdpSocket::readyRead, &dhtNode, [&] {
        while (dhtNode.hasPendingDatagrams()) {
            QByteArray d(int(dhtNode.pendingDatagramSize()), 0);
            QHostAddress from;
            quint16 port = 0;
            dhtNode.readDatagram(d.data(), d.size(), &from, &port);
            const int t = int(d.indexOf("1:t2:"));
            if (t < 0) continue;
            dhtNode.writeDatagram(QByteArray("d1:rd2:id20:") + QByteArray(20, 'x') + "e1:t2:" + d.mid(t + 5, 2) + "1:y1:re", from, port);
        }
    });

    const QList<PortProbe> ask = {probe(QStringLiteral("TCP"), open.serverPort()), probe(QStringLiteral("TCP"), closedTcp),
                                  probe(QStringLiteral("UDP"), datagramSock.localPort(), QStringLiteral("datagram")),
                                  probe(QStringLiteral("UDP"), silentUdp, QStringLiteral("datagram")),
                                  probe(QStringLiteral("UDP"), dhtNode.localPort(), QStringLiteral("dht"))};
    QSignalSpy spy(A.plugin, &NetworkPlugin::portProbeResult);
    QVERIFY(A.plugin->probePorts(7, ask));
    QVERIFY(spy.wait(20000));
    QCOMPARE(spy.first().at(0).toUInt(), 7u);
    const QList<PortProbe> res = spy.first().at(1).value<QList<PortProbe>>();
    QCOMPARE(res.size(), ask.size());
    QCOMPARE(int(resultFor(res, QStringLiteral("TCP"), open.serverPort()).reach), int(Reach::Yes));
    QCOMPARE(int(resultFor(res, QStringLiteral("TCP"), closedTcp).reach), int(Reach::No));
    QCOMPARE(int(resultFor(res, QStringLiteral("UDP"), datagramSock.localPort()).reach), int(Reach::Yes));
    QCOMPARE(int(resultFor(res, QStringLiteral("UDP"), silentUdp).reach), int(Reach::No));
    QCOMPARE(int(resultFor(res, QStringLiteral("UDP"), dhtNode.localPort()).reach), int(Reach::Yes));
    QVERIFY(resultFor(res, QStringLiteral("TCP"), open.serverPort()).helper.contains(QLatin1String("Bob")));
    QVERIFY(consumed >= 1);

    // a second request within a minute is refused (rate limit), nothing is tested
    spy.clear();
    const int beforeConsumed = consumed;
    QVERIFY(A.plugin->probePorts(8, ask));
    QVERIFY(spy.wait(20000));
    const QList<PortProbe> again = spy.first().at(1).value<QList<PortProbe>>();
    QCOMPARE(int(resultFor(again, QStringLiteral("TCP"), open.serverPort()).reach), int(Reach::Unknown));
    QVERIFY(resultFor(again, QStringLiteral("TCP"), open.serverPort()).error.contains(QLatin1String("too often")));
    QCOMPARE(consumed, beforeConsumed);

    // a user who does not test ports says so
    Instance C(QStringLiteral("Carol"));
    C.settings.setValue(QString(keys::diag::AnswerProbes), false);
    C.start();
    Instance D(QStringLiteral("Dave"));
    D.start();
    QVERIFY(link(C, D));
    QSignalSpy spy2(D.plugin, &NetworkPlugin::portProbeResult);
    QVERIFY(D.plugin->probePorts(9, {probe(QStringLiteral("TCP"), open.serverPort())}));
    QVERIFY(spy2.wait(20000));
    const QList<PortProbe> declined = spy2.first().at(1).value<QList<PortProbe>>();
    QCOMPARE(int(declined.first().reach), int(Reach::Unknown));
    QVERIFY(declined.first().error.contains(QLatin1String("does not test ports")));

    // no connected peer: nothing to ask
    Instance E(QStringLiteral("Erin"));
    E.start();
    QVERIFY(!E.plugin->probePorts(10, ask));
    // diagnostics: bindings of the overlay listen port
    const PluginDiagnostics d = A.plugin->diagnostics();
    QVERIFY(d.valid);
    QVERIFY(!d.bindings.isEmpty());
    QCOMPARE(d.bindings.first().port, int(A.plugin->listenPort()));
}

QTEST_MAIN(TstOverlayProbe)
#include "tst_overlay_probe.moc"

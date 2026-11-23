// SPDX-License-Identifier: GPL-3.0-or-later
// Gnutella module: browse-host client: "GET /" with Accept: application/x-gnutella-packets,
// optionally deflated, body = a stream of QueryHit descriptors → BrowseEntry batches.
#pragma once

#include "gnut_headers.h"
#include "gnut_message.h"
#include "gnut_zlib.h"
#include "mx/types.h"

#include <QtCore/QObject>
#include <QtCore/QTimer>
#include <memory>

#include <functional>

class QTcpSocket;

namespace mx::gnut {

class BrowseClient : public QObject {
    Q_OBJECT
public:
    BrowseClient(const HostPort &addr, const QString &userAgent, QObject *parent = nullptr);
    ~BrowseClient() override;
    void start();                               // direct connection
    void startWithSocket(QTcpSocket *s);        // pushed connection (GIV already consumed)
    void abort();
    HostPort address() const { return m_addr; }
    int received() const { return m_count; }
    static constexpr int MaxEntries = 50000;
    // [enc-1] makes the direct connection's socket (TLS when encryption is on); default plain
    void setSocketFactory(std::function<QTcpSocket *(QObject *)> f) { m_factory = std::move(f); }

signals:
    void entries(const QList<mx::BrowseEntry> &batch, bool finished);
    void failed(const QString &reason);

private:
    void attach(QTcpSocket *s);
    void onReadyRead();
    void processBody(const QByteArray &raw);
    void finish(bool ok, const QString &why);

    HostPort m_addr;
    QString m_ua;
    QTcpSocket *m_sock = nullptr;
    HeaderReader m_hdr;
    bool m_inBody = false;
    std::unique_ptr<Inflater> m_inflater;
    Framer m_framer;
    qint64 m_contentLeft = -1;
    qint64 m_rawTotal = 0;
    QList<mx::BrowseEntry> m_batch;
    int m_count = 0;
    QTimer m_timeout;
    bool m_done = false;
    std::function<QTcpSocket *(QObject *)> m_factory;
};

} // namespace mx::gnut

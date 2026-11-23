// SPDX-License-Identifier: GPL-3.0-or-later
// http_probe <url> [expected-sha256] [--out <dir>] [--conns N] [--limit <kB/s>] [--log <file>]
// Live proof of mxnet_http: uses the plugin + RangeSource exactly like the download engine
// (openSource with unknown size → probe → extra fields → mini range scheduler with
// maxPendingRequests() parallel requests), writes the file under --out (default build/http-probe),
// prints the speed and verifies SHA-256. Exit 0 only if the download completed (and the hash matched).
#include <QtCore/QCommandLineParser>
#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QMutex>
#include <QtCore/QSysInfo>
#include <QtCore/QTimer>
#include <QtNetwork/QSslSocket>
#include <cstdio>

#include "http_plugin.h"
#include "mini_scheduler.h"
#include "mx/settings_keys.h"
#include "mx/simple_settings.h"

namespace {
QFile *g_log = nullptr;
void out(const QString &s)
{
    const QByteArray b = s.toUtf8() + '\n';
    fwrite(b.constData(), 1, size_t(b.size()), stdout);
    fflush(stdout);
    if (g_log) { g_log->write(b); g_log->flush(); }
}

class Limiter : public mx::RateLimiter {
public:
    explicit Limiter(qint64 rate) : m_rate(rate) { m_clock.start(); }
    qint64 acquire(Dir d, qint64 bytes) override
    {
        if (d != Down || m_rate <= 0) return bytes;
        QMutexLocker l(&m_m);
        const qint64 now = m_clock.nsecsElapsed();
        m_tokens = qMin<double>(qMax<double>(m_rate, 4096), m_tokens + double(m_rate) * double(now - m_last) / 1e9);
        m_last = now;
        const qint64 n = qMin<qint64>(bytes, qint64(m_tokens));
        m_tokens -= n;
        return n;
    }
    void account(Dir d, mx::Net, qint64 bytes) override { if (d == Down) total += bytes; }
    int limitBps(Dir d) const override { return d == Down ? int(m_rate) : 0; }
    qint64 total = 0;
private:
    QMutex m_m;
    qint64 m_rate;
    double m_tokens = 0;
    qint64 m_last = 0;
    QElapsedTimer m_clock;
};
QString mb(qint64 n) { return QString::number(n / 1048576.0, 'f', 1) + QStringLiteral(" MiB"); }
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCommandLineParser p;
    p.addHelpOption();
    p.addPositionalArgument(QStringLiteral("url"), QStringLiteral("http(s) URL"));
    p.addPositionalArgument(QStringLiteral("sha256"), QStringLiteral("expected SHA-256 (hex), optional"));
    QCommandLineOption outOpt(QStringLiteral("out"), QStringLiteral("output directory"), QStringLiteral("dir"));
    QCommandLineOption connOpt(QStringLiteral("conns"), QStringLiteral("connections per source"), QStringLiteral("n"), QStringLiteral("4"));
    QCommandLineOption limOpt(QStringLiteral("limit"), QStringLiteral("download limit kB/s"), QStringLiteral("kBps"), QStringLiteral("0"));
    QCommandLineOption logOpt(QStringLiteral("log"), QStringLiteral("also write output to this file"), QStringLiteral("file"));
    QCommandLineOption keepOpt(QStringLiteral("keep"), QStringLiteral("keep the downloaded file"));
    p.addOptions({outOpt, connOpt, limOpt, logOpt, keepOpt});
    p.process(app);
    const QStringList pos = p.positionalArguments();
    if (pos.isEmpty()) p.showHelp(2);
    const QString url = pos.at(0);
    const QByteArray expectedHash = pos.value(1).toLatin1().toLower();
    QFile logFile(p.value(logOpt));
    if (p.isSet(logOpt) && logFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) g_log = &logFile;

    out(QStringLiteral("http_probe on %1 | Qt %2 | TLS %3 (%4)")
            .arg(QSysInfo::prettyProductName(), QString::fromLatin1(qVersion()), QSslSocket::activeBackend(),
                 QSslSocket::sslLibraryVersionString()));

    mx::SimpleSettings settings;
    settings.setValue(QStringLiteral("http/connectionsPerSource"), p.value(connOpt).toInt());
    settings.setValue(mx::keys::http::MaxConnectionsPerServer, qMax(4, p.value(connOpt).toInt()));
    Limiter limiter(p.value(limOpt).toLongLong() * 1024);
    mx::NetworkPlugin *plugin = mx::createHttpPlugin(&app);
    mx::PluginContext ctx;
    ctx.settings = &settings;
    ctx.limiter = &limiter;
    plugin->init(ctx);
    plugin->start();
    QObject::connect(plugin, &mx::NetworkPlugin::sourcesFound, [](const QString &, const QList<mx::SearchSource> &s) {
        for (const auto &x : s) out(QStringLiteral("  mirror hint: %1").arg(x.uri));
    });
    QObject::connect(plugin, &mx::NetworkPlugin::hashesLearned, [](const QString &, const mx::FileHashes &h) {
        out(QStringLiteral("  hash hint: %1").arg(QString::fromLatin1(QJsonDocument::fromVariant(h.toVariant()).toJson(QJsonDocument::Compact))));
    });

    mx::SearchSource ss;
    ss.net = mx::Net::Http;
    ss.uri = url;
    mx::RangeSource *src = plugin->openSource(QStringLiteral("probe"), {}, 0, ss);   // size unknown, like a pasted link
    if (!src) { out(QStringLiteral("openSource refused the URL")); return 2; }

    QElapsedTimer total;
    total.start();
    QFile file;
    mxtest::MiniScheduler *sched = nullptr;
    int rc = 1;
    qint64 lastBytes = 0, lastMs = 0;
    QTimer progress;

    auto finish = [&](int code) { rc = code; QTimer::singleShot(0, &app, &QCoreApplication::quit); };

    QObject::connect(src, &mx::RangeSource::stateChanged, &app, [&] {
        out(QStringLiteral("[%1 ms] state %2: %3").arg(total.elapsed()).arg(int(src->state())).arg(src->stateText()));
        if (sched) return;
        if (src->state() == mx::RangeSource::Failed) { finish(1); return; }
        if (src->state() != mx::RangeSource::Ready) return;
        const QVariantMap x = src->descriptor().extra;
        out(QStringLiteral("  finalUrl      %1").arg(x.value(QStringLiteral("finalUrl")).toString()));
        out(QStringLiteral("  size          %1").arg(x.value(QStringLiteral("size")).toLongLong()));
        out(QStringLiteral("  fileName      %1").arg(x.value(QStringLiteral("fileName")).toString()));
        out(QStringLiteral("  contentType   %1").arg(x.value(QStringLiteral("contentType")).toString()));
        out(QStringLiteral("  ranges        %1 | maxPending %2 | requestSize %3")
                .arg(x.value(QStringLiteral("supportsRanges")).toBool()).arg(src->maxPendingRequests()).arg(src->preferredRequestSize()));
        out(QStringLiteral("  etag          %1 | lastModified %2 | torrent %3 | server %4")
                .arg(x.value(QStringLiteral("etag")).toString(), x.value(QStringLiteral("lastModified")).toString())
                .arg(x.value(QStringLiteral("isTorrentFile")).toBool()).arg(src->clientName()));
        const QString dir = p.isSet(outOpt) ? p.value(outOpt) : QStringLiteral("build/http-probe");
        QDir().mkpath(dir);
        QString name = x.value(QStringLiteral("fileName")).toString();
        if (name.isEmpty()) name = QStringLiteral("download.bin");
        file.setFileName(dir + QLatin1Char('/') + name);
        if (!file.open(QIODevice::ReadWrite | QIODevice::Truncate)) { out(QStringLiteral("cannot write ") + file.fileName()); finish(2); return; }
        const qint64 size = x.value(QStringLiteral("size")).toLongLong();
        if (size > 0) file.resize(size);
        sched = new mxtest::MiniScheduler(src, &file, size, 0, &app);
        QObject::connect(sched, &mxtest::MiniScheduler::finished, &app, [&](bool ok) {
            progress.stop();
            const double secs = sched->elapsedMs() / 1000.0;
            out(QStringLiteral("download %1: %2 in %3 s = %4 MiB/s (max %5 parallel requests, %6 failed requests, chunk max %7 B)")
                    .arg(ok ? QStringLiteral("complete") : QStringLiteral("FAILED: ") + sched->error)
                    .arg(mb(sched->finalSize)).arg(secs, 0, 'f', 1).arg(sched->finalSize / 1048576.0 / qMax(0.001, secs), 0, 'f', 2)
                    .arg(sched->maxInFlight).arg(sched->failures).arg(sched->maxChunk));
            if (!ok) { finish(1); return; }
            if (sched->finalSize >= 0) file.resize(sched->finalSize);
            file.seek(0);
            QCryptographicHash h(QCryptographicHash::Sha256);
            h.addData(&file);
            const QByteArray got = h.result().toHex();
            out(QStringLiteral("sha256 %1 %2").arg(QString::fromLatin1(got), file.fileName()));
            int code = 0;
            if (!expectedHash.isEmpty()) {
                const bool match = got == expectedHash;
                out(match ? QStringLiteral("HASH OK") : QStringLiteral("HASH MISMATCH (expected %1)").arg(QString::fromLatin1(expectedHash)));
                code = match ? 0 : 3;
            }
            file.close();
            if (!p.isSet(keepOpt)) file.remove();
            src->close();
            finish(code);
        });
        progress.setInterval(5000);
        QObject::connect(&progress, &QTimer::timeout, &app, [&] {
            const qint64 now = sched->elapsedMs();
            const qint64 b = sched->received;
            out(QStringLiteral("  %1 received, %2 MiB/s").arg(mb(b)).arg((b - lastBytes) / 1048576.0 / qMax<qint64>(1, now - lastMs) * 1000.0, 0, 'f', 2));
            lastBytes = b;
            lastMs = now;
        });
        progress.start();
        sched->start();
    });
    src->open();
    app.exec();
    delete src;
    return rc;
}

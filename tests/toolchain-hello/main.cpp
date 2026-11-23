// SPDX-License-Identifier: GPL-3.0-or-later
// toolchain-hello: proves Qt Widgets/Network/Sql + static libtorrent/OpenSSL/zlib
// work on both Linux and Windows (wine).
//   toolchain_hello [--screenshot out.png] [--exit-after <sec>] [--url <https url>]
//                   [--no-network] [--multimedia] [--font <ttf>]
#include <QApplication>
#include <QAudioDevice>
#include <QCommandLineParser>
#include <QFontDatabase>
#include <QHeaderView>
#include <QMediaDevices>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPushButton>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSslSocket>
#include <QStyleFactory>
#include <QSvgRenderer>
#include <QPainter>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <libtorrent/version.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/hasher.hpp>
#include <libtorrent/hex.hpp>

#include <openssl/crypto.h>
#include <zlib.h>

#include <cstdio>

static int g_failures = 0;

static void report(QTreeWidget *tree, const QString &key, const QString &value, bool ok = true)
{
    std::printf("%-22s %s%s\n", qPrintable(key), qPrintable(value), ok ? "" : "   <-- FAIL");
    std::fflush(stdout);
    if (!ok) ++g_failures;
    auto *item = new QTreeWidgetItem(tree, {key, value});
    if (!ok) item->setForeground(1, QColor(255, 64, 64));
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setStyle(QStyleFactory::create("Windows"));

    QCommandLineParser p;
    p.addHelpOption();
    QCommandLineOption shotOpt("screenshot", "Grab the window to <file> and quit.", "file");
    QCommandLineOption exitOpt("exit-after", "Quit after <sec> seconds.", "sec");
    QCommandLineOption urlOpt("url", "HTTPS URL to GET.", "url", "https://www.example.com/");
    QCommandLineOption noNetOpt("no-network", "Skip the HTTPS GET.");
    QCommandLineOption mmOpt("multimedia", "Enumerate audio outputs (Qt Multimedia).");
    QCommandLineOption fontOpt("font", "Load <ttf> and use its family at 8 pt as app font.", "ttf");
    p.addOptions({shotOpt, exitOpt, urlOpt, noNetOpt, mmOpt, fontOpt});
    p.process(app);
    if (p.isSet(fontOpt)) {
        const int id = QFontDatabase::addApplicationFont(p.value(fontOpt));
        const QStringList fams = QFontDatabase::applicationFontFamilies(id);
        if (!fams.isEmpty()) {
            QFont f(fams.first());
            f.setPointSize(8);
            f.setHintingPreference(QFont::PreferFullHinting);
            f.setStyleStrategy(QFont::NoAntialias);   // classic Win9x look: no AA
            QApplication::setFont(f);
        }
    }

    QWidget win;
    win.setWindowTitle("OctoMX toolchain hello");
    auto *lay = new QVBoxLayout(&win);
    auto *tree = new QTreeWidget;
    tree->setHeaderLabels({"Check", "Result"});
    tree->setRootIsDecorated(false);
    QPalette pal = tree->palette();
    pal.setColor(QPalette::Base, Qt::black);
    pal.setColor(QPalette::Window, Qt::black);
    pal.setColor(QPalette::Text, QColor(0, 255, 0));
    tree->setPalette(pal);
    tree->setStyleSheet("QTreeWidget { background: black; color: #00ff00; }");
    lay->addWidget(tree);
    auto *btn = new QPushButton("Quit");
    {   // prove QtSvg works: render a tiny SVG as the button icon
        QSvgRenderer svg(QByteArray("<svg xmlns='http://www.w3.org/2000/svg' width='16' height='16'>"
                                    "<circle cx='8' cy='8' r='7' fill='#00a000'/></svg>"));
        QPixmap pm(16, 16); pm.fill(Qt::transparent);
        QPainter ptr(&pm); svg.render(&ptr); ptr.end();
        btn->setIcon(QIcon(pm));
    }
    QObject::connect(btn, &QPushButton::clicked, &app, &QApplication::quit);
    lay->addWidget(btn);
    win.resize(640, 420);

    report(tree, "Qt", QString::fromLatin1(qVersion()));
    report(tree, "Platform", QGuiApplication::platformName() + " / style " + QApplication::style()->name());
    report(tree, "Font", QApplication::font().family() + " " + QString::number(QApplication::font().pointSizeF()));
    report(tree, "libtorrent", QString::fromLatin1(lt::version()));
    report(tree, "OpenSSL (static)", QString::fromLatin1(OpenSSL_version(OPENSSL_VERSION)));
    report(tree, "zlib", QString::fromLatin1(zlibVersion()));
    report(tree, "Boost", QString::number(BOOST_VERSION / 100000) + "." + QString::number(BOOST_VERSION / 100 % 1000));

    {   // libtorrent's SHA-1 goes through libcrypto (TORRENT_USE_LIBCRYPTO)
        lt::hasher h("abc", 3);
        const std::string hex = lt::aux::to_hex(h.final());
        report(tree, "lt::hasher sha1(abc)", QString::fromStdString(hex),
               hex == "a9993e364706816aba3e25717850c26c9cd0d89d");
    }
    {   // start + stop a real session (no DHT bootstrap / UPnP so it is quick)
        lt::settings_pack sp;
        sp.set_str(lt::settings_pack::listen_interfaces, "127.0.0.1:0");
        sp.set_bool(lt::settings_pack::enable_dht, false);
        sp.set_bool(lt::settings_pack::enable_lsd, false);
        sp.set_bool(lt::settings_pack::enable_upnp, false);
        sp.set_bool(lt::settings_pack::enable_natpmp, false);
        lt::session ses(lt::session_params(std::move(sp)));
        const bool listening = ses.is_listening();
        report(tree, "lt::session", listening ? QString("listening on port %1").arg(ses.listen_port())
                                              : QString("not listening"), listening);
    }

    {   // SQLite in memory
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "mem");
        db.setDatabaseName(":memory:");
        bool ok = db.open();
        QString v;
        if (ok) {
            QSqlQuery q(db);
            ok = q.exec("CREATE TABLE t(name TEXT)") && q.exec("INSERT INTO t VALUES('OctoMX')")
                 && q.exec("SELECT name || ' / sqlite ' || sqlite_version() FROM t") && q.next();
            v = ok ? q.value(0).toString() : q.lastError().text();
        } else {
            v = db.lastError().text();
        }
        report(tree, "QSqlDatabase", v, ok);
    }
    {   // QtConcurrent
        const int r = QtConcurrent::run([] { return 6 * 7; }).result();
        report(tree, "QtConcurrent", QString::number(r), r == 42);
    }
    if (p.isSet(mmOpt)) {
        const auto outs = QMediaDevices::audioOutputs();
        report(tree, "Audio outputs", QString::number(outs.size()) +
               (outs.isEmpty() ? QString() : " (" + outs.first().description() + ")"));
    }

    report(tree, "TLS backend", QSslSocket::activeBackend() + " / " + QSslSocket::sslLibraryVersionString(),
           QSslSocket::supportsSsl());

    tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);

    auto finish = [&]() {
        if (p.isSet(shotOpt)) {
            const QString f = p.value(shotOpt);
            const bool ok = win.grab().save(f);
            report(tree, "Screenshot", f, ok);
            QTimer::singleShot(0, &app, [&] { app.exit(g_failures ? 1 : 0); });
        }
    };

    QNetworkAccessManager nam;
    if (!p.isSet(noNetOpt)) {
        QNetworkRequest req(QUrl(p.value(urlOpt)));
        req.setTransferTimeout(20000);
        QNetworkReply *reply = nam.get(req);
        QObject::connect(reply, &QNetworkReply::finished, &app, [&, reply] {
            const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray body = reply->readAll();
            const bool ok = reply->error() == QNetworkReply::NoError && code == 200;
            report(tree, "HTTPS GET", QString("%1 -> HTTP %2, %3 bytes %4")
                   .arg(reply->url().toString()).arg(code).arg(body.size())
                   .arg(ok ? QString() : reply->errorString()), ok);
            reply->deleteLater();
            finish();
        });
    }

    win.show();
    if (p.isSet(noNetOpt))
        QTimer::singleShot(200, &app, finish);
    if (p.isSet(exitOpt))
        QTimer::singleShot(int(p.value(exitOpt).toDouble() * 1000), &app, [&] { app.exit(g_failures ? 1 : 0); });

    const int rc = app.exec();
    std::printf("RESULT: %s (%d failures)\n", g_failures ? "FAIL" : "OK", g_failures);
    return rc;
}

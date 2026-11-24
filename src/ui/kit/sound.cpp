// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: [ui-h] alert sounds, see sound.h.
#include "ui/kit/sound.h"

#include <QtCore/QDataStream>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QStandardPaths>
#include <QtCore/QtMath>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QApplication>

#include "mx/services.h"
#include "mx/settings_keys.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <mmsystem.h>
#endif

namespace mx::ui::sound {

namespace {

Hook &hook()
{
    static Hook h;
    return h;
}

QString &last()
{
    static QString l;
    return l;
}

void record(const QString &what)
{
    const QString path = qEnvironmentVariable("MX_SOUND_LOG");
    if (path.isEmpty())
        return;
    QFile f(path);
    if (f.open(QIODevice::Append | QIODevice::Text))
        f.write(what.toUtf8() + '\n');
}

#ifndef Q_OS_WIN
// the first installed desktop sound player
QString findPlayer()
{
    // [mac-1] afplay: the macOS command-line player (always there)
    for (const char *p : {"afplay", "canberra-gtk-play", "paplay", "pw-play", "aplay"}) {
        const QString exe = QStandardPaths::findExecutable(QString::fromLatin1(p));
        if (!exe.isEmpty())
            return exe;
    }
    return QString();
}

QStringList playerArgs(const QString &exe, const QString &file)
{
    const QString n = QFileInfo(exe).fileName();
    if (n == QLatin1String("canberra-gtk-play"))
        return {QStringLiteral("-f"), file};
    if (n == QLatin1String("aplay"))
        return {QStringLiteral("-q"), file};
    return {file};
}

// the chime as a file the player can open (written once per run into the cache folder)
QString builtinFile()
{
    static QString path;
    if (!path.isEmpty() && QFileInfo::exists(path))
        return path;
    QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (dir.isEmpty())
        dir = QDir::tempPath();
    QDir().mkpath(dir);
    path = dir + QStringLiteral("/octomx-alert.wav");
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(builtinWav()) < 0)
        path.clear();
    return path;
}
#endif

} // namespace

bool suppressed()
{
    if (qEnvironmentVariableIsSet("MX_SOUND_OFF") || qEnvironmentVariableIsSet("MX_DEMO_FROZEN"))
        return true;
    const QString p = QGuiApplication::platformName();
    return p == QLatin1String("offscreen") || p == QLatin1String("minimal");
}

void setHook(const Hook &h) { hook() = h; }

QString lastPlayed() { return last(); }

QByteArray builtinWav()
{
    // an original two-note chime (E6 then A6 with a soft attack and decay), 22.05 kHz, 16-bit mono
    static const QByteArray wav = [] {
        const int rate = 22050;
        const double notes[2] = {1318.5, 1760.0};
        const int noteLen = rate * 9 / 100;   // 90 ms per note
        QByteArray pcm;
        QDataStream ds(&pcm, QIODevice::WriteOnly);
        ds.setByteOrder(QDataStream::LittleEndian);
        for (int n = 0; n < 2; ++n) {
            for (int i = 0; i < noteLen; ++i) {
                const double t = double(i) / rate;
                const double attack = qMin(1.0, i / (rate * 0.005));
                const double decay = qExp(-6.0 * double(i) / noteLen);
                const double v = qSin(2 * 3.14159265358979 * notes[n] * t) * attack * decay * 0.45;
                ds << qint16(qRound(v * 32767));
            }
        }
        QByteArray out;
        QDataStream w(&out, QIODevice::WriteOnly);
        w.setByteOrder(QDataStream::LittleEndian);
        w.writeRawData("RIFF", 4);
        w << quint32(36 + pcm.size());
        w.writeRawData("WAVEfmt ", 8);
        w << quint32(16) << quint16(1) << quint16(1) << quint32(rate) << quint32(rate * 2) << quint16(2) << quint16(16);
        w.writeRawData("data", 4);
        w << quint32(pcm.size());
        w.writeRawData(pcm.constData(), int(pcm.size()));
        return out;
    }();
    return wav;
}

QString player()
{
#ifdef Q_OS_WIN
    return QStringLiteral("PlaySound");
#else
    static const QString p = findPlayer();
    return p;
#endif
}

bool canPlayFiles() { return !player().isEmpty(); }

QString choiceFor(const Settings *s, Event e)
{
    if (!s || !s->b(keys::misc::Sounds))
        return QString();
    auto messageAlert = [s]() -> QString {
        const QString m = s->s(keys::messages::AlertMode);
        if (m == QLatin1String("beep"))
            return QStringLiteral("beep");
        if (m == QLatin1String("sound"))
            return QStringLiteral("sound");
        if (m == QLatin1String("wav")) {
            const QString wav = s->s(keys::messages::AlertWav);
            return !wav.isEmpty() && QFileInfo(wav).isFile() ? wav : QStringLiteral("sound");
        }
        return QString();
    };
    switch (e) {
    case Event::Message:
        return s->b(keys::messages::SoundOnMessage) ? messageAlert() : QString();
    case Event::HotlistOnline:
        return s->b(keys::hotlist::SoundOnline) ? messageAlert() : QString();
    case Event::DownloadDone:
        return s->b(keys::misc::SoundDownloadComplete) ? QStringLiteral("sound") : QString();
    }
    return QString();
}

QString alert(const Settings *s, Event e)
{
    const QString what = choiceFor(s, e);
    if (!what.isEmpty())
        play(what);
    return what;
}

bool play(const QString &what)
{
    if (what.isEmpty())
        return false;
    last() = what;
    if (hook()) {
        hook()(what);
        return true;
    }
    if (qEnvironmentVariableIsSet("MX_SOUND_LOG")) {
        record(what);
        return true;
    }
    if (suppressed())
        return true;
#ifdef Q_OS_WIN
    if (what == QLatin1String("beep"))
        return MessageBeep(MB_OK) != 0;
    if (what == QLatin1String("sound")) {
        static const QByteArray wav = builtinWav();   // must stay alive while PlaySound plays it
        return PlaySoundW(reinterpret_cast<LPCWSTR>(wav.constData()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT) != 0;
    }
    const QString native = QDir::toNativeSeparators(what);
    return PlaySoundW(reinterpret_cast<LPCWSTR>(native.utf16()), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT) != 0;
#else
    const QString exe = player();
    if (what == QLatin1String("beep")) {
        if (QFileInfo(exe).fileName() == QLatin1String("canberra-gtk-play"))
            return QProcess::startDetached(exe, {QStringLiteral("-i"), QStringLiteral("bell")});
        QApplication::beep();
        return true;
    }
    const QString file = what == QLatin1String("sound") ? builtinFile() : what;
    if (exe.isEmpty() || file.isEmpty()) {
        QApplication::beep();
        return true;
    }
    return QProcess::startDetached(exe, playerArgs(exe, file));
#endif
}

} // namespace mx::ui::sound

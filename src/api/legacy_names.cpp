// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX [ren-1]: copies installed before 5.0 keep working where they are (see mx/legacy_names.h).
#include "mx/legacy_names.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>

#include "mx/services.h"
#include "mx/settings_keys.h"

namespace mx::legacy {

namespace {
QString clean(const QString &p)
{
    return p.isEmpty() ? QString() : QDir::cleanPath(QDir(p).absolutePath());
}
bool hasSettings(const QString &dir)
{
    return !dir.isEmpty() && QFileInfo(QDir(dir).filePath(QStringLiteral("settings.json"))).isFile();
}
} // namespace

QString legacyProfileDir(const QString &newDefault)
{
    if (newDefault.isEmpty())
        return QString();
    const QFileInfo fi(clean(newDefault));
    return QDir::cleanPath(fi.absolutePath() + QLatin1Char('/') + QLatin1String(kLegacyAppName));
}

QString chooseProfileDir(const QString &newDefault)
{
    const QString fresh = clean(newDefault);
    const QString old = legacyProfileDir(fresh);
    if (!hasSettings(fresh) && hasSettings(old) && old != fresh)
        return old;
    return fresh;
}

bool isStandardProfileDir(const QString &dir, const QString &newDefault)
{
    const QString d = clean(dir);
    if (d.isEmpty() || newDefault.isEmpty())
        return false;
    return d == clean(newDefault) || d == legacyProfileDir(newDefault);
}

QString databaseFile(const QString &profileDir)
{
    const QDir p(profileDir);
    const QString fresh = p.filePath(QLatin1String(kDatabase));
    const QString old = p.filePath(QLatin1String(kLegacyDatabase));
    if (!QFileInfo::exists(fresh) && QFileInfo(old).isFile())
        return old;
    return fresh;
}

QString defaultIncomingDir(const QString &downloadsDir)
{
    const QDir d(downloadsDir);
    const QString fresh = QDir::cleanPath(d.filePath(QLatin1String(kDownloadFolder)));
    const QString old = QDir::cleanPath(d.filePath(QLatin1String(kLegacyDownloadFolder)));
    if (!QFileInfo::exists(fresh) && QFileInfo(old).isDir())
        return old;
    return fresh;
}

QStringList migrateSettings(Settings *s)
{
    QStringList changed;
    if (!s)
        return changed;
    using namespace mx::keys;
    // the legacy default update check address (GitHub redirects it, but a default stays a default):
    // the current default follows. An address the user set stays.
    if (s->s(updates::Url) == QLatin1String(kLegacyUpdateUrl)) {
        s->setValue(updates::Url, QVariant());
        changed << QString(updates::Url);
    }
    // the legacy default user agent (prefix and a version number), stored by an older settings page
    const QString ua = s->s(http::UserAgent);
    if (ua.startsWith(QLatin1String(kLegacyUserAgentPrefix))) {
        bool versionOnly = true;
        for (QChar c : ua.mid(int(qstrlen(kLegacyUserAgentPrefix))))
            versionOnly = versionOnly && (c.isDigit() || c == QLatin1Char('.'));
        if (versionOnly) {
            s->setValue(http::UserAgent, QVariant());
            changed << QString(http::UserAgent);
        }
    }
    // the legacy name of our own network in the search network list
    QStringList nets = s->sl(search::Networks);
    bool netsChanged = false;
    for (QString &n : nets)
        if (n.trimmed().compare(QLatin1String(kLegacyNetName), Qt::CaseInsensitive) == 0) {
            n = netName(Net::Overlay);
            netsChanged = true;
        }
    if (netsChanged) {
        s->setValue(search::Networks, nets);
        changed << QString(search::Networks);
    }
    // a font picked as the bundled font under its legacy family name: the settings name the bundled
    // font as "Tahoma" (the kit's Tahoma)
    for (const K k : {ui::FontNormal, ui::FontDynamic, ui::FontLarge}) {
        const QString key(k);
        const QString v = s->s(key);
        if (v.startsWith(QLatin1String(kLegacyFontFamily), Qt::CaseInsensitive)) {
            s->setValue(key, QStringLiteral("Tahoma") + v.mid(int(qstrlen(kLegacyFontFamily))));
            changed << key;
        }
    }
    return changed;
}

} // namespace mx::legacy

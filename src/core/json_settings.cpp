// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/json_settings.h"
#include "core/log.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>

#include "mx/durable_file.h"

namespace mx {

JsonSettings::JsonSettings(const QString &filePath, QObject *parent)
    : SimpleSettings(parent)
    , m_path(filePath)
{
    m_timer.setSingleShot(true);
    m_timer.setInterval(500);
    connect(&m_timer, &QTimer::timeout, this, &JsonSettings::sync);
}

JsonSettings::~JsonSettings()
{
    sync();
}

bool JsonSettings::load(QString *error)
{
    QFile f(m_path);
    if (!f.exists()) {
        loadValues({});
        return true;
    }
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = f.errorString();
        qCWarning(lcCore) << "cannot read" << m_path << f.errorString();
        return false;
    }
    const QByteArray raw = f.readAll();
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &pe);
    f.close();
    const QString backup = m_path + QStringLiteral(".bak");
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        // [set-1] a damaged file keeps what can still be read: every entry that parses on its own,
        // then what the last good copy (settings.json.bak) has for the others. It used to start
        // from the defaults (every setting lost for one broken character).
        QVariantMap values = salvageEntries(raw);
        const int salvaged = int(values.size());
        QFile b(backup);
        if (b.open(QIODevice::ReadOnly)) {
            const QJsonDocument good = QJsonDocument::fromJson(b.readAll());
            const QVariantMap old = good.isObject() ? good.object().toVariantMap() : QVariantMap();
            for (auto it = old.constBegin(); it != old.constEnd(); ++it)
                if (!values.contains(it.key())) values.insert(it.key(), it.value());
        }
        const QString bad = m_path + QStringLiteral(".bad");
        QFile::remove(bad);
        QFile::rename(m_path, bad);
        if (error) *error = pe.errorString();
        qCWarning(lcCore) << "settings file damaged, moved to" << bad << pe.errorString() << ":" << salvaged
                          << "entries read from it," << values.size() - salvaged << "from the backup";
        loadValues(values);
        m_dirty = true;   // write the recovered settings back
        m_timer.start();
        return false;
    }
    loadValues(doc.object().toVariantMap());
    m_dirty = false;
    // [set-1] the last file that loaded fine, for the case above
    QFile::remove(backup);
    QFile::copy(m_path, backup);
    return true;
}

QVariantMap JsonSettings::salvageEntries(const QByteArray &raw)
{
    // the file is written with one top-level entry per line ("    \"key\": value,"), a list or an
    // object over several lines; each entry is parsed on its own, a broken one is dropped
    QVariantMap out;
    const QList<QByteArray> lines = raw.split('\n');
    QByteArray entry;
    auto flush = [&] {
        QByteArray e = entry.trimmed();
        entry.clear();
        while (e.endsWith(',')) e.chop(1);
        if (e.isEmpty() || !e.startsWith('"')) return;
        const QJsonDocument d = QJsonDocument::fromJson("{" + e + "}");
        if (!d.isObject() || d.object().size() != 1) return;
        const QVariantMap m = d.object().toVariantMap();
        out.insert(m.firstKey(), m.first());
    };
    for (const QByteArray &line : lines) {
        // a new entry starts at the indentation of the top level
        if (line.startsWith("    \"")) flush();
        if (line.startsWith('{') || line.startsWith('}')) {   // the top-level braces
            flush();
            continue;
        }
        entry += line + '\n';
    }
    flush();
    return out;
}

void JsonSettings::scheduleSave()
{
    m_dirty = true;
    m_timer.start();
}

void JsonSettings::sync()
{
    m_timer.stop();
    if (!m_dirty) return;
    QJsonObject obj;
    const QVariantMap values = storedValues();
    for (auto it = values.constBegin(); it != values.constEnd(); ++it)
        obj.insert(it.key(), QJsonValue::fromVariant(settingToJson(findSetting(it.key()), it.value())));
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    QSaveFile f(m_path);
    if (!f.open(QIODevice::WriteOnly)) {
        qCWarning(lcCore) << "cannot write" << m_path << f.errorString();
        return;
    }
    f.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    durable::syncFile(f);   // [safe-1] on the disk before it replaces the old file (a power cut)
    if (!f.commit()) {
        qCWarning(lcCore) << "cannot commit" << m_path << f.errorString();
        return;
    }
    m_dirty = false;
}

} // namespace mx

// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/json_settings.h"
#include "core/log.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>

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
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    f.close();
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        const QString bad = m_path + QStringLiteral(".bad");
        QFile::remove(bad);
        QFile::rename(m_path, bad);
        if (error) *error = pe.errorString();
        qCWarning(lcCore) << "settings file corrupt, moved to" << bad << pe.errorString();
        loadValues({});
        return false;
    }
    loadValues(doc.object().toVariantMap());
    m_dirty = false;
    return true;
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
    if (!f.commit()) {
        qCWarning(lcCore) << "cannot commit" << m_path << f.errorString();
        return;
    }
    m_dirty = false;
}

} // namespace mx

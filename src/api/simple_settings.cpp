// SPDX-License-Identifier: GPL-3.0-or-later
#include "mx/simple_settings.h"

namespace mx {

SimpleSettings::SimpleSettings(QObject *parent)
    : Settings(parent)
{
}

QVariant SimpleSettings::defaultValue(const QString &key) const
{
    {
        QReadLocker l(&m_lock);
        const auto it = m_runtimeDefaults.constFind(key);
        if (it != m_runtimeDefaults.constEnd()) return it.value();
    }
    return settingDefault(key);
}

QVariant SimpleSettings::value(const QString &key) const
{
    {
        QReadLocker l(&m_lock);
        const auto it = m_values.constFind(key);
        if (it != m_values.constEnd()) {
            // Empty path = "use the computed default" (incoming / incomplete dirs).
            const SettingDef *d = findSetting(key);
            if (!(d && d->type == SettingType::Path && it.value().toString().isEmpty()))
                return it.value();
        }
    }
    return defaultValue(key);
}

void SimpleSettings::setValue(const QString &key, const QVariant &v)
{
    if (key.isEmpty()) return;
    QVariant nv = v;
    if (const SettingDef *d = findSetting(key)) {
        bool ok = false;
        nv = coerceSetting(*d, v, &ok);
        if (!ok && v.isValid() && !v.isNull())
            return; // reject garbage, keep the current value
        if (!v.isValid() || v.isNull())
            nv = QVariant(); // setValue(key, QVariant()) == reset to default
    }
    const QVariant before = value(key);
    {
        QWriteLocker l(&m_lock);
        if (nv.isValid()) m_values.insert(key, nv);
        else m_values.remove(key);
    }
    const QVariant after = value(key);
    scheduleSave();
    if (before != after)
        emit changed(key, after);
}

QStringList SimpleSettings::keys(const QString &prefix) const
{
    QStringList out;
    for (const SettingDef &d : settingsSchema()) {
        if (d.key.endsWith(QLatin1String("/*"))) continue;
        if (prefix.isEmpty() || d.key.startsWith(prefix)) out << d.key;
    }
    QReadLocker l(&m_lock);
    for (auto it = m_values.constBegin(); it != m_values.constEnd(); ++it) {
        if ((prefix.isEmpty() || it.key().startsWith(prefix)) && !out.contains(it.key()))
            out << it.key();
    }
    return out;
}

void SimpleSettings::resetToDefaults(const QString &prefix)
{
    QStringList touched;
    QHash<QString, QVariant> before;
    {
        QReadLocker l(&m_lock);
        for (auto it = m_values.constBegin(); it != m_values.constEnd(); ++it)
            if (prefix.isEmpty() || it.key().startsWith(prefix)) touched << it.key();
    }
    for (const QString &k : touched) before.insert(k, value(k));
    {
        QWriteLocker l(&m_lock);
        for (const QString &k : touched) {
            // never drop unknown keys owned by someone else on a full reset of a section we don't know
            if (findSetting(k)) m_values.remove(k);
        }
    }
    scheduleSave();
    for (const QString &k : touched) {
        const QVariant now = value(k);
        if (now != before.value(k)) emit changed(k, now);
    }
}

void SimpleSettings::setRuntimeDefault(const QString &key, const QVariant &v)
{
    QWriteLocker l(&m_lock);
    m_runtimeDefaults.insert(key, v);
}

bool SimpleSettings::contains(const QString &key) const
{
    QReadLocker l(&m_lock);
    return m_values.contains(key);
}

QVariantMap SimpleSettings::storedValues() const
{
    QReadLocker l(&m_lock);
    return m_values;
}

void SimpleSettings::loadValues(const QVariantMap &values)
{
    QVariantMap clean;
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
        const SettingDef *d = findSetting(it.key());
        clean.insert(it.key(), d ? settingFromJson(d, it.value()) : it.value());
    }
    QWriteLocker l(&m_lock);
    m_values = clean;
}

} // namespace mx

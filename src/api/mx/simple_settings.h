// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX: in-memory mx::Settings backed by the schema (mx/settings_keys.h). Owner: core task.
// Used directly by the mock core and as the base of the file-backed settings in mxcore.
// * value(): stored value, else runtime default (setRuntimeDefault), else schema default.
// * setValue(): coerces + clamps via the schema (unknown keys are stored as given), emits
//   changed(key, coercedValue) only when the effective value changes, then calls scheduleSave().
// * Unknown keys found in loaded data are preserved untouched.
// Thread-safety: reads and writes are guarded by a lock, but signals are emitted on the calling
// thread: call setValue() on the main thread (as mx/services.h requires).
#pragma once

#include <QtCore/QHash>
#include <QtCore/QReadWriteLock>
#include <QtCore/QVariantMap>

#include "mx/services.h"
#include "mx/settings_keys.h"

namespace mx {

class SimpleSettings : public Settings {
    Q_OBJECT
public:
    explicit SimpleSettings(QObject *parent = nullptr);

    QVariant value(const QString &key) const override;
    void setValue(const QString &key, const QVariant &v) override;
    QVariant defaultValue(const QString &key) const override;
    QStringList keys(const QString &prefix = QString()) const override;  // schema keys + stored keys
    void resetToDefaults(const QString &prefix = QString()) override;
    void sync() override {}

    // A default computed at runtime (e.g. the incoming directory); overrides the schema default.
    void setRuntimeDefault(const QString &key, const QVariant &v);
    // True if the key has an explicitly stored value.
    bool contains(const QString &key) const;
    // Everything explicitly stored (raw, coerced values; Bytes as QByteArray).
    QVariantMap storedValues() const;
    // Replace the stored values (coercing known keys, keeping unknown ones). No signals.
    void loadValues(const QVariantMap &values);

protected:
    virtual void scheduleSave() {}

private:
    mutable QReadWriteLock m_lock;
    QVariantMap m_values;
    QHash<QString, QVariant> m_runtimeDefaults;
};

} // namespace mx

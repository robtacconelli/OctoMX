// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX core: mx::Settings persisted as <profile>/settings.json.
// JSON object of "key": value (keys are the slash paths of mx/settings_keys.h). Only values that
// were explicitly set are written; unknown keys found in the file are preserved. Writes are atomic
// (QSaveFile) and debounced (500 ms after the last change); sync() writes immediately.
// A corrupt file is renamed to settings.json.bad and defaults are used.
#pragma once

#include <QtCore/QTimer>

#include "mx/simple_settings.h"

namespace mx {

class JsonSettings : public SimpleSettings {
    Q_OBJECT
public:
    explicit JsonSettings(const QString &filePath, QObject *parent = nullptr);
    ~JsonSettings() override;

    bool load(QString *error = nullptr);   // read the file (missing file = defaults, returns true)
    void sync() override;                  // write now if dirty
    QString filePath() const { return m_path; }
    void setSaveDelay(int ms) { m_timer.setInterval(ms); }

protected:
    void scheduleSave() override;

private:
    QString m_path;
    QTimer m_timer;
    bool m_dirty = false;
};

} // namespace mx

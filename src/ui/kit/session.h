// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: per-process ("this session only") switches that must never be persisted, and the
// one-time migration of settings keys the UI used to keep privately.
#pragma once

#include <QtCore/QObject>
#include <QtCore/QStringList>
#include <QtCore/QVariantMap>

namespace mx {
class Settings;
}

namespace mx::ui {

class Session : public QObject {
    Q_OBJECT
public:
    static Session *instance();

    // Settings ▸ File Transfers ▸ "Suppress dangerous filetype warning (this session only)".
    // The permanent switch is transfers/suppressDangerousWarning.
    bool suppressDangerousWarning() const { return m_suppressDanger; }
    void setSuppressDangerousWarning(bool on);

    // Search: "this hit is a suspected fake" warning was shown (once per session).
    bool suspectWarningShown() const { return m_suspectWarned; }
    void setSuspectWarningShown(bool on) { m_suspectWarned = on; }

    void reset();   // tests

signals:
    void changed();

private:
    using QObject::QObject;
    bool m_suppressDanger = false;
    bool m_suspectWarned = false;
};

// Moves the values of the retired private UI keys (ui/searchFileType, ui/searchAutoFindSources,
// ui/searchAutoEnterQueue, ui/suppressDangerWarning, ui/searchConnectionFilter) to their canonical
// keys (search/defaultType, search/autoFindSourcesMin, search/autoEnterQueue, dropped (session only),
// search/defaultConnection) and removes the old values. Idempotent; called at every start.
void migrateLegacySettings(mx::Settings *s);

// [ui-f] The file-type settings hold codes (mx::FileTypeFilter): search/defaultType and the button
// presets; texts of OctoMX <= 4.0 are converted, the old preset defaults become audio / video /
// pictures, an untouched 2001 "Available Filetypes" list and WinMX 3.53 Shared File Types ids are
// replaced by the table's groups. Idempotent; part of migrateLegacySettings().
void migrateFileTypeSettings(mx::Settings *s);
// Search page file-type menu: code ("audio:min:192", "video") -> button text (kit/file_types.h), and
// any stored text or code (also the old "MP3 Audio - Any Bitrate", "Video (.avi .asf .mpg .mpeg)")
// -> code (empty if it means nothing).
QString fileTypeTextForCode(const QString &code);
QString fileTypeCodeForText(const QString &text);
// Connection menu: "DSL or faster" <-> rank 8 ("Any Connection" = 0).
QString connectionTextForRank(int rank);
int connectionRankForText(const QString &text);

// [ui-e] transfer slots / queues for a 2026 connection, derived from the connection type (identity/
// connection: DSL ... "10 Gbit+"): upload slots, uploads per user, downloads, sources per download,
// queue length. The schema defaults (3 upload slots, 200 queue places) date from dial-up / DSL.
QVariantMap connectionDefaults(const QString &connectionType);
// Applies connectionDefaults() for the current connection type to every key that still has its
// schema default or the value derived for the previous type (a value the user set is kept), and
// remembers the type in ui/connectionDefaults. Called at start and whenever the type changes.
// Returns the keys that were changed.
QStringList applyConnectionDefaults(mx::Settings *s);

} // namespace mx::ui

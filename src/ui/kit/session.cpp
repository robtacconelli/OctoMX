// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/kit/session.h"

#include <QtCore/QStringList>

#include "mx/services.h"
#include "mx/settings_keys.h"
#include "ui/kit/file_types.h"

namespace mx::ui {

Session *Session::instance()
{
    static Session *s = new Session(nullptr);
    return s;
}

void Session::setSuppressDangerousWarning(bool on)
{
    if (m_suppressDanger == on)
        return;
    m_suppressDanger = on;
    emit changed();
}

void Session::reset()
{
    m_suspectWarned = false;
    setSuppressDangerousWarning(false);
}

// ------------------------------------------------------------------------------- file types

namespace {
// [qa-1] owner request: "<type> or faster" for the modern connection types, fastest first; the value is
// mx::connectionSpeedRank() (search connection filter, search/defaultConnection)
const struct { const char *t; int r; } kConnections[] = {
    {QT_TR_NOOP("Any Connection"), 0},
    {QT_TR_NOOP("10 Gbit or faster"), 12}, {QT_TR_NOOP("5 Gbit or faster"), 11}, {QT_TR_NOOP("2.5 Gbit or faster"), 10},
    {QT_TR_NOOP("FTTH or faster"), 9},     {QT_TR_NOOP("FTTB or faster"), 8},    {QT_TR_NOOP("5G or faster"), 7},
    {QT_TR_NOOP("Cable or faster"), 6},    {QT_TR_NOOP("FTTC or faster"), 5},    {QT_TR_NOOP("Satellite or faster"), 4},
    {QT_TR_NOOP("VDSL or faster"), 3},     {QT_TR_NOOP("4G or faster"), 2},      {QT_TR_NOOP("DSL or faster"), 1},
};
} // namespace

QString fileTypeTextForCode(const QString &code)
{
    return filetypes::buttonText(code);   // [ui-f] generated from the type table
}

QString fileTypeCodeForText(const QString &text)
{
    return fileTypeCodeFromLegacy(text);   // [ui-f] codes, and every text / code of OctoMX <= 4.0
}

QString connectionTextForRank(int rank)
{
    for (const auto &c : kConnections)
        if (c.r == rank)
            return QObject::tr(c.t);
    return QObject::tr(kConnections[0].t);
}

int connectionRankForText(const QString &text)
{
    for (const auto &c : kConnections)
        if (text.compare(QObject::tr(c.t), Qt::CaseInsensitive) == 0 || text.compare(QLatin1String(c.t), Qt::CaseInsensitive) == 0)
            return c.r;
    // the original WinMX filter texts ("T3 or faster", "Cable or faster", "56K or faster" ...)
    const QString t = text.trimmed();
    if (t.endsWith(QLatin1String(" or faster"), Qt::CaseInsensitive))
        return connectionSpeedRank(t.left(t.size() - 10));
    bool ok = false;
    const int n = t.toInt(&ok);   // the old ui/searchConnectionFilter stored the old rank (0..10)
    if (!ok || n <= 0)
        return 0;
    return n >= 10 ? connectionSpeedRank(QStringLiteral("T3")) : n == 7 ? connectionSpeedRank(QStringLiteral("Cable"))
                                                                  : connectionSpeedRank(QStringLiteral("DSL"));
}

// ------------------------------------------------------------------------------- migration

void migrateFileTypeSettings(Settings *s)
{
    if (!s)
        return;
    // search/defaultType: the menu text of <= 4.0 ("MP3 Audio - At least 128 kbps") -> its code
    const QString def = s->s(keys::search::DefaultType);
    if (!FileTypeFilter::fromCode(def).isValid()) {
        const QString code = fileTypeCodeFromLegacy(def);
        s->setValue(keys::search::DefaultType, code.isEmpty() ? QStringLiteral("any") : code);
    } else if (FileTypeFilter::fromCode(def).code() != def) {
        s->setValue(keys::search::DefaultType, FileTypeFilter::fromCode(def).code());
    }
    // the quick-button presets: the old defaults were MP3 / 2001 video / picture lists; the buttons
    // now stand for the general types (Audio = all audio); a preset the user chose keeps its meaning
    const struct { const char *key; const char *oldDefault; const char *code; } presets[] = {
        {"search/typeButtonAudio", "MP3 Audio - Any Bitrate", "audio"},
        {"search/typeButtonVideo", "Video (.avi .asf .mpg .mpeg)", "video"},
        {"search/typeButtonPictures", "Pictures (.jpg .png .jpeg .gif)", "pictures"},
    };
    for (const auto &p : presets) {
        const QString k = QString::fromLatin1(p.key);
        const QString v = s->s(k);
        if (FileTypeFilter::fromCode(v).isValid()) {
            if (FileTypeFilter::fromCode(v).code() != v)
                s->setValue(k, FileTypeFilter::fromCode(v).code());
            continue;
        }
        QString code = v.trimmed().compare(QLatin1String(p.oldDefault), Qt::CaseInsensitive) == 0 ? QString::fromLatin1(p.code)
                                                                                                  : fileTypeCodeFromLegacy(v);
        s->setValue(k, code.isEmpty() ? QString::fromLatin1(p.code) : code);
    }
    // custom extensions: one normalised list ("mkv srt")
    const QString custom = s->s(keys::search::CustomExtensions);
    const QString norm = parseExtensionList(custom).join(QLatin1Char(' '));
    if (custom != norm)
        s->setValue(keys::search::CustomExtensions, norm);
    // Settings > Library > Available Filetypes: the 2001 list (unchanged by the user) -> the table
    const QStringList oldTypes = {QStringLiteral("Audio (.mp3 .ogg)"), QStringLiteral("Video (.asf .avi .mpg .mpeg)"),
                                  QStringLiteral("Pictures (.png .jpg .jpeg .gif)"), QStringLiteral("Text (.txt .rtf .pdf)"),
                                  QStringLiteral("HTML (.htm .html)"), QStringLiteral("Archive (.zip .iso .rar .r##)"),
                                  QStringLiteral("Executable (.exe)"), QStringLiteral("Other Audio (.mpc .au .aif .wav .midi)"),
                                  QStringLiteral("Server Lists (.met .wsx)"), QStringLiteral("Torrents (.torrent)"),
                                  QStringLiteral("Colour Schemes (.clr)")};
    if (s->sl(keys::library::FileTypes) == oldTypes)
        s->setValue(keys::library::FileTypes, s->defaultValue(keys::library::FileTypes));
    // Shared File Types: the WinMX 3.53 ids -> the groups of the table (the same files stay shared)
    const QStringList shared = s->sl(keys::library::SharedTypes);
    if (isLegacySharedTypeList(shared))
        s->setValue(keys::library::SharedTypes, normalizeSharedTypeIds(shared, s->b(keys::library::ShareUnlistedTypes)));
}


void migrateLegacySettings(Settings *s)
{
    if (!s)
        return;
    // A retired key is no longer in the schema: value() is valid only when the profile stored it.
    auto take = [s](const char *oldKey) -> QVariant {
        const QString k = QString::fromLatin1(oldKey);
        if (findSetting(k))
            return {};   // still in the schema (older api): leave it alone
        const QVariant v = s->value(k);
        if (v.isValid())
            s->setValue(k, QVariant());   // removes an unknown key
        return v;
    };
    const QString marker = QStringLiteral("ui/settingsMigrated");
    const bool firstRun = !s->value(marker).isValid();

    const QVariant type = take("ui/searchFileType");
    if (type.isValid()) {
        const QString code = fileTypeCodeFromLegacy(type.toString());
        if (!code.isEmpty())
            s->setValue(keys::search::DefaultType, code);
    }
    migrateFileTypeSettings(s);
    const QVariant afs = take("ui/searchAutoFindSources");
    if (afs.isValid())
        s->setValue(keys::search::AutoFindSourcesMin, afs.toInt());
    const QVariant aeq = take("ui/searchAutoEnterQueue");
    if (aeq.isValid())
        s->setValue(keys::search::AutoEnterQueue, aeq.toInt());
    take("ui/suppressDangerWarning");   // was "this session only" anyway: never carried over
    // the connection filter used to be stored as a rank in ui/searchConnectionFilter (schema key)
    const QString conn = s->s(keys::ui::SearchConnectionFilter);
    if (!conn.isEmpty()) {
        s->setValue(keys::search::DefaultConnection, connectionTextForRank(connectionRankForText(conn)));
        s->setValue(keys::ui::SearchConnectionFilter, QVariant());
    }
    // [qa-1] original WinMX filter texts ("T3 or faster", "56K or faster") -> the modern list
    const QString dc = s->s(keys::search::DefaultConnection);
    const QString dcNew = connectionTextForRank(connectionRankForText(dc));
    if (!dc.isEmpty() && dc.compare(dcNew, Qt::CaseInsensitive) != 0)
        s->setValue(keys::search::DefaultConnection, dcNew);
    // [5.2] the sources of a download are folded away unless the user asks for them: a stored
    // "expanded" from the time that was the default is dropped once (the box in Settings, File
    // Transfers brings it back)
    if (s->value(marker).toInt() < 2) {
        if (!firstRun)
            s->setValue(keys::transfers::NewDownloadsExpanded, QVariant());
        s->setValue(marker, 2);
    }
}

// ------------------------------------------------------------------------------------ [ui-e]

QVariantMap connectionDefaults(const QString &type)
{
    // upload slots by typical upstream: DSL ~1 Mbit, VDSL ~10-40, cable / fibre 50-1000, multi-gigabit
    struct Row { const char *type; int up, perUser, down, sources, queue; };
    static const Row rows[] = {
        {"Unknown", 3, 4, 40, 200, 200},   {"DSL", 3, 4, 40, 200, 200},       {"4G", 4, 4, 50, 250, 250},
        {"VDSL", 6, 4, 60, 300, 300},      {"Satellite", 4, 4, 50, 250, 250}, {"FTTC", 8, 4, 80, 400, 400},
        {"Cable", 10, 4, 100, 500, 500},   {"5G", 8, 4, 80, 400, 400},        {"FTTB", 12, 5, 120, 600, 600},
        {"FTTH", 16, 6, 150, 800, 800},    {"2.5 Gbit", 24, 8, 200, 1000, 1000},
        {"5 Gbit", 32, 8, 250, 1500, 1500}, {"10 Gbit+", 48, 10, 300, 2000, 2000},
    };
    const Row *r = &rows[0];
    for (const Row &x : rows)
        if (type == QLatin1String(x.type))
            r = &x;
    QVariantMap m;
    m.insert(QString(keys::transfers::MaxUploads), r->up);
    m.insert(QString(keys::transfers::MaxUploadsPerUser), r->perUser);
    m.insert(QString(keys::transfers::MaxDownloads), r->down);
    m.insert(QString(keys::transfers::MaxSourcesPerDownload), r->sources);
    m.insert(QString(keys::queue::MaxLength), r->queue);
    return m;
}

QStringList applyConnectionDefaults(Settings *s)
{
    QStringList changed;
    if (!s)
        return changed;
    const QString type = s->value(keys::identity::Connection).toString();
    const QString before = s->value(keys::ui::ConnectionDefaults).toString();
    if (type == before)
        return changed;
    const QVariantMap now = connectionDefaults(type);
    const QVariantMap old = before.isEmpty() ? QVariantMap() : connectionDefaults(before);
    for (auto it = now.constBegin(); it != now.constEnd(); ++it) {
        const qint64 cur = s->value(it.key()).toLongLong();
        const bool untouched = cur == s->defaultValue(it.key()).toLongLong() ||
                               (old.contains(it.key()) && cur == old.value(it.key()).toLongLong());
        if (untouched && cur != it.value().toLongLong()) {
            s->setValue(it.key(), it.value());
            changed << it.key();
        }
    }
    s->setValue(keys::ui::ConnectionDefaults, type);
    return changed;
}

} // namespace mx::ui

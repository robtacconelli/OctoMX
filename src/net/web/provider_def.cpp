// SPDX-License-Identifier: GPL-3.0-or-later
#include "provider_def.h"

#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QSet>

namespace mx::web {

static const char *const kTypeKeys[] = {"any", "audio", "video", "image", "document", "software", "archive", "other"};

QString mediaTypeKey(MediaType t)
{
    const int i = int(t);
    if (i < 0 || i > int(MediaType::Other)) return QStringLiteral("any");
    return QLatin1String(kTypeKeys[i]);
}

MediaType mediaTypeFromKey(const QString &key)
{
    const QString k = key.trimmed().toLower();
    for (int i = 0; i <= int(MediaType::Other); ++i)
        if (k == QLatin1String(kTypeKeys[i])) return MediaType(i);
    if (k == QLatin1String("movie") || k == QLatin1String("movies") || k == QLatin1String("tv")) return MediaType::Video;
    if (k == QLatin1String("music")) return MediaType::Audio;
    if (k == QLatin1String("book") || k == QLatin1String("books") || k == QLatin1String("text")) return MediaType::Document;
    if (k == QLatin1String("app") || k == QLatin1String("apps") || k == QLatin1String("games")) return MediaType::Software;
    return MediaType::Any;
}

static bool compileRegex(const QString &pattern, QRegularExpression *out, QString *error, const QString &what,
                         bool dotAll = false)
{
    QRegularExpression::PatternOptions opts = QRegularExpression::CaseInsensitiveOption;
    if (dotAll) opts |= QRegularExpression::DotMatchesEverythingOption;
    QRegularExpression re(pattern, opts);
    if (!re.isValid()) {
        if (error) *error = QStringLiteral("bad regex in %1: %2").arg(what, re.errorString());
        return false;
    }
    *out = re;
    return true;
}

static bool parseField(const QJsonValue &v, FieldSpec *f, QString *error, const QString &what)
{
    if (v.isString()) {
        f->path = v.toString();
        return true;
    }
    if (!v.isObject()) {
        if (error) *error = QStringLiteral("field %1 must be a string or an object").arg(what);
        return false;
    }
    const QJsonObject o = v.toObject();
    f->path = o.value(QLatin1String("path")).toString();
    f->tmpl = o.value(QLatin1String("template")).toString();
    const QJsonValue c = o.value(QLatin1String("value"));
    if (c.isString()) f->constant = c.toString();
    else if (c.isDouble()) f->constant = QString::number(c.toDouble(), 'g', 17);
    else if (c.isBool()) f->constant = c.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    f->transform = o.value(QLatin1String("transform")).toString().toLower();
    f->format = o.value(QLatin1String("format")).toString();
    const QString rx = o.value(QLatin1String("regex")).toString();
    if (!rx.isEmpty() && !compileRegex(rx, &f->regex, error, what, true)) return false;
    if (!f->valid()) {
        if (error) *error = QStringLiteral("field %1 has no path/template/value").arg(what);
        return false;
    }
    return true;
}

bool parseProviderDef(const QJsonObject &o, ProviderDef *out, QString *error)
{
    ProviderDef d;
    d.id = o.value(QLatin1String("id")).toString().trimmed();
    if (d.id.isEmpty()) { if (error) *error = QStringLiteral("provider without id"); return false; }
    d.name = o.value(QLatin1String("name")).toString(d.id);
    d.homepage = o.value(QLatin1String("homepage")).toString();
    d.notes = o.value(QLatin1String("notes")).toString();
    d.enabled = o.value(QLatin1String("enabled")).toBool(true);

    const QJsonObject rq = o.value(QLatin1String("request")).toObject();
    d.url = rq.value(QLatin1String("url")).toString();
    if (!d.url.startsWith(QLatin1String("http://")) && !d.url.startsWith(QLatin1String("https://"))) {
        if (error) *error = QStringLiteral("%1: request.url must be http(s)").arg(d.id);
        return false;
    }
    d.method = rq.value(QLatin1String("method")).toString(QStringLiteral("GET")).toUpper();
    if (d.method != QLatin1String("GET") && d.method != QLatin1String("POST")) {
        if (error) *error = QStringLiteral("%1: method must be GET or POST").arg(d.id);
        return false;
    }
    d.body = rq.value(QLatin1String("body")).toString();
    d.contentType = rq.value(QLatin1String("contentType")).toString();
    const QJsonObject hdr = rq.value(QLatin1String("headers")).toObject();
    for (auto it = hdr.begin(); it != hdr.end(); ++it) d.headers.insert(it.key(), it.value().toString());
    d.spaceAs = rq.value(QLatin1String("spaceAs")).toString(QStringLiteral("%20"));
    const QJsonObject cats = rq.value(QLatin1String("categories")).toObject();
    for (auto it = cats.begin(); it != cats.end(); ++it) {
        const QString k = it.key().toLower();
        if (k == QLatin1String("any")) d.hasAnyCategory = true;
        const MediaType t = mediaTypeFromKey(k);
        if (t == MediaType::Any && k != QLatin1String("any")) continue;
        d.categories.insert(int(t), it.value().isDouble() ? QString::number(it.value().toInteger()) : it.value().toString());
    }
    d.strictCategories = rq.value(QLatin1String("strictCategories")).toBool(false);
    d.probeQuery = rq.value(QLatin1String("probeQuery")).toString(d.probeQuery);

    const QJsonObject pg = o.value(QLatin1String("paging")).toObject();
    d.firstPage = pg.value(QLatin1String("first")).toInt(1);
    d.maxPages = qBound(1, pg.value(QLatin1String("maxPages")).toInt(1), 20);
    d.pageSize = qMax(0, pg.value(QLatin1String("pageSize")).toInt(0));

    const QJsonObject lim = o.value(QLatin1String("limits")).toObject();
    d.minIntervalMs = qBound(0, lim.value(QLatin1String("minIntervalMs")).toInt(1000), 60000);
    d.timeoutMs = qBound(1000, lim.value(QLatin1String("timeoutMs")).toInt(10000), 60000);
    d.maxBytes = qBound(1024, lim.value(QLatin1String("maxBytes")).toInt(4 * 1024 * 1024), 4 * 1024 * 1024);

    const QJsonObject rs = o.value(QLatin1String("response")).toObject();
    d.format = rs.value(QLatin1String("format")).toString(QStringLiteral("json")).toLower();
    if (d.format == QLatin1String("html")) d.format = QStringLiteral("regex");
    if (d.format != QLatin1String("json") && d.format != QLatin1String("rss") && d.format != QLatin1String("xml")
        && d.format != QLatin1String("regex")) {
        if (error) *error = QStringLiteral("%1: unknown response.format %2").arg(d.id, d.format);
        return false;
    }
    d.items = rs.value(QLatin1String("items")).toString();
    if (d.items.isEmpty()) {
        if (d.format == QLatin1String("rss") || d.format == QLatin1String("xml")) d.items = QStringLiteral("item");
        else if (d.format == QLatin1String("json")) d.items = QStringLiteral("[]");
        else { if (error) *error = QStringLiteral("%1: regex format needs response.items").arg(d.id); return false; }
    }
    if (d.format == QLatin1String("regex")) {
        QRegularExpression probe;
        if (!compileRegex(d.items, &probe, error, d.id + QStringLiteral(".items"), true)) return false;
    }
    d.decimalUnits = rs.value(QLatin1String("decimalUnits")).toBool(false);
    const QJsonObject fields = rs.value(QLatin1String("fields")).toObject();
    for (auto it = fields.begin(); it != fields.end(); ++it) {
        FieldSpec f;
        if (!parseField(it.value(), &f, error, d.id + QLatin1Char('.') + it.key())) return false;
        d.fields.insert(it.key(), f);
    }
    if (!d.fields.contains(QLatin1String("name"))) {
        if (error) *error = QStringLiteral("%1: response.fields.name is required").arg(d.id);
        return false;
    }
    if (!d.fields.contains(QLatin1String("infoHash")) && !d.fields.contains(QLatin1String("magnet"))
        && !d.fields.contains(QLatin1String("httpUrl"))) {
        if (error) *error = QStringLiteral("%1: needs fields.infoHash, fields.magnet or fields.httpUrl").arg(d.id);
        return false;
    }
    const QJsonObject fl = rs.value(QLatin1String("files")).toObject();
    if (!fl.isEmpty()) {
        d.files.items = fl.value(QLatin1String("items")).toString();
        if (!parseField(fl.value(QLatin1String("path")), &d.files.path, error, d.id + QStringLiteral(".files.path"))) return false;
        if (fl.contains(QLatin1String("size")) && !parseField(fl.value(QLatin1String("size")), &d.files.size, error, d.id + QStringLiteral(".files.size"))) return false;
        if (fl.contains(QLatin1String("url")) && !parseField(fl.value(QLatin1String("url")), &d.files.url, error, d.id + QStringLiteral(".files.url"))) return false;
        const QJsonObject flt = fl.value(QLatin1String("filter")).toObject();
        d.files.filterField = flt.value(QLatin1String("field")).toString();
        if (!d.files.filterField.isEmpty()
            && !compileRegex(flt.value(QLatin1String("regex")).toString(), &d.files.filterRegex, error, d.id + QStringLiteral(".files.filter")))
            return false;
        const QString ex = fl.value(QLatin1String("exclude")).toString();
        if (!ex.isEmpty() && !compileRegex(ex, &d.files.exclude, error, d.id + QStringLiteral(".files.exclude"))) return false;
        d.files.maxFiles = qBound(1, fl.value(QLatin1String("maxFiles")).toInt(2000), 20000);
    }
    const QJsonObject dt = o.value(QLatin1String("details")).toObject();
    if (!dt.isEmpty()) {
        d.details.url = dt.value(QLatin1String("url")).toString();
        d.details.key = dt.value(QLatin1String("key")).toString(QStringLiteral("details"));
        d.details.maxItems = qBound(0, dt.value(QLatin1String("maxItems")).toInt(8), 50);
        d.details.timeoutMs = qBound(1000, dt.value(QLatin1String("timeoutMs")).toInt(8000), 30000);
    }

    for (const QJsonValue &v : o.value(QLatin1String("categoryTypes")).toArray()) {
        const QJsonObject c = v.toObject();
        QRegularExpression re;
        if (!compileRegex(c.value(QLatin1String("match")).toString(), &re, error, d.id + QStringLiteral(".categoryTypes"))) return false;
        d.categoryTypes.append({re, mediaTypeFromKey(c.value(QLatin1String("type")).toString())});
    }
    for (const QJsonValue &v : o.value(QLatin1String("adultCategories")).toArray()) {
        QRegularExpression re;
        if (!compileRegex(v.toString(), &re, error, d.id + QStringLiteral(".adultCategories"))) return false;
        d.adultCategories.append(re);
    }
    d.defaultType = mediaTypeFromKey(o.value(QLatin1String("defaultType")).toString());
    d.matchName = o.value(QLatin1String("matchName")).toBool(true);
    for (const QJsonValue &v : o.value(QLatin1String("trackers")).toArray())
        if (!v.toString().trimmed().isEmpty()) d.trackers << v.toString().trimmed();
    *out = d;
    return true;
}

QList<QJsonObject> providerObjects(const QByteArray &json, QString *error)
{
    QList<QJsonObject> out;
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    if (doc.isNull()) {
        if (error) *error = QStringLiteral("JSON error at offset %1: %2").arg(pe.offset).arg(pe.errorString());
        return out;
    }
    QJsonArray arr;
    if (doc.isArray()) arr = doc.array();
    else if (doc.object().contains(QLatin1String("providers"))) arr = doc.object().value(QLatin1String("providers")).toArray();
    else arr.append(doc.object());
    for (const QJsonValue &v : arr)
        if (v.isObject()) out << v.toObject();
    if (out.isEmpty() && error) *error = QStringLiteral("no provider objects");
    return out;
}

QJsonObject mergeJsonObjects(const QJsonObject &base, const QJsonObject &over)
{
    QJsonObject r = base;
    for (auto it = over.begin(); it != over.end(); ++it) {
        if (it.value().isObject() && r.value(it.key()).isObject())
            r.insert(it.key(), mergeJsonObjects(r.value(it.key()).toObject(), it.value().toObject()));
        else
            r.insert(it.key(), it.value());
    }
    return r;
}

QString defaultProviderProfile()
{
#ifdef MX_WEB_DEFAULT_PROFILE
    return QStringLiteral(MX_WEB_DEFAULT_PROFILE);
#else
    return QStringLiteral("standard");
#endif
}

ProviderSet loadProviders(const QStringList &overrideFiles, const QByteArray &builtinOverride, const QString &profile)
{
    ProviderSet set;
    QList<QString> order;
    QHash<QString, QJsonObject> objs;
    QHash<QString, QString> origin;
    QSet<QString> userDefined;

    auto ingest = [&](const QByteArray &data, const QString &src, bool user) {
        QString err;
        const QList<QJsonObject> list = providerObjects(data, &err);
        if (list.isEmpty()) { set.messages << QStringLiteral("%1: %2").arg(src, err); return; }
        for (const QJsonObject &o : list) {
            const QString id = o.value(QLatin1String("id")).toString().trimmed();
            if (id.isEmpty()) { set.messages << QStringLiteral("%1: provider without id skipped").arg(src); continue; }
            if (objs.contains(id)) {
                objs[id] = mergeJsonObjects(objs.value(id), o);
            } else {
                objs.insert(id, o);
                order << id;
                if (user) userDefined.insert(id);
            }
            origin[id] = src;
        }
    };

    QByteArray builtin = builtinOverride;
    if (builtin.isEmpty()) {
        QFile f(QStringLiteral(":/mx/web/providers.json"));
        if (f.open(QIODevice::ReadOnly)) builtin = f.readAll();
        else set.messages << QStringLiteral("built-in provider list missing");
    }
    if (!builtin.isEmpty()) ingest(builtin, QStringLiteral("builtin"), false);
    // Default-enabled set of the built-in providers ("defaults" in providers.json, profile picked
    // at build time with MX_WEB_DEFAULT_PROFILE). Applied before the user's override files, so
    // those (and the user's per-provider toggles) still win.
    const QJsonObject defaults = QJsonDocument::fromJson(builtin).object().value(QLatin1String("defaults")).toObject();
    if (!defaults.isEmpty()) {
        const QString prof = profile.isEmpty() ? defaultProviderProfile() : profile;
        const QJsonValue list = defaults.value(prof);
        if (!list.isArray()) {
            set.messages << QStringLiteral("builtin: unknown default profile \"%1\", all providers enabled").arg(prof);
        } else {
            QSet<QString> on;
            for (const QJsonValue &v : list.toArray()) on.insert(v.toString());
            if (!on.contains(QStringLiteral("*")))
                for (const QString &id : std::as_const(order))
                    if (!on.contains(id)) objs[id].insert(QStringLiteral("enabled"), false);
        }
    }
    for (const QString &path : overrideFiles) {
        QFile f(path);
        if (!f.exists()) continue;
        if (f.size() > 1024 * 1024 || !f.open(QIODevice::ReadOnly)) {
            set.messages << QStringLiteral("%1: cannot read (or larger than 1 MB)").arg(path);
            continue;
        }
        ingest(f.readAll(), path, true);
    }
    for (const QString &id : order) {
        ProviderDef d;
        QString err;
        if (!parseProviderDef(objs.value(id), &d, &err)) {
            set.messages << QStringLiteral("%1: provider %2 skipped: %3").arg(origin.value(id), id, err);
            continue;
        }
        d.origin = origin.value(id);
        d.userDefined = userDefined.contains(id);
        set.providers << d;
    }
    return set;
}

} // namespace mx::web

// SPDX-License-Identifier: GPL-3.0-or-later
#include "response_parser.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtCore/QUrl>
#include <QtCore/QXmlStreamReader>

#include <functional>

#include "transforms.h"

namespace mx::web {

static constexpr int kMaxItems = 2000;
static constexpr int kMaxBody = 4 * 1024 * 1024;

QString variantText(const QVariant &v)
{
    switch (v.typeId()) {
    case QMetaType::QString: return v.toString();
    case QMetaType::Double: {
        const double d = v.toDouble();
        if (d == double(qint64(d)) && d > -9e18 && d < 9e18) return QString::number(qint64(d));
        return QString::number(d, 'g', 17);
    }
    case QMetaType::Bool: return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    case QMetaType::QVariantList: {
        QStringList parts;
        for (const QVariant &e : v.toList()) {
            const QString s = variantText(e);
            if (!s.isEmpty()) parts << s;
        }
        return parts.join(QLatin1Char(' '));
    }
    case QMetaType::QVariantMap:
    case QMetaType::UnknownType: return QString();
    default:
        if (v.isNull()) return QString();
        return v.toString();
    }
}

static QVariant step(const QVariant &cur, const QString &seg, bool *found)
{
    *found = false;
    if (cur.typeId() == QMetaType::QVariantMap) {
        const QVariantMap m = cur.toMap();
        auto it = m.constFind(seg);
        if (it == m.constEnd()) return QVariant();
        *found = true;
        return it.value();
    }
    if (cur.typeId() == QMetaType::QVariantList) {
        bool ok = false;
        const int idx = seg.toInt(&ok);
        const QVariantList l = cur.toList();
        if (!ok || idx < 0 || idx >= l.size()) return QVariant();
        *found = true;
        return l.at(idx);
    }
    return QVariant();
}

static QVariant resolveFrom(const QVariant &start, const QStringList &segs, bool *found)
{
    QVariant cur = start;
    *found = true;
    for (const QString &s : segs) {
        bool f = false;
        cur = step(cur, s, &f);
        if (!f) { *found = false; return QVariant(); }
    }
    return cur;
}

QVariant resolvePath(const ItemContext &ctx, const QString &pathIn, const QVariantMap *vars)
{
    QString path = pathIn.trimmed();
    if (path.startsWith(QLatin1Char('@'))) return vars ? vars->value(path) : QVariant();
    if (ctx.isEmpty()) return QVariant();
    if (path.isEmpty() || path == QLatin1String(".")) return ctx.last();
    int up = 0;
    while (path.startsWith(QLatin1String("../"))) { ++up; path = path.mid(3); }
    bool explicitRoot = false;
    if (path.startsWith(QLatin1String("$."))) { explicitRoot = true; path = path.mid(2); }
    const QStringList segs = path.split(QLatin1Char('.'), Qt::SkipEmptyParts);
    bool found = false;
    if (explicitRoot) return resolveFrom(ctx.first(), segs, &found);
    if (up > 0) {
        const int idx = ctx.size() - 1 - up;
        if (idx < 0) return QVariant();
        return resolveFrom(ctx.at(idx), segs, &found);
    }
    // innermost first, then outwards (a file entry can use its item's "identifier")
    for (int i = ctx.size() - 1; i >= 0; --i) {
        const QVariant v = resolveFrom(ctx.at(i), segs, &found);
        if (found) return v;
    }
    return QVariant();
}

static QString applyFilter(const QString &v, const QString &filter)
{
    const QString f = filter.trimmed().toLower();
    if (f == QLatin1String("urlencode")) return QString::fromLatin1(QUrl::toPercentEncoding(v));
    if (f == QLatin1String("urlpath")) return QString::fromLatin1(QUrl::toPercentEncoding(v, "/"));
    if (f == QLatin1String("lower")) return v.toLower();
    if (f == QLatin1String("upper")) return v.toUpper();
    if (f == QLatin1String("trim")) return v.trimmed();
    if (f == QLatin1String("html")) return stripHtml(v);
    if (f == QLatin1String("json")) {
        QString s = QString::fromUtf8(QJsonDocument(QJsonArray{v}).toJson(QJsonDocument::Compact));
        return s.mid(1, s.size() - 2);
    }
    return v;
}

QString expandTemplate(const QString &tmpl, const ItemContext &ctx, const QVariantMap *vars)
{
    static const QRegularExpression re(QStringLiteral("\\{([^{}]+)\\}"));
    QString out;
    int last = 0;
    auto it = re.globalMatch(tmpl);
    while (it.hasNext()) {
        const auto m = it.next();
        out += QStringView(tmpl).mid(last, m.capturedStart() - last);
        const QStringList parts = m.captured(1).split(QLatin1Char('|'));
        QString v = variantText(resolvePath(ctx, parts.first(), vars));
        for (int i = 1; i < parts.size(); ++i) v = applyFilter(v, parts.at(i));
        out += v;
        last = m.capturedEnd();
    }
    out += QStringView(tmpl).mid(last);
    return out;
}

static QString fieldText(const FieldSpec &f, const ItemContext &ctx, const QVariantMap *vars = nullptr)
{
    if (!f.valid()) return QString();
    QString t;
    if (!f.constant.isEmpty()) t = f.constant;
    else if (!f.tmpl.isEmpty()) t = expandTemplate(f.tmpl, ctx, vars);
    else t = variantText(resolvePath(ctx, f.path, vars));
    if (f.regex.isValid() && !f.regex.pattern().isEmpty()) {
        const auto m = f.regex.match(t);
        if (!m.hasMatch()) return QString();
        const QString named = m.captured(QStringLiteral("v"));
        if (!named.isEmpty()) t = named;
        else if (m.lastCapturedIndex() >= 1) t = m.captured(1);
        else t = m.captured(0);
    }
    if (f.transform == QLatin1String("html")) return stripHtml(t);
    if (f.transform == QLatin1String("lower")) return t.trimmed().toLower();
    return t.trimmed();
}

static int toCount(const QString &s)
{
    QString d;
    for (QChar c : s) {
        if (c.isDigit()) d += c;
        else if (c == QLatin1Char(',') && !d.isEmpty()) continue;
        else if (!d.isEmpty()) break;
        else if (!c.isSpace()) return -1;
    }
    bool ok = false;
    const qint64 v = d.toLongLong(&ok);
    if (!ok || v < 0) return -1;
    return int(qMin<qint64>(v, 1 << 30));
}

static QString cleanUrl(const QString &u)
{
    const QString s = decodeHtmlEntities(u.trimmed());
    if (s.startsWith(QLatin1String("http://")) || s.startsWith(QLatin1String("https://"))
        || s.startsWith(QLatin1String("magnet:"), Qt::CaseInsensitive))
        return s;
    return QString();
}

bool extractRecord(const ProviderDef &def, const ItemContext &ctx, TorrentRecord *out)
{
    TorrentRecord r;
    r.providerId = def.id;
    auto F = [&](const char *name) -> const FieldSpec * {
        auto it = def.fields.constFind(QLatin1String(name));
        return it == def.fields.constEnd() ? nullptr : &it.value();
    };
    auto text = [&](const char *name) -> QString {
        const FieldSpec *f = F(name);
        return f ? fieldText(*f, ctx) : QString();
    };

    r.name = repairMojibake(decodeHtmlEntities(text("name"))).simplified();
    if (r.name.isEmpty()) return false;

    r.magnet = cleanUrl(text("magnet"));
    if (!r.magnet.startsWith(QLatin1String("magnet:"), Qt::CaseInsensitive)) r.magnet.clear();
    if (const FieldSpec *f = F("infoHash")) {
        const QString h = fieldText(*f, ctx);
        r.btih = f->transform == QLatin1String("magnet") ? infoHashFromMagnet(h) : parseInfoHash(h);
        if (r.btih.isEmpty() && h.startsWith(QLatin1String("magnet:"), Qt::CaseInsensitive)) r.btih = infoHashFromMagnet(h);
    }
    if (r.btih.isEmpty() && !r.magnet.isEmpty()) r.btih = infoHashFromMagnet(r.magnet);

    if (const FieldSpec *f = F("size")) {
        const QString s = fieldText(*f, ctx);
        r.size = parseSize(s, def.decimalUnits);
    }
    if (r.size < 0 && !r.magnet.isEmpty()) {
        bool ok = false;
        const qint64 xl = magnetParam(r.magnet, QStringLiteral("xl")).toLongLong(&ok);
        if (ok && xl > 0) r.size = xl;
    }
    if (F("seeders")) r.seeders = toCount(text("seeders"));
    if (F("leechers")) r.leechers = toCount(text("leechers"));
    if (F("numFiles")) {
        r.numFiles = toCount(text("numFiles"));
        if (r.numFiles == 0) r.numFiles = -1;
    }
    if (const FieldSpec *f = F("published")) r.published = parseDate(fieldText(*f, ctx), f->format);
    r.category = decodeHtmlEntities(text("category")).simplified();
    // URL fields may refer to already extracted values: {@infoHash} (hex), {@name}
    const QVariantMap vars{{QStringLiteral("@infoHash"), QString::fromLatin1(r.btih.toHex())},
                           {QStringLiteral("@INFOHASH"), QString::fromLatin1(r.btih.toHex().toUpper())},
                           {QStringLiteral("@name"), r.name}};
    auto urlText = [&](const char *name) -> QString {
        const FieldSpec *f = F(name);
        return f ? cleanUrl(fieldText(*f, ctx, &vars)) : QString();
    };
    r.torrentUrl = urlText("torrentUrl");
    r.httpUrl = urlText("httpUrl");
    if (r.httpUrl.startsWith(QLatin1String("magnet:"))) r.httpUrl.clear();
    r.detailsUrl = urlText("detailsUrl");
    if (r.btih.isEmpty() && r.httpUrl.isEmpty()) return false;
    if (r.magnet.isEmpty() && !r.btih.isEmpty() && !def.trackers.isEmpty())
        r.magnet = buildMagnet(r.btih, r.name, r.size, def.trackers);

    // classification
    r.type = def.defaultType;
    if (!r.category.isEmpty()) {
        for (const auto &ct : def.categoryTypes)
            if (ct.first.match(r.category).hasMatch()) { r.type = ct.second; break; }
        for (const auto &re : def.adultCategories)
            if (re.match(r.category).hasMatch()) { r.adult = true; break; }
    }

    // file list
    if (def.files.valid()) {
        const QString path = def.files.items;
        // walk the item-relative items path
        const QStringList segs = path.split(QLatin1Char('.'), Qt::SkipEmptyParts);
        std::function<void(const QVariant &, int, ItemContext)> walk = [&](const QVariant &cur, int i, ItemContext stack) {
            if (r.files.size() >= def.files.maxFiles) return;
            if (i == segs.size()) {
                stack << cur;
                RecordFile rf;
                rf.path = decodeHtmlEntities(fieldText(def.files.path, stack));
                if (rf.path.isEmpty()) return;
                if (!def.files.filterField.isEmpty()) {
                    const QString fv = variantText(resolvePath(ItemContext{cur}, def.files.filterField));
                    if (!def.files.filterRegex.match(fv).hasMatch()) return;
                }
                if (def.files.exclude.isValid() && !def.files.exclude.pattern().isEmpty()
                    && def.files.exclude.match(rf.path).hasMatch())
                    return;
                if (def.files.size.valid()) rf.size = parseSize(fieldText(def.files.size, stack), def.decimalUnits);
                if (def.files.url.valid()) rf.url = cleanUrl(fieldText(def.files.url, stack));
                r.files << rf;
                return;
            }
            QString seg = segs.at(i);
            const bool iter = seg.endsWith(QLatin1String("[]"));
            if (iter) seg.chop(2);
            QVariant next = cur;
            if (!seg.isEmpty()) {
                bool f = false;
                next = step(cur, seg, &f);
                if (!f) return;
            }
            if (iter) {
                if (next.typeId() != QMetaType::QVariantList) return;
                for (const QVariant &e : next.toList()) {
                    if (i + 1 == segs.size()) walk(e, i + 1, stack);
                    else { ItemContext s2 = stack; s2 << e; walk(e, i + 1, s2); }
                }
            } else {
                walk(next, i + 1, stack);
            }
        };
        walk(ctx.last(), 0, ctx);
        if (r.numFiles < 0 && !r.files.isEmpty() && def.files.filterField.isEmpty()
            && def.files.exclude.pattern().isEmpty() && r.files.size() < def.files.maxFiles)
            r.numFiles = r.files.size();
    }
    *out = r;
    return true;
}

QVariant parseJsonVariant(const QByteArray &bodyIn, QString *error)
{
    if (bodyIn.size() > kMaxBody) { if (error) *error = QStringLiteral("response too large"); return QVariant(); }
    QByteArray body = bodyIn;
    if (body.startsWith("\xEF\xBB\xBF")) body = body.mid(3);
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
    if (doc.isNull()) {
        if (error) *error = QStringLiteral("invalid JSON (%1 at %2)").arg(pe.errorString()).arg(pe.offset);
        return QVariant();
    }
    return doc.isArray() ? QVariant(doc.array().toVariantList()) : QVariant(doc.object().toVariantMap());
}

// ---------------------------------------------------------------- JSON items
static void collectJsonItems(const QVariant &root, const QString &itemsPath, QList<ItemContext> *out)
{
    const QStringList segs = itemsPath.split(QLatin1Char('.'), Qt::SkipEmptyParts);
    std::function<void(const QVariant &, int, ItemContext)> walk = [&](const QVariant &cur, int i, ItemContext stack) {
        if (out->size() >= kMaxItems) return;
        if (i == segs.size()) {
            if (stack.isEmpty() || !(stack.last() == cur)) stack << cur;
            out->append(stack);
            return;
        }
        QString seg = segs.at(i);
        const bool iter = seg.endsWith(QLatin1String("[]"));
        if (iter) seg.chop(2);
        QVariant next = cur;
        if (!seg.isEmpty()) {
            bool f = false;
            next = step(cur, seg, &f);
            if (!f) return;
        }
        if (iter) {
            if (next.typeId() != QMetaType::QVariantList) return;
            for (const QVariant &e : next.toList()) {
                ItemContext s2 = stack;
                s2 << e;
                if (i + 1 == segs.size()) { if (out->size() < kMaxItems) out->append(s2); }
                else walk(e, i + 1, s2);
            }
        } else {
            walk(next, i + 1, stack);
        }
    };
    if (segs.isEmpty()) {
        // the root itself: a list of items or a single item
        if (root.typeId() == QMetaType::QVariantList)
            for (const QVariant &e : root.toList()) { if (out->size() < kMaxItems) out->append(ItemContext{root, e}); }
        return;
    }
    walk(root, 0, ItemContext{root});
}

// ---------------------------------------------------------------- XML items
static QList<ItemContext> collectXmlItems(const QByteArray &body, const QString &itemName, QString *error)
{
    QList<ItemContext> out;
    QXmlStreamReader xml(body);
    QVariantMap item;
    int depth = 0;                // > 0 while inside an item
    QList<QPair<QString, QString>> stack;  // (qualified name, text) of open elements inside the item
    auto put = [&](const QString &key, const QString &value) {
        if (!item.contains(key)) item.insert(key, value);
    };
    while (!xml.atEnd() && out.size() < kMaxItems) {
        const auto tok = xml.readNext();
        if (tok == QXmlStreamReader::StartElement) {
            const QString q = xml.qualifiedName().toString();
            const QString local = xml.name().toString();
            if (depth == 0) {
                if (q == itemName || local == itemName) { depth = 1; item.clear(); stack.clear(); }
                continue;
            }
            ++depth;
            stack.append({q, QString()});
            for (const QXmlStreamAttribute &a : xml.attributes()) {
                put(q + QLatin1Char('@') + a.qualifiedName().toString(), a.value().toString());
                if (local != q) put(local + QLatin1Char('@') + a.name().toString(), a.value().toString());
            }
        } else if (tok == QXmlStreamReader::Characters) {
            if (depth > 1 && !stack.isEmpty()) stack.last().second += xml.text();
        } else if (tok == QXmlStreamReader::EndElement) {
            if (depth == 0) continue;
            if (depth == 1) {
                out.append(ItemContext{QVariant(), item});
                depth = 0;
                continue;
            }
            --depth;
            if (!stack.isEmpty()) {
                const auto el = stack.takeLast();
                put(el.first, el.second.trimmed());
                const int colon = el.first.indexOf(QLatin1Char(':'));
                if (colon > 0) put(el.first.mid(colon + 1), el.second.trimmed());
            }
        }
    }
    if (xml.hasError() && out.isEmpty() && error) *error = QStringLiteral("XML error: %1").arg(xml.errorString());
    return out;
}

// ---------------------------------------------------------------- regex items
static QList<ItemContext> collectRegexItems(const QString &text, const QString &pattern, QString *error)
{
    QList<ItemContext> out;
    const QRegularExpression re(pattern, QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    if (!re.isValid()) { if (error) *error = re.errorString(); return out; }
    const QStringList names = re.namedCaptureGroups();
    auto it = re.globalMatch(text);
    while (it.hasNext() && out.size() < kMaxItems) {
        const auto m = it.next();
        QVariantMap item;
        item.insert(QStringLiteral("_"), m.captured(0));
        for (int i = 1; i < names.size(); ++i)
            if (!names.at(i).isEmpty()) item.insert(names.at(i), m.captured(i));
        for (int i = 1; i <= m.lastCapturedIndex(); ++i) item.insert(QString::number(i), m.captured(i));
        out.append(ItemContext{QVariant(), item});
    }
    return out;
}

ParseResult parseResponse(const ProviderDef &def, const QByteArray &body)
{
    ParseResult res;
    if (body.size() > kMaxBody) { res.error = QStringLiteral("response too large"); return res; }
    QList<ItemContext> items;
    if (def.format == QLatin1String("json")) {
        QString err;
        const QVariant root = parseJsonVariant(body, &err);
        if (!root.isValid()) { res.error = err; return res; }
        QString path = def.items;
        if (path == QLatin1String("[]")) path.clear();
        collectJsonItems(root, path, &items);
    } else if (def.format == QLatin1String("rss") || def.format == QLatin1String("xml")) {
        QString err;
        items = collectXmlItems(body, def.items, &err);
        if (items.isEmpty() && !err.isEmpty()) {
            // an empty RSS channel is fine; only a document that is not XML at all is an error
            const QByteArray head = body.left(512).trimmed().toLower();
            if (!head.startsWith("<?xml") && !head.startsWith("<rss") && !head.startsWith("<feed")) {
                res.error = err;
                return res;
            }
        }
    } else {
        QString err;
        items = collectRegexItems(QString::fromUtf8(body), def.items, &err);
        if (!err.isEmpty()) { res.error = err; return res; }
    }
    res.ok = true;
    res.rawItems = items.size();
    for (const ItemContext &ctx : items) {
        TorrentRecord r;
        if (extractRecord(def, ctx, &r)) {
            res.records << r;
            res.contexts << ctx;
        } else {
            ++res.skipped;
        }
    }
    return res;
}

bool applyDetails(const ProviderDef &def, ItemContext ctx, const QByteArray &body, TorrentRecord *rec)
{
    if (ctx.isEmpty()) return false;
    QString err;
    const QVariant doc = parseJsonVariant(body, &err);
    if (!doc.isValid()) return false;
    QVariantMap item = ctx.last().toMap();
    item.insert(def.details.key, doc);
    ctx.last() = item;
    TorrentRecord r;
    if (!extractRecord(def, ctx, &r)) return false;
    r.extra = rec->extra;
    *rec = r;
    return true;
}

} // namespace mx::web

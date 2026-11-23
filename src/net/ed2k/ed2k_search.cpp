// SPDX-License-Identifier: GPL-3.0-or-later
#include "ed2k_search.h"
#include "ed2k_io.h"
#include "ed2k_opcodes.h"

#include <QtCore/QUrl>
#include <functional>

namespace mx::ed2k {

// ---------------------------------------------------------------- parsing
namespace {
struct Tok { enum T { Word, Phrase, AndT, OrT, NotT, LParen, RParen, End } t; QString s; };

QList<Tok> tokenize(const QString &in)
{
    QList<Tok> out;
    int i = 0;
    const int n = int(in.size());
    while (i < n) {
        const QChar c = in[i];
        if (c.isSpace()) { ++i; continue; }
        if (c == QLatin1Char('(')) { out.append(Tok{Tok::LParen, QString()}); ++i; continue; }
        if (c == QLatin1Char(')')) { out.append(Tok{Tok::RParen, QString()}); ++i; continue; }
        if (c == QLatin1Char('"')) {
            int j = i + 1;
            while (j < n && in[j] != QLatin1Char('"')) ++j;
            const QString p = in.mid(i + 1, j - i - 1).simplified();
            if (!p.isEmpty()) out.append(Tok{Tok::Phrase, p});
            i = j + 1;
            continue;
        }
        if (c == QLatin1Char('-') && i + 1 < n && !in[i + 1].isSpace()) {
            out.append(Tok{Tok::NotT, QString()});
            ++i;
            continue;
        }
        int j = i;
        while (j < n && !in[j].isSpace() && in[j] != QLatin1Char('(') && in[j] != QLatin1Char(')')
               && in[j] != QLatin1Char('"')) ++j;
        const QString w = in.mid(i, j - i);
        if (w == QLatin1String("AND")) out.append(Tok{Tok::AndT, QString()});
        else if (w == QLatin1String("OR")) out.append(Tok{Tok::OrT, QString()});
        else if (w == QLatin1String("NOT")) out.append(Tok{Tok::NotT, QString()});
        else out.append(Tok{Tok::Word, w});
        i = j;
    }
    out.append(Tok{Tok::End, QString()});
    return out;
}

struct Parser {
    QList<Tok> t;
    int p = 0;
    QString err;
    int depth = 0;
    const Tok &peek() const { return t[p]; }
    SearchNodePtr kw(const QString &s) { auto n = std::make_shared<SearchNode>(); n->kind = SearchNode::Keyword; n->text = s; return n; }
    SearchNodePtr bin(SearchNode::Kind k, SearchNodePtr a, SearchNodePtr b) {
        auto n = std::make_shared<SearchNode>(); n->kind = k; n->a = a; n->b = b; return n;
    }
    // primary: run of words (concatenated) | phrase | ( or )
    SearchNodePtr primary() {
        if (++depth > 32) { err = QStringLiteral("expression too deep"); return nullptr; }
        SearchNodePtr r;
        if (peek().t == Tok::LParen) {
            ++p;
            r = orExpr();
            if (!r) return nullptr;
            if (peek().t != Tok::RParen) { err = QStringLiteral("missing ')'"); return nullptr; }
            ++p;
        } else if (peek().t == Tok::Word || peek().t == Tok::Phrase) {
            QString s = peek().s;
            ++p;
            while (peek().t == Tok::Word || peek().t == Tok::Phrase) { s += QLatin1Char(' ') + peek().s; ++p; }
            r = kw(s);
        } else {
            err = QStringLiteral("keyword expected");
            return nullptr;
        }
        --depth;
        return r;
    }
    // andExpr: primary { [AND] primary | NOT primary }
    SearchNodePtr andExpr() {
        SearchNodePtr left;
        if (peek().t == Tok::NotT) { err = QStringLiteral("NOT needs a left operand"); return nullptr; }
        left = primary();
        if (!left) return nullptr;
        for (;;) {
            if (peek().t == Tok::AndT) { ++p; auto r = primary(); if (!r) return nullptr; left = bin(SearchNode::And, left, r); }
            else if (peek().t == Tok::NotT) { ++p; auto r = primary(); if (!r) return nullptr; left = bin(SearchNode::Not, left, r); }
            else if (peek().t == Tok::LParen || peek().t == Tok::Word || peek().t == Tok::Phrase) {
                auto r = primary(); if (!r) return nullptr; left = bin(SearchNode::And, left, r);
            }
            else break;
        }
        return left;
    }
    SearchNodePtr orExpr() {
        SearchNodePtr left = andExpr();
        if (!left) return nullptr;
        while (peek().t == Tok::OrT) {
            ++p;
            auto r = andExpr();
            if (!r) return nullptr;
            left = bin(SearchNode::Or, left, r);
        }
        return left;
    }
};
} // namespace

SearchNodePtr parseSearchExpression(const QString &text, QString *error)
{
    Parser ps;
    ps.t = tokenize(text.left(512));
    if (ps.t.size() <= 1) { if (error) *error = QStringLiteral("empty query"); return nullptr; }
    SearchNodePtr n = ps.orExpr();
    if (n && ps.peek().t != Tok::End) { n.reset(); ps.err = QStringLiteral("unexpected token"); }
    if (!n && error) *error = ps.err;
    return n;
}

QString searchNodeToString(const SearchNodePtr &n)
{
    if (!n) return QString();
    switch (n->kind) {
    case SearchNode::Keyword: return QLatin1Char('"') + n->text + QLatin1Char('"');
    case SearchNode::And: return QStringLiteral("(%1 AND %2)").arg(searchNodeToString(n->a), searchNodeToString(n->b));
    case SearchNode::Or: return QStringLiteral("(%1 OR %2)").arg(searchNodeToString(n->a), searchNodeToString(n->b));
    case SearchNode::Not: return QStringLiteral("(%1 NOT %2)").arg(searchNodeToString(n->a), searchNodeToString(n->b));
    }
    return QString();
}

// ---------------------------------------------------------------- encoding
static void encodeNode(Writer &w, const SearchNodePtr &n)
{
    switch (n->kind) {
    case SearchNode::Keyword: w.u8(0x01).str16(n->text); return;
    case SearchNode::And: w.u8(0x00).u8(0x00); break;
    case SearchNode::Or:  w.u8(0x00).u8(0x01); break;
    case SearchNode::Not: w.u8(0x00).u8(0x02); break;
    }
    encodeNode(w, n->a);
    encodeNode(w, n->b);
}

QString ed2kTypeForMedia(mx::MediaType t)
{
    switch (t) {
    case mx::MediaType::Audio: return QStringLiteral("Audio");
    case mx::MediaType::Video: return QStringLiteral("Video");
    case mx::MediaType::Image: return QStringLiteral("Image");
    case mx::MediaType::Document: return QStringLiteral("Doc");
    case mx::MediaType::Software: return QStringLiteral("Pro");
    case mx::MediaType::Archive: return QStringLiteral("Pro");   // archives are published/searched as "Pro"
    default: return QString();
    }
}

QByteArray encodeSearchTree(const SearchNodePtr &expr, const mx::SearchQuery &q, const SearchEncodeOptions &opt,
                            bool withExtension, bool withBitrate)
{
    // operands, ANDed in the interleaved form "AND e1 AND e2 ... en"
    QList<std::function<void(Writer &)>> ops;
    if (expr) ops.append([expr](Writer &w) { encodeNode(w, expr); });
    const QString type = ed2kTypeForMedia(q.type);
    if (!type.isEmpty())
        ops.append([type](Writer &w) { w.u8(0x02).raw16(type.toLatin1()).u16(1).u8(FT_FILETYPE); });
    auto numParam = [&opt](Writer &w, quint64 v, quint8 op, quint8 id, const QByteArray &name) {
        if (v > 0xFFFFFFFFull && opt.largeFiles) w.u8(0x08).u64(v);
        else w.u8(0x03).u32(quint32(qMin<quint64>(v, 0xFFFFFFFFull)));
        w.u8(op);
        if (name.isEmpty()) w.u16(1).u8(id); else w.raw16(name);
    };
    if (q.minSize > 0) {
        const quint64 v = quint64(q.minSize - 1);   // GREATER is exclusive
        ops.append([=](Writer &w) { numParam(w, v, SEARCH_OP_GREATER, FT_FILESIZE, {}); });
    }
    if (q.maxSize > 0) {
        const quint64 v = quint64(q.maxSize) + 1;
        ops.append([=](Writer &w) { numParam(w, v, SEARCH_OP_LESS, FT_FILESIZE, {}); });
    }
    // [ui-f] one extension is asked for (FT_FILEFORMAT); several ("Lossless", "Custom") are not: the
    // type term narrows the answer and the core filters the names (mx::hitMatchesFileType)
    const QStringList exts = mx::queryExtensions(q);
    const QString ext = exts.size() == 1 ? exts.first() : QString();
    if (withExtension && !ext.isEmpty())
        ops.append([ext](Writer &w) { w.u8(0x02).str16(ext).u16(1).u8(FT_FILEFORMAT); });
    if (withBitrate && q.minBitrate > 0) {
        const quint64 v = quint64(q.minBitrate - 1);
        ops.append([=](Writer &w) { numParam(w, v, SEARCH_OP_GREATER, 0, QByteArray("bitrate")); });
    }
    if (withBitrate && q.maxBitrate > 0) {   // [ui-f] "Bitrate at best" / "equal to"
        const quint64 v = quint64(q.maxBitrate) + 1;
        ops.append([=](Writer &w) { numParam(w, v, SEARCH_OP_LESS, 0, QByteArray("bitrate")); });
    }
    Writer w;
    for (int i = 0; i < ops.size(); ++i) {
        if (i + 1 < ops.size()) w.u8(0x00).u8(0x00);
        ops[i](w);
    }
    return w.take();
}

QByteArray encodeSearchRequest(const mx::SearchQuery &q, const SearchEncodeOptions &opt, QString *error)
{
    QString text = q.text.trimmed();
    if (!q.text2.trimmed().isEmpty())
        text = text.isEmpty() ? q.text2.trimmed() : QStringLiteral("(%1) (%2)").arg(text, q.text2.trimmed());
    SearchNodePtr expr = parseSearchExpression(text, error);
    if (!expr) return QByteArray();
    return encodeSearchTree(expr, q, opt, true, true);
}

// ---------------------------------------------------------------- results
static int parseLength(const Tag &t)
{
    if (t.isInt()) return int(qMin<quint64>(t.num, 0x7FFFFFFF));
    if (!t.isStr()) return -1;
    const QStringList p = t.str.split(QLatin1Char(':'));
    int v = 0;
    for (const QString &x : p) {
        bool ok = false;
        const int k = x.trimmed().toInt(&ok);
        if (!ok || k < 0) return -1;
        v = v * 60 + k;
    }
    return v > 0 ? v : -1;
}

bool parseSearchResultEntry(Reader &r, SearchResultEntry *out)
{
    SearchResultEntry e;
    e.hash = r.hash16();
    e.clientId = r.u32();
    e.clientPort = r.u16();
    TagList tags;
    if (!readTagList(r, &tags, 200)) return false;
    quint64 sizeLo = 0, sizeHi = 0;
    bool haveSize = false;
    for (const Tag &t : tags) {
        if (t.name.isEmpty()) {
            switch (t.id) {
            case FT_FILENAME: if (t.isStr()) e.name = t.str; break;
            case FT_FILESIZE: if (t.isInt()) { sizeLo = t.num; haveSize = true; } break;
            case FT_FILESIZE_HI: if (t.isInt()) sizeHi = t.num; break;
            case FT_FILETYPE:
                if (t.isStr()) e.typeText = t.str;
                else if (t.isInt()) {
                    static const char *const names[] = {"", "Audio", "Video", "Image", "Pro", "Doc", "Arc", "Iso"};
                    if (t.num < 8) e.typeText = QLatin1String(names[t.num]);
                }
                break;
            case FT_FILEFORMAT: if (t.isStr()) e.format = t.str; break;
            case FT_SOURCES: if (t.isInt()) e.sources = int(qMin<quint64>(t.num, 0x7FFFFFFF)); break;
            case FT_COMPLETE_SOURCES: if (t.isInt()) e.completeSources = int(qMin<quint64>(t.num, 0x7FFFFFFF)); break;
            case FT_MEDIA_LENGTH: e.lengthSec = parseLength(t); break;
            case FT_MEDIA_BITRATE: if (t.isInt()) e.bitrateKbps = int(qMin<quint64>(t.num, 100000)); break;
            case FT_MEDIA_CODEC: if (t.isStr()) e.codec = t.str; break;
            case FT_MEDIA_ARTIST: if (t.isStr()) e.artist = t.str; break;
            case FT_MEDIA_ALBUM: if (t.isStr()) e.album = t.str; break;
            case FT_MEDIA_TITLE: if (t.isStr()) e.title = t.str; break;
            default: break;
            }
        } else if (t.is("length")) { e.lengthSec = parseLength(t); }
        else if (t.is("bitrate")) { if (t.isInt()) e.bitrateKbps = int(qMin<quint64>(t.num, 100000)); }
        else if (t.is("codec")) { if (t.isStr()) e.codec = t.str; }
        else if (t.is("Artist")) { if (t.isStr()) e.artist = t.str; }
        else if (t.is("Album")) { if (t.isStr()) e.album = t.str; }
        else if (t.is("Title")) { if (t.isStr()) e.title = t.str; }
    }
    if (!r.ok()) return false;
    if (!haveSize) return false;
    e.size = qint64(sizeLo & 0xFFFFFFFFull) | (qint64(sizeHi) << 32);
    if (sizeLo > 0xFFFFFFFFull) e.size = qint64(sizeLo);   // 64-bit FT_FILESIZE
    if (e.size <= 0 || e.size > MAX_FILE_SIZE || e.name.trimmed().isEmpty()) {
        // keep parsing the list, but this entry is useless
        e.size = 0;
    }
    if (out) *out = e;
    return true;
}

bool parseSearchResults(const QByteArray &payload, QList<SearchResultEntry> *out, bool *more)
{
    Reader r(payload);
    const quint32 n = r.u32();
    if (!r.ok() || qint64(n) * 26 > r.remaining() || n > 5000) return false;
    QList<SearchResultEntry> list;
    for (quint32 i = 0; i < n; ++i) {
        SearchResultEntry e;
        if (!parseSearchResultEntry(r, &e)) return false;
        if (e.size > 0) list.append(e);
    }
    if (more) *more = (r.remaining() >= 1) && r.u8() != 0;
    if (out) *out = list;
    return true;
}

QString formatEd2kFileLink(const QString &name, qint64 size, const QByteArray &hash)
{
    QString n = name;
    n.replace(QLatin1Char('|'), QLatin1Char('_'));
    const QString enc = QString::fromUtf8(QUrl::toPercentEncoding(n, " ()[]{}!'$&+,;=@~"));
    return QStringLiteral("ed2k://|file|%1|%2|%3|/").arg(enc).arg(size).arg(toHex(hash).toUpper());
}

mx::MediaType mediaTypeFromEd2k(const QString &t, const QString &fileName)
{
    if (t == QLatin1String("Audio")) return mx::MediaType::Audio;
    if (t == QLatin1String("Video")) return mx::MediaType::Video;
    if (t == QLatin1String("Image")) return mx::MediaType::Image;
    if (t == QLatin1String("Doc")) return mx::MediaType::Document;
    if (t == QLatin1String("Arc")) return mx::MediaType::Archive;
    if (t == QLatin1String("Iso")) return mx::MediaType::Software;   // [ui-f] disc images are software
    mx::MediaType byName = mx::mediaTypeForFileName(fileName);
    if (t == QLatin1String("Pro") && byName != mx::MediaType::Archive) return mx::MediaType::Software;
    return byName;
}

mx::SearchHit toSearchHit(const SearchResultEntry &e, quint32 queryId)
{
    mx::SearchHit h;
    h.queryId = queryId;
    h.fileName = e.name;
    h.size = e.size;
    h.type = mediaTypeFromEd2k(e.typeText, e.name);
    h.durationSec = e.lengthSec;
    h.bitrateKbps = e.bitrateKbps;
    h.hashes.ed2k = e.hash;
    h.key = QStringLiteral("ed2k:") + toHex(e.hash);
    mx::SearchSource s;
    s.net = mx::Net::ED2K;
    s.uri = formatEd2kFileLink(e.name, e.size, e.hash);
    s.connection = QStringLiteral("eD2k");
    s.seeds = e.completeSources;
    s.peers = e.sources;
    if (e.clientId && !isLowId(e.clientId) && e.clientId != FILE_COMPLETE_ID && e.clientId != FILE_INCOMPLETE_ID
        && e.clientPort && e.clientPort != FILE_COMPLETE_PORT && e.clientPort != FILE_INCOMPLETE_PORT
        && isRoutableIp(e.clientId)) {
        const QString addr = QStringLiteral("%1:%2").arg(ipToString(e.clientId)).arg(e.clientPort);
        s.user.net = mx::Net::ED2K;
        s.user.id = addr;
        s.user.name = addr;
        s.user.address = addr;
    }
    if (!e.codec.isEmpty()) s.extra.insert(QStringLiteral("codec"), e.codec);
    if (!e.artist.isEmpty()) s.extra.insert(QStringLiteral("artist"), e.artist);
    if (!e.album.isEmpty()) s.extra.insert(QStringLiteral("album"), e.album);
    if (!e.title.isEmpty()) s.extra.insert(QStringLiteral("title"), e.title);
    h.sources.append(s);
    return h;
}

} // namespace mx::ed2k

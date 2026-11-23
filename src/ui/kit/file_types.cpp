// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/kit/file_types.h"

#include "ui/kit/format.h"

#include <QtGui/QAction>
#include <QtGui/QFontMetrics>
#include <QtWidgets/QMenu>

#include "mx/types.h"
#include "ui/kit/widgets.h"

namespace mx::ui::filetypes {

namespace {
const int kBitrates[] = {320, 256, 224, 192, 160, 128, 112, 96, 80, 64, 56, 48, 40, 32};
const struct { const char *code; const char *menu; const char *button; } kOps[] = {
    {"min", QT_TR_NOOP("Bitrate at least"), QT_TR_NOOP("At least")},
    {"eq", QT_TR_NOOP("Bitrate equal to"), QT_TR_NOOP("Equal to")},
    {"max", QT_TR_NOOP("Bitrate at best"), QT_TR_NOOP("At best")},
};
const int kShown = 4;   // extensions in a label before "..."

QString dotted(const QStringList &exts, int n)
{
    QStringList parts;
    for (int i = 0; i < exts.size() && i < n; ++i)
        parts << QLatin1Char('.') + exts.at(i);
    if (n < exts.size())
        parts << QStringLiteral("...");
    return parts.join(QLatin1Char(' '));
}

int shownCount(const QStringList &exts)
{
    return exts.size() <= kShown + 1 ? int(exts.size()) : kShown;
}

QString opButton(const QString &op)
{
    for (const auto &o : kOps)
        if (op == QLatin1String(o.code))
            return QObject::tr(o.button);
    return QString();
}
} // namespace

QString kindName(const QString &kind)
{
    static const struct { const char *kind; const char *name; } names[] = {
        {"any", QT_TR_NOOP("All Files")},          {"audio", QT_TR_NOOP("Audio")},
        {"video", QT_TR_NOOP("Video")},            {"pictures", QT_TR_NOOP("Pictures")},
        {"documents", QT_TR_NOOP("Documents")},    {"software", QT_TR_NOOP("Software")},
        {"archives", QT_TR_NOOP("Archives")},      {"discimages", QT_TR_NOOP("Disc Images")},
        {"torrents", QT_TR_NOOP("Torrents")},
        {"mp3", QT_TR_NOOP("MP3 only")},           {"lossless", QT_TR_NOOP("Lossless audio")},
        {"servers", QT_TR_NOOP("Server Lists")},   {"clr", QT_TR_NOOP("Colour Schemes")},
        {"custom", QT_TR_NOOP("Custom")},
    };
    for (const auto &n : names)
        if (kind == QLatin1String(n.kind))
            return QObject::tr(n.name);
    return QString();
}

QStringList kindExtensions(const QString &kind)
{
    FileTypeFilter f;
    f.kind = kind;
    const QStringList req = f.requiredExtensions();
    if (!req.isEmpty())
        return req;
    return extensionsForMediaType(f.mediaType());
}

QString groupLabel(const QString &name, const QStringList &extensions)
{
    if (extensions.isEmpty())
        return name;
    return QStringLiteral("%1 (%2)").arg(name, dotted(extensions, shownCount(extensions)));
}

QString menuLabel(const QString &kind)
{
    if (kind == QLatin1String("any") || kind == QLatin1String("custom"))
        return kindName(kind);
    return groupLabel(kindName(kind), kindExtensions(kind));
}

QString buttonText(const QString &code)
{
    const FileTypeFilter f = FileTypeFilter::fromCode(code);
    if (!f.isValid())
        return QString();
    if (f.kind == QLatin1String("custom"))
        return groupLabel(kindName(f.kind), f.custom);
    if (!f.bitrateOp.isEmpty())
        return QObject::tr("%1 - %2 %3 kbps").arg(kindName(f.kind), opButton(f.bitrateOp)).arg(f.kbps);
    if (f.kind == QLatin1String("mp3"))
        return QObject::tr("%1 - Any Bitrate").arg(kindName(f.kind));
    return menuLabel(f.kind);
}

QString fittedButtonText(const QString &code, const QFontMetrics &fm, int width)
{
    const QString full = buttonText(code);
    if (fm.horizontalAdvance(full) <= width)
        return full;
    const FileTypeFilter f = FileTypeFilter::fromCode(code);
    if (f.isValid() && f.bitrateOp.isEmpty() && f.kind != QLatin1String("any") && f.kind != QLatin1String("mp3")) {
        const QStringList exts = f.kind == QLatin1String("custom") ? f.custom : kindExtensions(f.kind);
        for (int n = qMin<int>(shownCount(exts), int(exts.size())) - 1; n >= 1; --n) {
            const QString t = QStringLiteral("%1 (%2)").arg(kindName(f.kind), dotted(exts, n));
            if (fm.horizontalAdvance(t) <= width)
                return t;
        }
        if (fm.horizontalAdvance(kindName(f.kind)) <= width)
            return kindName(f.kind);
    }
    return fmt::elide(full, fm, width);   // [post-1] three ASCII dots
}

QString kindDescription(const QString &kind)
{
    if (kind == QLatin1String("any"))
        return QObject::tr("All Files: every file type");
    const QStringList exts = kindExtensions(kind);
    QStringList dots;
    for (const QString &e : exts)
        dots << QLatin1Char('.') + e;
    return QObject::tr("%1: %2").arg(kindName(kind), dots.join(QLatin1Char(' ')));
}

QString description(const QString &code)
{
    const FileTypeFilter f = FileTypeFilter::fromCode(code);
    if (!f.isValid())
        return QString();
    QString d;
    if (f.kind == QLatin1String("custom")) {
        QStringList dots;
        for (const QString &e : f.custom)
            dots << QLatin1Char('.') + e;
        d = QObject::tr("%1: %2").arg(kindName(f.kind), dots.join(QLatin1Char(' ')));
    } else {
        d = kindDescription(f.kind);
    }
    if (!f.bitrateOp.isEmpty())
        d += QObject::tr(", bitrate %1 %2 kbps").arg(opButton(f.bitrateOp).toLower()).arg(f.kbps);
    return d;
}

namespace {
QAction *choice(MenuButton *b, QMenu *menu, const QString &label, const QString &code)
{
    QAction *a = b->addChoiceTo(menu, label, code, buttonText(code));
    const QString d = description(code);
    a->setToolTip(d);
    a->setStatusTip(d);
    return a;
}

void bitrateMenus(MenuButton *b, QMenu *parent, const QString &kind)
{
    choice(b, parent, QObject::tr("Any Bitrate"), kind);
    parent->addSeparator();
    for (const auto &o : kOps) {
        QMenu *sub = parent->addMenu(QObject::tr(o.menu));
        sub->setToolTipsVisible(true);
        for (int kb : kBitrates)
            choice(b, sub, QObject::tr("%1 kbps").arg(kb), QStringLiteral("%1:%2:%3").arg(kind, QString::fromLatin1(o.code)).arg(kb));
    }
}

QMenu *submenu(MenuButton *b, const QString &kind)
{
    QMenu *m = b->addSubmenu(menuLabel(kind));
    m->setToolTipsVisible(true);
    m->menuAction()->setToolTip(kindDescription(kind));
    m->menuAction()->setStatusTip(kindDescription(kind));
    return m;
}
} // namespace

void populateMenu(MenuButton *b, bool customDialogEntry, const QStringList &customCodes)
{
    b->menu()->setToolTipsVisible(true);
    b->setTextFitter([](const QVariant &data, const QFontMetrics &fm, int room) {
        return fittedButtonText(data.toString(), fm, room);   // fewer extensions rather than a cut label
    });
    choice(b, b->menu(), menuLabel(QStringLiteral("any")), QStringLiteral("any"));
    bitrateMenus(b, submenu(b, QStringLiteral("audio")), QStringLiteral("audio"));
    for (const char *k : {"video", "pictures", "documents", "software", "archives", "discimages", "torrents"})
        choice(b, b->menu(), menuLabel(QLatin1String(k)), QLatin1String(k));
    b->addSeparator();
    bitrateMenus(b, submenu(b, QStringLiteral("mp3")), QStringLiteral("mp3"));
    for (const char *k : {"lossless", "servers", "clr"})
        choice(b, b->menu(), menuLabel(QLatin1String(k)), QLatin1String(k));
    b->addSeparator();
    if (customDialogEntry) {
        QAction *a = b->addChoice(QObject::tr("Custom Extensions..."), QStringLiteral("custom"), kindName(QStringLiteral("custom")));
        a->setObjectName(QStringLiteral("fileTypeCustom"));
        const QString tip = QObject::tr("Search for files with the extensions you type (for example: mkv srt)");
        a->setToolTip(tip);
        a->setStatusTip(tip);
    }
    for (const QString &c : customCodes) {
        const FileTypeFilter f = FileTypeFilter::fromCode(c);
        if (f.kind == QLatin1String("custom"))
            choice(b, b->menu(), buttonText(f.code()), f.code());
    }
}

QList<QMenu *> menusOf(MenuButton *button)
{
    QList<QMenu *> out{button->menu()};
    out << button->menu()->findChildren<QMenu *>();
    return out;
}

} // namespace mx::ui::filetypes

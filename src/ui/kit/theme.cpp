// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/kit/theme.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtGui/QFontDatabase>
#include <QtGui/QFontMetrics>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QApplication>
#include <QtWidgets/QStyleFactory>
#include <QtWidgets/QToolTip>

#include "ui/kit/tips.h"

#include "mx/services.h"
#include "ui/kit/classic_style.h"
#include "ui/kit/colors.h"
#include "ui/kit/scale.h"

namespace mx::ui {

namespace {
QString g_family;
QString g_fontSource;
double g_classicWidthFactor = 1.0;
bool g_loadedScaled = false;     // [ui-e] the faces were loaded for a scale above 100 % (no strikes)
QList<int> g_fontIds;

// [qa-1] FreeType derives the line height of a scalable font with an embedded strike from hhea and
// rounds it to 12 px for the 11-px face (leading -1), while GDI / DirectWrite (Windows, wine) use 13:
// multi-line labels and layouts were 1 px per line tighter on Linux. A small hhea lineGap (40/2048 em,
// 0.2 px at 11 px, which rounds away on Windows) makes FreeType report 13 as well. Applied only when
// the engine shows the negative leading, so Windows rendering is untouched.
QByteArray withLineGap(QByteArray data, qint16 gap)
{
    if (data.size() < 12)
        return data;
    auto be16 = [&](int o) { return (quint8(data[o]) << 8) | quint8(data[o + 1]); };
    auto be32 = [&](int o) { return (quint32(be16(o)) << 16) | quint32(be16(o + 2)); };
    const int n = be16(4);
    for (int i = 0; i < n && 12 + i * 16 + 16 <= data.size(); ++i) {
        const int rec = 12 + i * 16;
        if (data.mid(rec, 4) == "hhea") {
            const int off = int(be32(rec + 8));
            if (off + 10 <= data.size()) {
                data[off + 8] = char((gap >> 8) & 0xff);
                data[off + 9] = char(gap & 0xff);
            }
            break;
        }
    }
    return data;
}

// [ui-e] Above 100 % the embedded bitmap strikes must not be used (Wine's Tahoma has strikes up to 18 px:
// at 150 % they replaced the antialiased outlines with thin bitmap glyphs on outline advances). Renaming
// the EBDT / EBLC / EBSC tables hides them from the font engine; 100 % keeps them (the classic 11-px look).
QByteArray withoutBitmaps(QByteArray data)
{
    if (data.size() < 12)
        return data;
    const int n = (quint8(data[4]) << 8) | quint8(data[5]);
    for (int i = 0; i < n && 12 + i * 16 + 16 <= data.size(); ++i) {
        const int rec = 12 + i * 16;
        const QByteArray tag = data.mid(rec, 4);
        if (tag == "EBDT" || tag == "EBLC" || tag == "EBSC" || tag == "bdat" || tag == "bloc")
            data[rec] = 'x';
    }
    return data;
}

int addFont(const QByteArray &data)
{
    const int id = data.isEmpty() ? -1 : QFontDatabase::addApplicationFontFromData(data);
    if (id >= 0)
        g_fontIds << id;
    return id;
}

QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

void loadFonts()
{
    if (!g_family.isEmpty())
        return;
    g_loadedScaled = isScaled();
    // The bundled font: Wine's Tahoma with Windows line metrics, family "OctoMX Tahoma". [qa-1] A copy in
    // a "fonts" folder of the installation (Windows: next to octomx.exe; Linux bundle: <dest>/fonts with
    // the program in <dest>/bin) is preferred, so users can inspect / replace the LGPL font; the copy
    // compiled into the program is the fallback. The shipped files are byte-identical to the embedded ones.
    QString dir;
    for (const QString &d : fontSearchDirs())
        if (QFile::exists(d + QStringLiteral("/tahoma.ttf"))) {
            dir = d;
            break;
        }
    const QString regPath = dir.isEmpty() ? QStringLiteral(":/mx/fonts/tahoma.ttf") : dir + QStringLiteral("/tahoma.ttf");
    QString boldPath = dir.isEmpty() ? QStringLiteral(":/mx/fonts/tahomabd.ttf") : dir + QStringLiteral("/tahomabd.ttf");
    if (!QFile::exists(boldPath))
        boldPath = QStringLiteral(":/mx/fonts/tahomabd.ttf");
    QByteArray reg = readFile(regPath), bold = readFile(boldPath);
    if (reg.isEmpty() && !dir.isEmpty()) {   // unreadable copy: the embedded one
        reg = readFile(QStringLiteral(":/mx/fonts/tahoma.ttf"));
        dir.clear();
    }
    int id = addFont(reg);
    int boldId = addFont(bold);
    QString bundled;
    if (id >= 0 && !QFontDatabase::applicationFontFamilies(id).isEmpty())
        bundled = QFontDatabase::applicationFontFamilies(id).first();
    g_fontSource = dir.isEmpty() ? QStringLiteral(":/mx/fonts") : dir;
    // other fonts of that folder (e.g. a chat font) are offered too
    if (!dir.isEmpty())
        for (const QString &f : QDir(dir).entryList({QStringLiteral("*.ttf"), QStringLiteral("*.otf")}, QDir::Files))
            if (f.compare(QLatin1String("tahoma.ttf"), Qt::CaseInsensitive) != 0 &&
                f.compare(QLatin1String("tahomabd.ttf"), Qt::CaseInsensitive) != 0)
                QFontDatabase::addApplicationFont(dir + QLatin1Char('/') + f);
    if (!bundled.isEmpty()) {
        QFont probe(bundled);
        probe.setPixelSize(11);
        probe.setStyleStrategy(QFont::NoAntialias);
        if (QFontMetrics(probe).leading() < 0) {
            QFontDatabase::removeApplicationFont(id);
            if (boldId >= 0)
                QFontDatabase::removeApplicationFont(boldId);
            reg = withLineGap(reg, 40);
            bold = withLineGap(bold, 40);
            id = addFont(reg);
            boldId = addFont(bold);
            if (id >= 0 && !QFontDatabase::applicationFontFamilies(id).isEmpty())
                bundled = QFontDatabase::applicationFontFamilies(id).first();
        }
    }
    // [ui-e] above 100 %: the same faces without their bitmap strikes (see withoutBitmaps). The classic
    // 11-px text width is measured first, so fontPx() can size the outlines against the real 100 % look.
    if (!bundled.isEmpty() && isScaled()) {
        auto sampleWidth = [&bundled] {
            QFont f(bundled);
            f.setPixelSize(11);
            f.setStyleStrategy(QFont::StyleStrategy(QFont::NoAntialias | QFont::PreferMatch));
            f.setHintingPreference(QFont::PreferFullHinting);
            f.setKerning(false);
            return QFontMetrics(f).horizontalAdvance(fontSampleText());
        };
        const int classic = sampleWidth();
        QFontDatabase::removeApplicationFont(id);
        if (boldId >= 0)
            QFontDatabase::removeApplicationFont(boldId);
        // Wine's Tahoma Bold has the regular outlines with bold advances (its bold weight lives in the
        // strikes): without strikes the regular face is emboldened by the font engine instead
        id = addFont(withoutBitmaps(reg));
        boldId = -1;
        if (id >= 0 && !QFontDatabase::applicationFontFamilies(id).isEmpty())
            bundled = QFontDatabase::applicationFontFamilies(id).first();
        const int outline = sampleWidth();
        if (classic > 0 && outline > 0)
            g_classicWidthFactor = double(classic) / outline;
    }
#ifdef Q_OS_WIN
    // Prefer the system Tahoma on Windows, but only the genuine one: Microsoft's Tahoma 8 pt has an
    // 11-px ascent; Wine's copy reports 9 (embedded-bitmap metrics) and would shift all text up.
    if (QFontDatabase::families().contains(QStringLiteral("Tahoma"))) {
        QFont t(QStringLiteral("Tahoma"));
        t.setPixelSize(11);
        t.setStyleStrategy(QFont::NoAntialias);
        if (QFontMetrics(t).ascent() >= 11 || bundled.isEmpty()) {
            g_family = QStringLiteral("Tahoma");
            g_fontSource = QStringLiteral("system");
            return;
        }
    }
#endif
    if (!bundled.isEmpty()) {
        g_family = bundled;
        return;
    }
    g_family = QFontDatabase::families().contains(QStringLiteral("Tahoma")) ? QStringLiteral("Tahoma") : QStringLiteral("Sans Serif");
    g_fontSource = QStringLiteral("system");
}
} // namespace

void preApplicationSetup()
{
    // [ui-e] The kit scales itself (ui/scale, kit/scale.h): Qt's high-DPI scaling is off so that one
    // logical pixel is one device pixel at every scale (no 1/2-px alternating lines at 150 %), and fonts
    // / style metrics assume 96 dpi (the kit picks pixel sizes). The process stays per-monitor DPI aware
    // on Windows (Qt's default + the manifest), so Windows does not stretch the window and the screen's
    // logical DPI tells "Automatic" the system scale setting. QT_SCALE_FACTOR is still honoured.
    if (!qEnvironmentVariableIsSet("QT_ENABLE_HIGHDPI_SCALING"))
        qputenv("QT_ENABLE_HIGHDPI_SCALING", "0");
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::Floor);
    QCoreApplication::setAttribute(Qt::AA_Use96Dpi);
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
#ifdef Q_OS_MACOS
    // [mac-1] the classic look on a Mac: message boxes and file dialogs are our own classic ones, not the
    // native Aqua panels (Qt 6 shows QMessageBox as a native alert on macOS otherwise). Core Text draws
    // the bundled Tahoma with its bitmap strikes, unsmoothed (QFont::NoAntialias), like Windows; Qt's
    // FreeType engine on macOS ignores the strikes (tst_qa1_fidelity glyphsOneAndJ), so it is not used.
    // Retina screens: Qt draws at 2x (= 200 %).
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QCoreApplication::setAttribute(Qt::AA_DontShowIconsInMenus, false);
#endif
}

QStringList fontSearchDirs()
{
    QStringList out;
    const QByteArray env = qgetenv("MX_FONT_DIR");
    if (!env.isEmpty())
        out << QString::fromLocal8Bit(env);
    if (QCoreApplication::instance()) {
        const QString app = QCoreApplication::applicationDirPath();
        out << app + QStringLiteral("/fonts") << QDir::cleanPath(app + QStringLiteral("/../fonts"))
            << QDir::cleanPath(app + QStringLiteral("/../share/octomx/fonts"))
            << QDir::cleanPath(app + QStringLiteral("/../Resources/fonts"));   // [mac-1] OctoMX.app/Contents/Resources
    }
    return out;
}

QString fontSampleText()
{
    return QStringLiteral("Search Transfers Networks Shared Files 0123456789 kbps MB/s "
                          "The quick brown fox jumps over the lazy dog");
}

double classicWidthFactor()
{
    loadFonts();
    return g_classicWidthFactor;
}

QString uiFontSource()
{
    loadFonts();
    return g_fontSource;
}

QString uiFontFamily()
{
    loadFonts();
    return g_family;
}

QFont classicFont(int pixelSize, bool bold)
{
    QFont f(uiFontFamily());
    // [ui-e] `pixelSize` is in design pixels (11 = Tahoma 8 pt at 100 %); fontPx() scales it
    f.setPixelSize(fontPx(pixelSize));
    f.setBold(bold);
    f.setStyleStrategy(QFont::StyleStrategy((smoothFonts() ? QFont::PreferAntialias : QFont::NoAntialias) | QFont::PreferMatch));
    // [ui-e] smooth (antialiased) text above 100 %: vertical hinting only, so the advances keep their
    // designed proportions (full hinting spaced the bold face out at 18 px)
    f.setHintingPreference(smoothFonts() ? QFont::PreferVerticalHinting : QFont::PreferFullHinting);
    f.setKerning(false);
    f.setFeature(QFont::Tag("liga"), 0);   // the original WinMX (GDI) never draws ligatures ("fi")
    f.setFeature(QFont::Tag("clig"), 0);
    return f;
}

namespace {
QFont builtinNormal() { return classicFont(11); }
QFont builtinLarge() { return classicFont(12, true); }   // "Large font": news bar, headlines
} // namespace

QFont normalFont()
{
    Fonts *f = Fonts::instance();
    return f->m_ready ? f->m_normal : builtinNormal();
}
QFont boldFont()
{
    QFont f = normalFont();
    f.setBold(true);
    return f;
}
QFont dynamicFont()
{
    Fonts *f = Fonts::instance();
    return f->m_ready ? f->m_dynamic : builtinNormal();
}
QFont largeFont()
{
    Fonts *f = Fonts::instance();
    return f->m_ready ? f->m_large : builtinLarge();
}

// ------------------------------------------------------------------------------------- Fonts

Fonts *Fonts::instance()
{
    static Fonts *f = new Fonts(nullptr);
    return f;
}

bool Fonts::isFontKey(const QString &key)
{
    return key == QLatin1String("ui/fontNormal") || key == QLatin1String("ui/fontDynamic") || key == QLatin1String("ui/fontLarge");
}

QFont Fonts::parse(const QString &value, const QFont &fallback)
{
    const QString v = value.trimmed();
    if (v.isEmpty())
        return fallback;
    QString family;
    double pt = 0;
    bool bold = false, italic = false;
    const QStringList parts = v.split(QLatin1Char(','));
    if (parts.size() > 4) {   // QFont::toString()
        QFont q;
        if (!q.fromString(v))
            return fallback;
        family = q.family();
        pt = q.pointSizeF() > 0 ? q.pointSizeF() : q.pixelSize() * 72.0 / 96.0;
        bold = q.bold();
        italic = q.italic();
    } else {
        family = parts.value(0).trimmed();
        pt = parts.value(1).trimmed().toDouble();
        for (const QString &x : parts.mid(2)) {
            bold = bold || x.trimmed().compare(QLatin1String("bold"), Qt::CaseInsensitive) == 0;
            italic = italic || x.trimmed().compare(QLatin1String("italic"), Qt::CaseInsensitive) == 0;
        }
    }
    if (pt <= 0)
        pt = 8;
    // pixel sizes like everything else in the kit (96 dpi: 8 pt = 11 px)
    QFont f = classicFont(qBound(6, qRound(pt * 96.0 / 72.0), 72), bold);
    // "Tahoma" is the kit's Tahoma (the bundled one with Windows metrics unless the genuine one exists)
    if (!family.isEmpty() && family.compare(QLatin1String("Tahoma"), Qt::CaseInsensitive) != 0 &&
        family.compare(uiFontFamily(), Qt::CaseInsensitive) != 0)
        f.setFamily(family);
    f.setItalic(italic);
    return f;
}

void Fonts::attach(mx::Settings *s)
{
    if (m_conn)
        disconnect(m_conn);
    m_settings = s;
    if (s)
        m_conn = connect(s, &mx::Settings::changed, this, [this](const QString &key, const QVariant &) {
            if (isFontKey(key))
                reload();
        });
    reload();
}

void Fonts::reload()
{
    auto pick = [this](const char *key, const QFont &builtin) {
        if (!m_settings)
            return builtin;
        const QString k = QString::fromLatin1(key);
        const QString v = m_settings->value(k).toString();
        // the default value is the built-in face exactly (its metrics are tuned to the references)
        if (v.isEmpty() || v == m_settings->defaultValue(k).toString())
            return builtin;
        return parse(v, builtin);
    };
    const QFont oldNormal = m_normal, oldDynamic = m_dynamic, oldLarge = m_large;
    m_normal = pick("ui/fontNormal", builtinNormal());
    m_dynamic = pick("ui/fontDynamic", m_normal);
    m_large = pick("ui/fontLarge", builtinLarge());
    const bool first = !m_ready;
    m_ready = true;
    if (!first && oldNormal == m_normal && oldDynamic == m_dynamic && oldLarge == m_large)
        return;
    if (qApp) {
        QApplication::setFont(m_normal);
        QToolTip::setFont(m_normal);
        // menus get the normal font from the style; open ones and the cached ones follow here
        for (QWidget *w : QApplication::allWidgets())
            if (w->inherits("QMenu"))
                w->setFont(m_normal);
    }
    emit changed();
    if (qApp)
        for (QWidget *w : QApplication::allWidgets())
            w->update();
}

QPalette classicPalette()
{
    using namespace chrome;
    QPalette p;
    for (QPalette::ColorGroup g : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
        p.setColor(g, QPalette::Window, Face);
        p.setColor(g, QPalette::Button, Face);
        p.setColor(g, QPalette::Light, Light);
        p.setColor(g, QPalette::Midlight, Face);
        p.setColor(g, QPalette::Mid, Shadow);
        p.setColor(g, QPalette::Dark, Shadow);
        p.setColor(g, QPalette::Shadow, DarkShadow);
        p.setColor(g, QPalette::WindowText, Text);
        p.setColor(g, QPalette::ButtonText, Text);
        p.setColor(g, QPalette::Text, Text);
        p.setColor(g, QPalette::BrightText, Light);
        p.setColor(g, QPalette::Base, Light);
        p.setColor(g, QPalette::AlternateBase, Face);
        p.setColor(g, QPalette::Highlight, MenuHighlight);
        p.setColor(g, QPalette::HighlightedText, Light);
        p.setColor(g, QPalette::ToolTipBase, Info);
        p.setColor(g, QPalette::ToolTipText, Text);
        p.setColor(g, QPalette::Link, QColor(0x00, 0x00, 0xFF));
        p.setColor(g, QPalette::LinkVisited, QColor(0x80, 0x00, 0x80));
        p.setColor(g, QPalette::PlaceholderText, Shadow);
    }
    p.setColor(QPalette::Disabled, QPalette::WindowText, DisabledText);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, DisabledText);
    p.setColor(QPalette::Disabled, QPalette::Text, DisabledText);
    p.setColor(QPalette::Disabled, QPalette::Base, Face);
    p.setColor(QPalette::Inactive, QPalette::Highlight, MenuHighlight);
    return p;
}

void installClassicLook(QApplication *app)
{
    // [ui-e] the scale may have changed since the faces were loaded (Setup Wizard): load them again
    if (!g_family.isEmpty() && g_loadedScaled != isScaled()) {
        for (int id : std::as_const(g_fontIds))
            QFontDatabase::removeApplicationFont(id);
        g_fontIds.clear();
        g_family.clear();
        g_classicWidthFactor = 1.0;
    }
    loadFonts();
    Fonts::instance()->refresh();
    app->setStyle(new ClassicStyle);
    app->setPalette(classicPalette());
    app->setFont(normalFont());
    QToolTip::setPalette(classicPalette());
    QToolTip::setFont(normalFont());
    tips::installToolTipWrapper(app);   // [ui-g] long tips wrap at a readable width, below the widget
    app->setEffectEnabled(Qt::UI_AnimateMenu, false);
    app->setEffectEnabled(Qt::UI_FadeMenu, false);
    app->setEffectEnabled(Qt::UI_AnimateCombo, false);
    app->setEffectEnabled(Qt::UI_AnimateTooltip, false);
    app->setEffectEnabled(Qt::UI_FadeTooltip, false);
    installScreenGuard();   // [qa-2] dialogs stay on the screen
}

} // namespace mx::ui

// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: application-wide classic look: style, palette, fonts, high-DPI policy.
//
// Call order in main():
//   mx::ui::preApplicationSetup();      // before QApplication: high-DPI rounding policy
//   QApplication app(argc, argv);
//   mx::ui::installClassicLook(&app);   // style + palette + Tahoma 8pt (no antialiasing)
//
// Scaling decision ([ui-e], src/ui/README.md, kit/scale.h): Qt's high-DPI scaling is off; the kit
// scales geometry, lines, fonts and icons itself by the interface scale (ui/scale). Fonts are given
// in design pixels (11 px = Tahoma 8 pt at 100 %) so the layout never depends on the logical DPI.
#pragma once

#include <QtCore/QObject>
#include <QtGui/QFont>
#include <QtGui/QPalette>

class QApplication;

namespace mx {
class Settings;
}

namespace mx::ui {

void preApplicationSetup();
void installClassicLook(QApplication *app);

QPalette classicPalette();
// The three fonts of Settings ▸ Appearance (ui/fontNormal, ui/fontDynamic, ui/fontLarge; value
// "Family,points[,bold]"). At their defaults they are the exact built-in faces below.
// Normal: Tahoma 8 pt (11 px), no antialiasing, full hinting, chrome, buttons, menus, dialogs.
// [qa-1] where the UI font comes from: the first of fontSearchDirs() holding tahoma.ttf
// ($MX_FONT_DIR, <app>/fonts, <app>/../fonts, <app>/../share/octomx/fonts), else ":/mx/fonts" (embedded)
QStringList fontSearchDirs();
QString uiFontSource();
QFont normalFont();
QFont boldFont();
// Dynamic: the black list views (rows; their height follows the font). Default = normalFont().
QFont dynamicFont();
// The "large font" (news bar, Networks headline): Tahoma bold 12 px (Wine's 13-px bold strike is regular-weight).
QFont largeFont();

// Follows the font settings: on a change the application font, the tool-tip font and every
// widget that sets a font explicitly (lists, headlines) are updated live; `changed()` tells the rest.
class Fonts : public QObject {
    Q_OBJECT
public:
    static Fonts *instance();
    void attach(mx::Settings *s);   // nullptr = built-in fonts
    // "Tahoma,8" / "Arial,10,bold" -> a classic (non-antialiased, pixel-size) font
    static QFont parse(const QString &value, const QFont &fallback);
    static bool isFontKey(const QString &key);
    // [ui-e] rebuild the faces (after the interface scale changed)
    void refresh() { if (m_ready) reload(); }
    QFont normal() const { return m_normal; }
    QFont dynamic() const { return m_dynamic; }
    QFont large() const { return m_large; }
signals:
    void changed();

private:
    using QObject::QObject;
    void reload();
    mx::Settings *m_settings = nullptr;
    QMetaObject::Connection m_conn;
    QFont m_normal, m_dynamic, m_large;
    bool m_ready = false;
    friend QFont normalFont();
    friend QFont dynamicFont();
    friend QFont largeFont();
};
// The family actually used ("Tahoma" if available, else the bundled one, else a fallback).
QString uiFontFamily();
// Prepare a font for the classic look (pixel size, no AA, full hinting). [ui-e] `pixelSize` is in design
// pixels: above 100 % the face is fontPx(pixelSize) px, antialiased when smoothFonts() (kit/scale.h).
QFont classicFont(int pixelSize, bool bold = false);
// [ui-e] text used to compare font widths, and (above 100 %) the width of the classic 11-px face
// with its bitmap strike divided by the width of the outline-only face that is used instead
QString fontSampleText();
double classicWidthFactor();

} // namespace mx::ui

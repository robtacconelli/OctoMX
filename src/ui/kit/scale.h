// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: the interface scale (Settings ▸ Appearance "Interface size", key ui/scale). [ui-e]
//
// One mechanism for every scale: Qt's own high-DPI scaling is switched off (1 logical pixel = 1
// device pixel, see preApplicationSetup()), and the kit scales everything itself from the design
// pixels measured on the original WinMX references:
//   * geometry: S(px) / SR(x, y, w, h), rectangles scale their EDGES (x and x + w), so widgets that
//     touch at 100 % still touch at 150 % and a fixed grid stays a grid;
//   * 1-px chrome lines (bevels, frames, tree lines): uiLine(), the same width everywhere, so a
//     bevel is never 1 px on one side and 2 px on the other;
//   * fonts: fontPx(design px), the largest pixel size whose text is not wider than the scaled
//     layout (hinted fonts grow faster than the factor, which made fixed labels overflow);
//   * icons: drawn in code at the scaled size (icons.h), never stretched.
// At 1.0 every helper is the identity, so 100 % is pixel-identical to the classic layout.
#pragma once

#include <QtCore/QPoint>
#include <QtCore/QRect>
#include <QtCore/QSize>
#include <QtCore/QString>
#include <QtCore/QStringList>

class QScreen;
class QFont;

namespace mx {
class Settings;
}

namespace mx::ui {

// The factor in use (1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0). Set once at start-up, before any widget
// exists (applyUiScale); tests may call setUiScale() before they build widgets.
double uiScale();
void setUiScale(double factor);
inline bool isScaled() { return uiScale() != 1.0; }

// design pixels -> device pixels
int S(int designPx);
int S(double designPx);
QRect SR(int x, int y, int w, int h);
QRect SR(const QRect &r);
QSize SS(int w, int h);
inline QSize SS(const QSize &s) { return SS(s.width(), s.height()); }
QPoint SP(int x, int y);
// device pixels -> design pixels, rounded up (a measured text width used in a design-pixel layout)
int toDesign(int devicePx);
// width of a 1-px chrome line at this scale (1 up to 150 %, 2 at 175-250 %, 3 at 300 %)
int uiLine();
// pixel size of a font designed at `designPx` (11 = Tahoma 8 pt)
int fontPx(int designPx);

// [qa-2] above 100 % a caption whose text is wider than its box (fixed classic geometry, but the
// font grew a little faster than the box) gets a font up to 15 % smaller instead of being cut off;
// at 100 % (and when the text fits) the font is returned unchanged
QFont fitFont(const QFont &f, const QString &text, int width);

// antialiased text (ui/smoothFonts: Auto = on above 100 %)
bool smoothFonts();
void setSmoothFonts(bool on);

// ui/scale values: "auto", "100" ... "300"
QStringList scaleChoices();                        // "auto", "100", "125", ...
QString scaleChoiceLabel(const QString &choice);   // "Auto (150 %)" / "125 %"
// Auto: the screen's scale setting (logical DPI / 96) rounded to a choice; at 100 % OS scale on a
// screen at least 2560 px wide: 125 % (150 % from 3200 px). nullptr = primary screen.
double autoScale(const QScreen *screen = nullptr);
double scaleForChoice(const QString &choice, const QScreen *screen = nullptr);

// Reads ui/scale + ui/smoothFonts and sets the factor (call after the core's settings exist and
// before the main window is created). MX_UI_SCALE=<percent> overrides (screenshots, tests).
// [qa-2] The factor is never larger than maxScaleFor(primary screen); requestedUiScale() is the
// factor the setting asked for (larger when the screen was too small for it).
void applyUiScale(mx::Settings *settings);
double requestedUiScale();

// [qa-2] the largest choice at which the largest classic dialog (kLargestDialog design px, title bar
// included) and the main window's button band (kMainBandWidth: rows A / B, 682 px + borders) fit a
// work area of that size / the screen's work area. Automatic never exceeds it.
inline constexpr QSize kLargestDialog(577, 402);
inline constexpr int kMainBandWidth = 690;
double maxScaleForArea(const QSize &workArea);
double maxScaleFor(const QScreen *screen = nullptr);

// [qa-2] where a window with this frame must go so that it is on the work area (its title bar
// first when it is larger than the area); the frame's own position when it already fits.
QPoint placeOnScreen(const QRect &frame, const QRect &workArea);
// Keeps every dialog, message box and secondary window (Browse, Whois, floating pages ...) on the
// screen when it is shown: one centred on a main window near the screen's edge, or restored from
// another monitor, is moved onto the work area; a resizable one larger than the area is made to fit.
// The main window is left alone. installClassicLook() installs it.
void installScreenGuard();

} // namespace mx::ui

// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/kit/scale.h"

#include <QtCore/QHash>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtWidgets/QDialog>
#include <QtGui/QFont>
#include <QtGui/QFontMetrics>
#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <QtGui/QWindow>

#include <cmath>

#include "mx/services.h"
#include "mx/settings_keys.h"
#include "ui/kit/theme.h"

namespace mx::ui {

namespace {
double g_scale = 1.0;
int g_smooth = -1;   // -1 = automatic (on above 100 %)
QHash<int, int> g_fontPx;

int roundEdge(double v)
{
    // edges round half away from zero, the same way for every caller (qRound)
    return qRound(v);
}
} // namespace

double uiScale()
{
    return g_scale;
}

void setUiScale(double factor)
{
    if (!(factor > 0.5 && factor <= 4.0))
        factor = 1.0;
    g_scale = factor;
    g_fontPx.clear();
}

int S(int designPx)
{
    if (g_scale == 1.0)
        return designPx;
    const int v = roundEdge(designPx * g_scale);
    // a visible length never disappears
    if (designPx > 0 && v < 1)
        return 1;
    return v;
}

int S(double designPx)
{
    return roundEdge(designPx * g_scale);
}

QRect SR(int x, int y, int w, int h)
{
    if (g_scale == 1.0)
        return QRect(x, y, w, h);
    const int x0 = S(x), y0 = S(y);
    return QRect(x0, y0, S(x + w) - x0, S(y + h) - y0);
}

QRect SR(const QRect &r)
{
    return SR(r.x(), r.y(), r.width(), r.height());
}

QSize SS(int w, int h)
{
    return QSize(S(w), S(h));
}

QPoint SP(int x, int y)
{
    return QPoint(S(x), S(y));
}

int toDesign(int devicePx)
{
    if (g_scale == 1.0)
        return devicePx;
    return int(std::ceil(devicePx / g_scale - 1e-9));
}

int uiLine()
{
    // 1.0-1.5: 1, 1.75-2.5: 2, 3.0: 3 (a 1.5-px line cannot be drawn evenly; the thinner choice
    // keeps the classic light look)
    return qMax(1, int(std::floor(g_scale + 0.25)));
}

int fontPx(int designPx)
{
    if (g_scale == 1.0 || designPx <= 0)
        return designPx;
    auto it = g_fontPx.constFind(designPx);
    if (it != g_fontPx.constEnd())
        return *it;
    int n = qMax(designPx, qRound(designPx * g_scale));
    if (QGuiApplication::instance()) {
        // Hinted glyphs grow faster than the size (16-px Tahoma is ~6 % wider than 1.5 × 11 px), so the
        // text would overflow controls whose width scales with the factor: take the largest size whose
        // sample text stays within the scaled width (+2 %).
        const QString sample = fontSampleText();
        auto width = [&sample](int px, bool smooth) {
            QFont f(uiFontFamily());
            f.setPixelSize(px);
            f.setStyleStrategy(QFont::StyleStrategy((smooth ? QFont::PreferAntialias : QFont::NoAntialias) | QFont::PreferMatch));
            f.setHintingPreference(smooth ? QFont::PreferVerticalHinting : QFont::PreferFullHinting);
            f.setKerning(false);
            return QFontMetrics(f).horizontalAdvance(sample);
        };
        // reference: the classic 100 % face (no antialiasing)
        const double target = width(designPx, false) * classicWidthFactor() * g_scale * 1.02;
        const int floorPx = qMax(designPx, int(std::floor(designPx * g_scale)) - 1);
        if (qEnvironmentVariableIsSet("MX_FONT_DEBUG"))
            for (int c = floorPx; c <= qRound(designPx * g_scale) + 1; ++c)
                qInfo("fontPx(%d) at %.2f: %d px -> %d (target %.0f)", designPx, g_scale, c, width(c, smoothFonts()), target);
        while (n > floorPx && width(n, smoothFonts()) > target)
            --n;
    }
    g_fontPx.insert(designPx, n);
    return n;
}

QFont fitFont(const QFont &f, const QString &text, int width)
{
    if (g_scale == 1.0 || text.isEmpty() || width <= 0 || f.pixelSize() <= 0)
        return f;
    if (QFontMetrics(f).horizontalAdvance(text) <= width)
        return f;
    QFont g = f;
    const int px = f.pixelSize(), floorPx = qMax(1, int(std::ceil(px * 0.85)));
    for (int n = px - 1; n >= floorPx; --n) {
        g.setPixelSize(n);
        if (QFontMetrics(g).horizontalAdvance(text) <= width)
            return g;
    }
    return g;   // the smallest allowed; what is still too wide is cut as before
}

bool smoothFonts()
{
    return g_smooth < 0 ? g_scale > 1.0 : g_smooth > 0;
}

void setSmoothFonts(bool on)
{
    g_smooth = on ? 1 : 0;
    g_fontPx.clear();
}

QStringList scaleChoices()
{
    return {QStringLiteral("auto"), QStringLiteral("100"), QStringLiteral("125"), QStringLiteral("150"),
            QStringLiteral("175"), QStringLiteral("200"), QStringLiteral("250"), QStringLiteral("300")};
}

QString scaleChoiceLabel(const QString &choice)
{
    if (choice == QLatin1String("auto"))
        return QObject::tr("Automatic (%1 %)").arg(qRound(autoScale() * 100));
    // [qa-2] a size this screen cannot hold says so (it is reduced at start)
    if (QGuiApplication::instance() && choice.toInt() / 100.0 > maxScaleFor(nullptr))
        return QObject::tr("%1 % (too large)").arg(choice);
    return QObject::tr("%1 %").arg(choice);
}

double maxScaleForArea(const QSize &workArea)
{
    // [qa-2] the largest classic dialogs (File Download 577 px wide, Setup Wizard 372 px + a title bar)
    // must fit the screen's work area, or their buttons end up off the screen (800x600 at 175 %), and
    // so must the main window's button band (else page buttons and Cancel / Clear are cut off)
    static const double steps[] = {3.0, 2.5, 2.0, 1.75, 1.5, 1.25};
    if (!workArea.isValid())
        return 3.0;
    for (double s : steps)
        if (std::ceil(qMax(kLargestDialog.width(), kMainBandWidth) * s) <= workArea.width() &&
            std::ceil(kLargestDialog.height() * s) <= workArea.height())
            return s;
    return 1.0;
}

double maxScaleFor(const QScreen *screen)
{
    if (!screen && QGuiApplication::instance())
        screen = QGuiApplication::primaryScreen();
    return screen ? maxScaleForArea(screen->availableGeometry().size()) : 3.0;
}

double autoScale(const QScreen *screen)
{
    if (!screen && QGuiApplication::instance())
        screen = QGuiApplication::primaryScreen();
    if (!screen)
        return 1.0;
    // Qt's scaling is off, so the logical DPI is the system's scale setting (96 = 100 %) and the
    // geometry is in device pixels
    const double os = screen->logicalDotsPerInch() / 96.0;
    static const double steps[] = {1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0};
    double pick = 1.0;
    if (os >= 1.125) {
        for (double s : steps)
            if (std::abs(s - os) < std::abs(pick - os))
                pick = s;
    } else {
        const int w = qMax(screen->geometry().width(), screen->geometry().height());
        if (w >= 3200)
            pick = 1.5;
        else if (w >= 2560)
            pick = 1.25;
    }
    return qMin(pick, maxScaleFor(screen));   // [qa-2] never larger than the screen
}

double scaleForChoice(const QString &choice, const QScreen *screen)
{
    bool ok = false;
    const int pct = choice.toInt(&ok);
    if (ok && pct >= 100 && pct <= 300)
        return pct / 100.0;
    return autoScale(screen);
}

namespace {
double g_requested = 1.0;
}

double requestedUiScale()
{
    return g_requested;
}

void applyUiScale(mx::Settings *settings)
{
    QString choice = settings ? settings->value(keys::ui::Scale).toString() : QString();
    const QByteArray env = qgetenv("MX_UI_SCALE");
    if (!env.isEmpty())
        choice = QString::fromLatin1(env);
    // [qa-2] a size the screen cannot hold (300 % on 800x600) is reduced to the largest that fits:
    // dialogs larger than the screen cannot be used (their buttons are off the screen)
    g_requested = scaleForChoice(choice);
    const double fit = maxScaleFor(nullptr);
    if (g_requested > fit)
        qInfo("octomx: interface scale %d %% does not fit the %dx%d work area: %d %% is used", qRound(g_requested * 100),
              QGuiApplication::primaryScreen()->availableGeometry().width(),
              QGuiApplication::primaryScreen()->availableGeometry().height(), qRound(fit * 100));
    setUiScale(qMin(g_requested, fit));
    if (QGuiApplication::instance())
        installScreenGuard();
    const QString smooth = settings ? settings->value(keys::ui::SmoothFonts).toString() : QString();
    if (smooth == QLatin1String("on"))
        g_smooth = 1;
    else if (smooth == QLatin1String("off"))
        g_smooth = 0;
    else
        g_smooth = -1;
    g_fontPx.clear();
}

// ---------------------------------------------------------------------------------- [qa-2] on screen

QPoint placeOnScreen(const QRect &frame, const QRect &workArea)
{
    if (!workArea.isValid() || workArea.contains(frame))
        return frame.topLeft();
    int x = frame.x(), y = frame.y();
    if (frame.right() > workArea.right())
        x = workArea.right() + 1 - frame.width();
    if (frame.bottom() > workArea.bottom())
        y = workArea.bottom() + 1 - frame.height();
    // larger than the area: the title bar and the left edge stay reachable
    x = qMax(x, workArea.left());
    y = qMax(y, workArea.top());
    return QPoint(x, y);
}

namespace {
// fits / moves one window onto its screen's work area; true when something was changed
bool keepOnScreen(QWidget *w)
{
    QScreen *sc = w->screen();
    if (w->parentWidget() && w->parentWidget()->window()->screen())
        sc = w->parentWidget()->window()->screen();
    if (!sc)
        return false;
    const QRect avail = sc->availableGeometry();
    QRect f = w->frameGeometry();
    bool changed = false;
    // a resizable window larger than the work area (Browse at 175 % on 1080p, a page floated from a
    // maximised window) is made to fit; a fixed one is only moved
    const QSize extra = f.size() - w->size();
    if (w->minimumSize() != w->maximumSize() && (f.width() > avail.width() || f.height() > avail.height())) {
        const QSize want = w->size().boundedTo(avail.size() - extra).expandedTo(w->minimumSize());
        if (want != w->size()) {
            w->resize(want);
            f = QRect(f.topLeft(), want + extra);
            changed = true;
        }
    }
    const QPoint to = placeOnScreen(f, avail);
    if (qEnvironmentVariableIsSet("MX_GUARD_DEBUG"))
        qInfo("screen guard: %s frame %d,%d %dx%d -> %d,%d", qPrintable(w->objectName()), f.x(), f.y(), f.width(), f.height(),
              to.x(), to.y());
    if (to != f.topLeft()) {
        w->move(w->pos() + (to - f.topLeft()));   // pos() and move() agree on frame / client
        changed = true;
    }
    return changed;
}

class ScreenGuard : public QObject {
public:
    using QObject::QObject;
    bool eventFilter(QObject *o, QEvent *e) override
    {
        if (e->type() == QEvent::Show && o->isWidgetType()) {
            auto *w = static_cast<QWidget *>(o);
            const Qt::WindowType type = w->windowType();
            // dialogs and the secondary windows (Browse, Whois, private messages, floating pages);
            // not menus / tool tips, and not the main window (its size is the user's)
            const bool secondary = (type == Qt::Window || type == Qt::Dialog || type == Qt::Tool) &&
                                   !w->inherits("mx::ui::MainWindow");
            if (w->isWindow() && secondary && !w->isMaximized() && !w->isFullScreen()) {
                keepOnScreen(w);
                // once more when the window system has placed it and its frame is known
                QPointer<QWidget> p(w);
                QTimer::singleShot(0, w, [p] {
                    if (p && p->isVisible() && !p->isMaximized())
                        keepOnScreen(p);
                });
            }
        }
        return false;
    }
};
} // namespace

void installScreenGuard()
{
    static QPointer<ScreenGuard> guard;
    if (!guard && QCoreApplication::instance()) {
        guard = new ScreenGuard(QCoreApplication::instance());
        QCoreApplication::instance()->installEventFilter(guard);
    }
}

} // namespace mx::ui

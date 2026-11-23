// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/kit/icons.h"

#include "ui/kit/scale.h"

#include <QtCore/QHash>
#include <QtCore/QtMath>
#include <QtGui/QPainter>
#include <QtGui/QPolygon>

#include <cmath>
#include <functional>

// [ui-e] Original pixel art in the style of the classic Windows 2000 / WinMX toolbar (16-colour VGA
// palette, black outlines, 16×16), drawn procedurally so every icon is drawn again, not stretched,
// at the interface scale. Coordinates are on the 16-unit design grid; the canvas maps each unit to
// `size / 16` device pixels with consistent edges, and draws every outline `t` device pixels wide
// (1 at 16 px, 2 at 24-40 px, 3 from 44 px). Areas (fill / disc / polygon) are painted first, outlines
// and details on top, so a thicker outline never leaves a gap.

namespace mx::ui {

namespace {

constexpr QRgb K = 0xff000000, W = 0xffffffff, G = 0xff808080, SL = 0xffc0c0c0, T = 0xff008080,
               LG = 0xff00ff00, DG = 0xff008000, R = 0xffff0000, MR = 0xff800000, Y = 0xffffff00,
               O = 0xff808000, B = 0xff0000ff, N = 0xff000080, C = 0xff00ffff;

int g_size = 16;   // the size the icon being drawn is drawn at (see draw())

class Canvas {
public:
    explicit Canvas(int size = g_size) : img(size, size, QImage::Format_ARGB32), k(size / 16.0)
    {
        img.fill(Qt::transparent);
        t = size < 22 ? 1 : size < 44 ? 2 : qMax(3, int(std::lround(k)));
    }
    QImage img;
    double k;
    int t;

    // ---- areas (layer 0)
    void fill(double x0, double y0, double x1, double y1, QRgb c) { area([=](QPainter &p) { p.fillRect(cells(x0, y0, x1, y1), QColor::fromRgba(c)); }); }
    void disc(double cx, double cy, double r, QRgb c)
    {
        area([=](QPainter &) { forPixels([&](double gx, double gy, int x, int y) {
                                   if (std::hypot(gx - cx, gy - cy) <= r)
                                       img.setPixel(x, y, c);
                               }); });
    }
    // polygon through pixel-corner coordinates of the grid
    void poly(const QList<QPointF> &pts, QRgb c)
    {
        area([=](QPainter &p) {
            QPolygonF q;
            for (const QPointF &pt : pts)
                q << QPointF(pt.x() * k, pt.y() * k);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor::fromRgba(c));
            p.drawPolygon(q);
        });
    }

    // ---- outlines and details (layer 1, painted after the areas in their order)
    void polyOnTop(const QList<QPointF> &pts, QRgb c)
    {
        stroke([=](QPainter &p) {
            QPolygonF q;
            for (const QPointF &pt : pts)
                q << QPointF(pt.x() * k, pt.y() * k);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor::fromRgba(c));
            p.drawPolygon(q);
        });
    }
    void fillOnTop(double x0, double y0, double x1, double y1, QRgb c)
    {
        stroke([=](QPainter &p) { p.fillRect(cells(x0, y0, x1, y1), QColor::fromRgba(c)); });
    }
    // a line through the centres of grid cells, t pixels wide
    void line(double x0, double y0, double x1, double y1, QRgb c)
    {
        stroke([=](QPainter &p) {
            if (x0 == x1 || y0 == y1) {   // axis-aligned: exact cells, t thick
                const QRect a = cells(qMin(x0, x1), qMin(y0, y1), qMax(x0, x1), qMax(y0, y1));
                QRect r = a;
                if (y0 == y1)
                    r = QRect(a.left(), centre(y0) - t / 2, a.width(), t);
                else
                    r = QRect(centre(x0) - t / 2, a.top(), t, a.height());
                p.fillRect(r, QColor::fromRgba(c));
                return;
            }
            // diagonal: a run of t×t squares at the cell centres (pixel-art stairs)
            const int n = int(qMax(std::abs(x1 - x0), std::abs(y1 - y0)) * k);
            for (int i = 0; i <= n; ++i) {
                const double f = n ? double(i) / n : 0;
                const double gx = x0 + (x1 - x0) * f, gy = y0 + (y1 - y0) * f;
                p.fillRect(QRect(centre(gx) - t / 2, centre(gy) - t / 2, t, t), QColor::fromRgba(c));
            }
        });
    }
    void hline(double x0, double x1, double y, QRgb c) { line(x0, y, x1, y, c); }
    void vline(double x, double y0, double y1, QRgb c) { line(x, y0, x, y1, c); }
    // outline of the cell box x0..x1 × y0..y1, flush with its outer edges, t thick
    void frame(double x0, double y0, double x1, double y1, QRgb c)
    {
        stroke([=](QPainter &p) {
            const QRect r = cells(x0, y0, x1, y1);
            const QColor q = QColor::fromRgba(c);
            p.fillRect(QRect(r.left(), r.top(), r.width(), t), q);
            p.fillRect(QRect(r.left(), r.bottom() - t + 1, r.width(), t), q);
            p.fillRect(QRect(r.left(), r.top(), t, r.height()), q);
            p.fillRect(QRect(r.right() - t + 1, r.top(), t, r.height()), q);
        });
    }
    // a ring of radius r (grid units, to the middle of the line), t pixels wide
    void ring(double cx, double cy, double r, QRgb c, double widthUnits = 0)
    {
        stroke([=](QPainter &) {
            const double half = widthUnits > 0 ? widthUnits * k / 2.0 : t / 2.0;
            forPixels([&](double gx, double gy, int x, int y) {
                const double d = std::hypot(gx - cx, gy - cy) * k;
                if (std::abs(d - r * k) <= half)
                    img.setPixel(x, y, c);
            });
        });
    }
    // one grid cell (a detail pixel; a t×t block at larger sizes, centred in the cell)
    void dot(double x, double y, QRgb c)
    {
        stroke([=](QPainter &p) {
            const QRect a = cells(x, y, x, y);
            const int s = qMax(t, qMin(a.width(), a.height()));
            p.fillRect(QRect(a.left() + (a.width() - s) / 2, a.top() + (a.height() - s) / 2, s, s), QColor::fromRgba(c));
        });
    }
    // clear cells again (an opening cut into a ring drawn before)
    void erase(double x0, double y0, double x1, double y1)
    {
        stroke([=](QPainter &p) {
            p.save();
            p.setCompositionMode(QPainter::CompositionMode_Clear);
            p.fillRect(cells(x0, y0, x1, y1), Qt::transparent);
            p.restore();
        });
    }
    void outlinePoly(const QList<QPointF> &cellPts, QRgb c, bool closed = true)
    {
        for (int i = 0; i + 1 < cellPts.size() + (closed ? 1 : 0); ++i) {
            const QPointF a = cellPts.at(i), b = cellPts.at((i + 1) % cellPts.size());
            line(a.x(), a.y(), b.x(), b.y(), c);
        }
    }

    QImage render()
    {
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing, false);
        for (const auto &f : m_areas)
            f(p);
        for (const auto &f : m_strokes)
            f(p);
        p.end();
        return img;
    }

private:
    std::vector<std::function<void(QPainter &)>> m_areas, m_strokes;
    void area(std::function<void(QPainter &)> f) { m_areas.push_back(std::move(f)); }
    void stroke(std::function<void(QPainter &)> f) { m_strokes.push_back(std::move(f)); }
    int edge(double v) const { return int(std::lround(v * k)); }
    int centre(double cell) const { return (edge(cell) + edge(cell + 1)) / 2; }
    QRect cells(double x0, double y0, double x1, double y1) const
    {
        return QRect(QPoint(edge(x0), edge(y0)), QPoint(edge(x1 + 1) - 1, edge(y1 + 1) - 1));
    }
    template <class F> void forPixels(F f)
    {
        for (int y = 0; y < img.height(); ++y)
            for (int x = 0; x < img.width(); ++x)
                f((x + 0.5) / k, (y + 0.5) / k, x, y);
    }
};

// ---- individual icons --------------------------------------------------------------------------

// ---- the application icon: the OctoMX badge ------------------------------------------------------
// [logo-1] The round MX badge (silver rim with a light / dark bevel, black face, bright green pixel
// "MX" with a dark-green drop shadow) with a pink octopus behind the letters, hugging them: its head
// rises above the MX with two eyes looking over the letters, two arms come round from behind and wrap
// over the front of the M's left stem and the X's right leg, two more curl under the letters.
// From 48 px up the octopus is drawn with per-pixel shapes on the 16-unit grid of the badge (no
// anti-aliasing: hard pixel edges like the rest of the pixel art); the small sizes are hand-made pixel
// maps (16, 20, 24, 28, 32 and 40 px, each drawn at its own size and at the sizes up to the next one).

namespace logo {

constexpr QRgb PK = 0xfff2559f,   // main pink
    PH = 0xffff9fd0,              // highlight
    PS = 0xffb0246c,              // shadow side, underside of the arms
    SU = 0xffffd2ea,              // sucker dots
    BL = 0xffdc2f78;              // rosy cheeks

void rimAndFace(Canvas &c)
{
    c.disc(8, 8, 7.9, G);
    c.disc(7.6, 7.6, 7.2, W);
    c.disc(8, 8, 6.9, SL);
    c.disc(8, 8, 5.6, K);
}

void letters(Canvas &c, double dx, double dy, QRgb col)
{
    c.line(3 + dx, 6 + dy, 3 + dx, 10 + dy, col);
    c.line(4 + dx, 7 + dy, 5 + dx, 8 + dy, col);
    c.line(6 + dx, 7 + dy, 5 + dx, 8 + dy, col);
    c.line(7 + dx, 6 + dy, 7 + dx, 10 + dy, col);
    c.line(9 + dx, 6 + dy, 13 + dx, 10 + dy, col);
    c.line(13 + dx, 6 + dy, 9 + dx, 10 + dy, col);
}

// an arm: a tapering tube along a smooth curve (Catmull-Rom) through a few points of the grid
struct Arm {
    std::vector<QPointF> c;   // centre line samples
    std::vector<double> r;    // radius at each sample
    std::vector<double> s;    // arc fraction 0 ... 1 at each sample
    int under = 1;            // the underside (suckers): +1 left of the direction of travel, -1 right
    double front = 2.0;       // from this arc fraction on the arm lies in front of the letters
    QRectF box;               // bounding box
};

Arm makeArm(const QList<QPointF> &pts, double r0, double r1, int under, double front)
{
    Arm a;
    a.under = under;
    a.front = front;
    const int n = int(pts.size());
    for (int i = 0; i + 1 < n; ++i) {
        const QPointF p0 = pts.at(qMax(0, i - 1)), p1 = pts.at(i), p2 = pts.at(i + 1), p3 = pts.at(qMin(n - 1, i + 2));
        for (int j = 0; j < 16; ++j) {
            const double t = j / 16.0, t2 = t * t, t3 = t2 * t;
            a.c.push_back(0.5 * (2 * p1 + (p2 - p0) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2 + (3 * p1 - p0 - 3 * p2 + p3) * t3));
        }
    }
    a.c.push_back(pts.last());
    double len = 0;
    std::vector<double> acc{0};
    for (size_t i = 1; i < a.c.size(); ++i)
        acc.push_back(len += std::hypot(a.c[i].x() - a.c[i - 1].x(), a.c[i].y() - a.c[i - 1].y()));
    for (double l : acc) {
        const double f = len > 0 ? l / len : 0;
        a.s.push_back(f);
        a.r.push_back(r0 + (r1 - r0) * std::pow(f, 0.9));
    }
    a.box = QRectF(a.c.front(), QSizeF(0, 0));
    for (const QPointF &p : a.c)
        a.box |= QRectF(p.x() - r0, p.y() - r0, 2 * r0, 2 * r0);
    return a;
}

struct Hit {
    double d = 1e9;   // signed distance to the surface (negative inside)
    double u = 0;     // distance from the centre line / radius
    double side = 0;  // > 0: on the underside
    double s = 0;     // arc fraction
    double r = 0;     // radius there
};

Hit hitArm(const Arm &a, QPointF p)
{
    Hit h;
    for (size_t i = 0; i + 1 < a.c.size(); ++i) {
        const QPointF A = a.c[i], AB = a.c[i + 1] - A, AP = p - A;
        const double l2 = QPointF::dotProduct(AB, AB);
        const double t = l2 > 0 ? qBound(0.0, QPointF::dotProduct(AP, AB) / l2, 1.0) : 0;
        const QPointF v = AP - AB * t;
        const double dist = std::hypot(v.x(), v.y());
        const double r = a.r[i] + (a.r[i + 1] - a.r[i]) * t;
        if (dist - r < h.d) {
            h.d = dist - r;
            h.u = r > 0 ? dist / r : 1;
            h.side = (AB.x() * v.y() - AB.y() * v.x()) * a.under;
            h.s = a.s[i] + (a.s[i + 1] - a.s[i]) * t;
            h.r = r;
        }
    }
    return h;
}

// calls f(point on the grid) for each pixel centre inside `box` (grid units) and sets the colour it
// returns (0: leave the pixel)
template <class F> void paint(QImage &img, const QRectF &box, F f)
{
    const double k = img.width() / 16.0;
    const int x0 = qMax(0, int(std::floor(box.left() * k))), x1 = qMin(img.width() - 1, int(std::ceil(box.right() * k)));
    const int y0 = qMax(0, int(std::floor(box.top() * k))), y1 = qMin(img.height() - 1, int(std::ceil(box.bottom() * k)));
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            if (const QRgb c = f(QPointF((x + 0.5) / k, (y + 0.5) / k)))
                img.setPixel(x, y, c);
}

// an arm with a black outline `ow` wide, a darker underside, a highlight and suckers where there is
// room (frontOnly: just its part in front of the letters)
void paintArm(QImage &img, const Arm &a, bool frontOnly, double ow)
{
    const double k = img.width() / 16.0;
    paint(img, a.box, [&](QPointF p) -> QRgb {
        const Hit h = hitArm(a, p);
        if (h.d > 0 || (frontOnly && h.s < a.front))
            return 0;
        if (h.d > -qMax(qMin(ow, h.r * 0.45), 1 / k))   // the outline, thinner towards the tip
            return K;
        if (h.side > 0 && h.u > 0.45 && h.r * k >= 3)
            return PS;
        if (h.side < 0 && h.u > 0.3 && h.u < 0.7 && h.r * k >= 4)
            return PH;
        return PK;
    });
    std::vector<std::pair<QPointF, double>> dots;   // suckers: centre, radius
    double next = 0.12;
    for (size_t i = 0; i + 1 < a.c.size(); ++i) {
        if (a.s[i] < next || a.r[i] * k < 5 || (frontOnly && a.s[i] < a.front))
            continue;
        const QPointF T = a.c[i + 1] - a.c[i];
        const double tl = std::hypot(T.x(), T.y());
        if (tl <= 0)
            continue;
        const QPointF under = QPointF(-T.y(), T.x()) / tl * double(a.under);
        dots.push_back({a.c[i] + under * (a.r[i] * 0.5), a.r[i] * 0.3});
        next = a.s[i] + 0.11;
    }
    paint(img, a.box, [&](QPointF p) -> QRgb {
        for (const auto &d : dots)
            if (std::hypot(p.x() - d.first.x(), p.y() - d.first.y()) <= d.second)
                return SU;
        return 0;
    });
}

// approximate signed distance to an ellipse (negative inside)
double ellipseDist(QPointF p, QPointF c, double rx, double ry)
{
    const double dx = p.x() - c.x(), dy = p.y() - c.y();
    const double f = std::hypot(dx / rx, dy / ry);
    if (f <= 0)
        return -qMin(rx, ry);
    return (f - 1) * f / std::hypot(dx / (rx * rx), dy / (ry * ry));
}

QImage large(int size)
{
    Canvas base(size);
    rimAndFace(base);
    QImage img = base.render();
    const double k = size / 16.0;
    const double ow = qMax(1, int(std::lround(size / 56.0))) / k;   // outline width, grid units

    // the arms: one round the M's left stem, its mirror image (about the middle of the MX, x = 8.5)
    // round the X's right leg, two below the letters
    auto mirrored = [](QList<QPointF> pts) {
        for (QPointF &p : pts)
            p.setX(17.0 - p.x());
        return pts;
    };
    const QList<QPointF> side = {{3.6, 7.6}, {2.3, 8.3}, {1.9, 9.5}, {3.0, 10.3}, {4.6, 10.2}, {5.3, 9.7}, {5.0, 9.2}, {4.6, 9.5}};
    const int sideUnder = -1;
    const QList<QPointF> low = {{7.4, 11.4}, {6.6, 12.6}, {5.2, 13.2}, {4.2, 12.8}, {4.1, 12.1}, {4.7, 12.0}};
    const double fat = size < 64 ? 1.25 : 1.0;   // thicker arms where pixels are few
    const Arm left = makeArm(side, 0.85 * fat, 0.2 * fat, sideUnder, 0.3), right = makeArm(mirrored(side), 0.85 * fat, 0.2 * fat, -sideUnder, 0.3);
    const Arm lowL = makeArm(low, 0.8 * fat, 0.2 * fat, 1, 2.0), lowR = makeArm(mirrored(low), 0.8 * fat, 0.2 * fat, -1, 2.0);
    for (const Arm *a : {&lowL, &lowR, &left, &right})
        paintArm(img, *a, false, ow);

    // the head (mantle): outline, shadow on the lower right, a highlight on the upper left
    const QPointF hc(8.5, 4.15);
    const double hrx = 3.6, hry = 3.05;
    paint(img, QRectF(hc.x() - hrx, hc.y() - hry, 2 * hrx, 2 * hry), [&](QPointF p) -> QRgb {
        const double d = ellipseDist(p, hc, hrx, hry);
        if (d > 0)
            return 0;
        if (d > -ow)
            return K;
        if (ellipseDist(p, hc + QPointF(-0.6, -0.7), hrx, hry) > 0)
            return PS;
        if (ellipseDist(p, hc + QPointF(-1.5, -1.6), 1.1, 0.8) <= 0)
            return PH;
        return PK;
    });
    if (size >= 64)   // rosy cheeks
        for (double bx : {5.9, 11.1})
            paint(img, QRectF(bx - 0.5, 5.4, 1, 0.6), [&](QPointF p) -> QRgb { return ellipseDist(p, QPointF(bx, 5.7), 0.45, 0.25) <= 0 ? BL : 0; });
    // eyes: white, with black pupils (and an outline and a gleam where there is room); the right eye
    // is the mirror image of the left one, so both have the same pixels
    const bool roomy = size >= 96;
    const QPointF e(7.0, 5.0);
    for (bool rightEye : {false, true}) {
        const double ex = rightEye ? 17.0 - e.x() : e.x();
        paint(img, QRectF(ex - 1, e.y() - 1.1, 2, 2.2), [&](QPointF p) -> QRgb {
            const QPointF m = rightEye ? QPointF(17.0 - p.x(), p.y()) : p;
            const double d = ellipseDist(m, e, roomy ? 1.0 : 0.95, roomy ? 1.08 : 1.0);
            if (d > 0)
                return 0;
            if (roomy && d > -ow)
                return K;
            if (roomy && std::hypot(p.x() - (ex - 0.14), p.y() - (e.y() + 0.04)) <= 0.14)
                return W;   // the gleam: top left in both eyes
            if (ellipseDist(m, e + QPointF(0.0, 0.2), roomy ? 0.42 : 0.3, roomy ? 0.52 : 0.6) <= 0)
                return K;
            return W;
        });
    }

    // the letters over the octopus, then the arms' parts in front of the letters
    Canvas lc(size);
    letters(lc, 0, 1, DG);
    letters(lc, 0, 0, LG);
    QPainter p(&img);
    p.drawImage(0, 0, lc.render());
    p.end();
    for (const Arm *a : {&left, &right})
        paintArm(img, *a, true, ow);
    return img;
}

// the small sizes: hand-made pixel maps over the badge; a '.' keeps the badge's own rim and face, so
// the round outline stays smooth when a map is drawn at a size that is not its own (22, 36, 44 px).
// k black, w white, s silver, g grey, L green, d dark green, P pink, h highlight, S shadow pink,
// b cheeks.
const char *const map16[16] = {
    "................",
    ".......PPP......",
    "......PhPPS.....",
    ".....PhPPPPS....",
    ".....PwPPPwS....",
    ".....P.PPP.S....",
    "...L...L.L...L..",
    "...LL.LL.dL.Ld..",
    "...LdLdL..dLd...",
    "...L.d.L..LdL...",
    "...L...L.Ld.dL..",
    "...dP..d.d..Pd..",
    ".....PP...PP....",
    "................",
    "................",
    "................",
};
const char *const map20[20] = {
    "....................",
    "....................",
    "........PPPPS.......",
    ".......PhPPPPS......",
    "......PhPPPPPPS.....",
    "......PPwPPPwPS.....",
    "......PP.PPP.PS.....",
    "......PPPPPPPPS.....",
    "...LL...LL.LL...LL..",
    "...LLL.LLL.dLL.LLd..",
    "...LLdLdLL..dLLLd...",
    "...LL.d.LL...LLL....",
    "...LL...LL..LLdLL...",
    "...LL...LL.LLd.dLL..",
    "...ddP..dd.dd..Pdd..",
    "......PP.....PP.....",
    "....................",
    "....................",
    "....................",
    "....................",
};
const char *const map24[24] = {
    "........................",
    "........................",
    "..........PPPPP.........",
    ".........PhPPPPS........",
    "........PhhPPPPPS.......",
    ".......PwwwPPPwwwS......",
    ".......Pw.wPPPw.wS......",
    ".......Pw.wPPPw.wS......",
    ".......PPPPPPPPPPS......",
    "....LL.PPPLLPLLPPS.LL...",
    "....LLLPPLLLPLLLPSLLL...",
    "....LLLLLLLL.dLLLLLLd...",
    "...PLLdLLdLL..dLLLLd.P..",
    "..PSLL.dd.LL...LLLL..SP.",
    "..PPPPP...LL..LLLLPPPPP.",
    "...SSSP...LL.LLLddPSSS..",
    "....LL....LL.LLd..dLL...",
    "....dd....dd.dd....dd...",
    ".....P..PP.....PP..P....",
    "......PP.........PP.....",
    "........................",
    "........................",
    "........................",
    "........................",
};
const char *const map28[28] = {
    "............................",
    "............................",
    "...........PPPPPPPP.........",
    "..........PhhPPPPPPS........",
    ".........PhPPPPPPPPPS.......",
    ".........PhPPPPPPPPPS.......",
    ".........PPwwPPPPwwPS.......",
    ".........PwkkwPPwkkwS.......",
    ".........PwkkwPPwkkwS.......",
    ".........PPwwPPPPwwPS.......",
    ".........bbPPPPPPPPbb.......",
    ".....LL..PPPLLPPLLPPS..LL...",
    "....PLLL.PPLLLPPLLLPS.LLLP..",
    "...PSLLLL.LLLL..dLLL.LLLdSP.",
    "..PS.LLdLLLdLL..ddLLLLLdd.SP",
    "..PS.LLddLddLL...ddLLLdd..SP",
    "..PS.LL.ddd.LL....LLLLL...SP",
    "...PPPPP.d..LL...LLLdLPPPPP.",
    "...SSPPP....LL..LLLdddPPPSS.",
    ".....LL.S...LL..LLdd.SdLL...",
    ".....dd.....dd..ddd...ddd...",
    ".....dd.....dd..dd.....dd...",
    "...........PP....PP.........",
    ".......P.PPP......PPP.P.....",
    ".......PPPS........SPPP.....",
    ".........S..........S.......",
    "............................",
    "............................",
};
const char *const map32[32] = {
    "................................",
    "................................",
    "..............PPPPPP............",
    ".............PhhPPPPP...........",
    "............PhhPPPPPSP..........",
    "...........PhhPPPPPPPSP.........",
    "...........PhPPPPPPPPPS.........",
    "...........PPwwPPPPwwPS.........",
    "...........Pw..wPPw..wS.........",
    "...........Pw..wPPw..wS.........",
    "...........PPwwPPPPwwPS.........",
    "...........bbPPPPPPPPbb.........",
    "......LL...PPPLLPPLLPPS...LL....",
    "......LLL..PPLLLPPLLLPS..LLL....",
    ".....PLLLL..LLLL..dLLL..LLLdP...",
    "....PSLLdLLLLdLL..ddLLLLLLddSP..",
    "...PS.LLddLLddLL...ddLLLLdd..SP.",
    "...PS.LL.dddd.LL....dLLLLd...SP.",
    "...PS.LL..dd..LL....LLLLLL...SP.",
    "...PPPPPP.....LL...LLLddLPPPPPP.",
    "....SSPPPP....LL..LLLdddPPPPSS..",
    "......LL.S....LL..LLdd..SdLL....",
    "......dd......dd..ddd....ddd....",
    "......dd......dd..dd......dd....",
    "............PP......PP..........",
    "........P.PPP........PPP.P......",
    "........PPPS..........SPPP......",
    ".........SS............SS.......",
    "................................",
    "................................",
    "................................",
    "................................",
};
const char *const map40[40] = {
    "........................................",
    "........................................",
    "................PPPPPPPPPS..............",
    "...............PhhPPPPPPPPS.............",
    "..............PhhPPPPPPPPPPS............",
    ".............PhhPPPPPPPPPPPPS...........",
    ".............PhPPPPPPPPPPPPPS...........",
    ".............PhPPPPPPPPPPPPPS...........",
    ".............PPPPPPPPPPPPPPPS...........",
    ".............PPwwwwPPPPwwwwPS...........",
    ".............PwwwwwwPPwwwwwwS...........",
    ".............PwwkkwwPPwwkkwwS...........",
    ".............PwwkkwwPPwwkkwwS...........",
    ".............bbwkkwPPPPwkkwbb...........",
    ".............PPwwwwPPPPwwwwSS...........",
    ".......LLL...PPPPLLLPPLLLPPPS...LLL.....",
    ".......LLLL..PPPLLLLPPLLLLPPS..LLLL.....",
    ".....PPLLLLL...LLLLL..dLLLL...LLLLdPP...",
    "....PPSLLLLLL.LLLLLL..ddLLLL.LLLLddSPP..",
    "...PPS.LLLdLLLLLdLLL..dddLLLLLLLddd.SPP.",
    "...PPS.LLLddLLLddLLL...dddLLLLLddd..SPP.",
    "..PPS..LLLdddLdddLLL....dddLLLddd....SPP",
    "..PPS..LLL.ddddd.LLL.....dLLLLLd.....SPP",
    "..PPS..LLLPPddd..LLL.....LLLLLPP.....SPP",
    "...PPPPPPPPS.d...LLL....LLLLdLSPPPPPPPP.",
    "...PPPPPPPS......LLL...LLLLdddLSPPPPPPP.",
    "....SSSSSL.......LLL..LLLLdddddLLSSSSS..",
    ".......LLL.......LLL..LLLddd.dddLLL.....",
    ".......ddd.......ddd..ddddd...ddddd.....",
    ".......ddd.......ddd..dddd.....dddd.....",
    ".......ddd......PPdd..ddPP......ddd.....",
    "...........P..PPP........PPP..P.........",
    "...........PPPPPS........SPPPPP.........",
    "............SSSS..........SSSS..........",
    "........................................",
    "........................................",
    "........................................",
    "........................................",
    "........................................",
    "........................................",
};
QImage fromMap(int size, const char *const *rows, int n)
{
    Canvas c(size);
    rimAndFace(c);
    QImage img = c.render();
    QPainter p(&img);
    auto edge = [&](int v) { return int(std::lround(double(v) * size / n)); };
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            QRgb col = 0;
            switch (rows[y][x]) {
            case 'k': col = K; break;
            case 'w': col = W; break;
            case 's': col = SL; break;
            case 'g': col = G; break;
            case 'L': col = LG; break;
            case 'd': col = DG; break;
            case 'P': col = PK; break;
            case 'h': col = PH; break;
            case 'S': col = PS; break;
            case 'b': col = BL; break;
            default: continue;
            }
            p.fillRect(QRect(QPoint(edge(x), edge(y)), QPoint(edge(x + 1) - 1, edge(y + 1) - 1)), QColor::fromRgba(col));
        }
    p.end();
    return img;
}

} // namespace logo

QImage appImage(int size)
{
    // hand-made designs for the small sizes, the drawn one from 48 px (UI scale 300 %) up
    if (size < 20)
        return logo::fromMap(size, logo::map16, 16);
    if (size < 24)
        return logo::fromMap(size, logo::map20, 20);
    if (size < 28)
        return logo::fromMap(size, logo::map24, 24);
    if (size < 32)
        return logo::fromMap(size, logo::map28, 28);
    if (size < 40)
        return logo::fromMap(size, logo::map32, 32);
    if (size < 48)
        return logo::fromMap(size, logo::map40, 40);
    return logo::large(size);
}

QImage networks()
{
    // a monitor on a foot with a teal screen, a silver tower next to it
    Canvas c;
    c.fill(0, 1, 10, 9, SL);
    c.fill(2, 3, 8, 7, T);
    c.fill(3, 11, 7, 12, SL);
    c.fill(11, 2, 15, 14, SL);
    c.frame(0, 1, 10, 9, K);
    c.frame(2, 3, 8, 7, G);
    c.hline(1, 9, 2, W);
    c.dot(3, 4, C);
    c.dot(4, 4, C);
    c.vline(5, 10, 10, K);
    c.frame(3, 11, 7, 12, K);
    c.frame(11, 2, 15, 14, K);
    c.hline(12, 14, 4, G);
    c.hline(12, 14, 6, G);
    c.dot(12, 12, LG);
    c.hline(12, 14, 3, W);
    return c.render();
}

QImage sharedFiles()
{
    // an open yellow folder with a white sheet (blue lines) leaning out of it
    Canvas c;
    c.poly({{1, 3}, {6, 3}, {7, 4}, {13, 4}, {13, 9}, {1, 9}}, O);       // back of the folder
    c.poly({{4, 2}, {11, 0}, {13, 7}, {6, 9}}, W);                       // the sheet, tilted
    c.poly({{0, 7}, {12, 7}, {15, 15}, {2, 15}}, Y);                     // front flap
    c.outlinePoly({{4, 2}, {10, 0}, {12, 6}}, K, false);
    c.line(5, 4, 9, 3, B);
    c.line(6, 6, 10, 5, B);
    c.outlinePoly({{1, 3}, {5, 3}, {6, 4}, {3, 4}}, K, false);
    c.vline(1, 3, 6, K);
    c.vline(13, 4, 6, K);
    c.outlinePoly({{0, 7}, {12, 7}, {15, 14}, {2, 14}}, K);
    c.hline(1, 11, 8, W);
    c.hline(4, 13, 13, O);
    return c.render();
}

QImage chat(bool star = false)
{
    // two speech balloons: a silver one behind (upper right), a white one in front with navy dots
    Canvas c;
    c.fill(6, 0, 15, 5, SL);
    c.frame(6, 0, 15, 5, G);
    c.fill(1, 4, 12, 11, W);
    c.poly({{3, 11}, {6, 11}, {2, 15}}, W);
    c.frame(1, 4, 12, 11, K);
    c.line(3, 12, 2, 14, K);
    c.line(5, 12, 3, 14, K);
    c.hline(3, 5, 11, W);
    c.dot(4, 7, N);
    c.dot(6, 7, N);
    c.dot(8, 7, N);
    c.hline(3, 9, 9, B);
    if (star) {
        c.line(11, 11, 15, 15, Y);
        c.line(15, 11, 11, 15, Y);
        c.vline(13, 10, 15, Y);
    }
    return c.render();
}

void magnifier(Canvas &c, double ox, double oy, double scale = 1.0)
{
    // navy rim, pale cyan lens with a white glint, black handle with a silver band
    auto X = [&](double v) { return ox + v * scale; };
    auto Yc = [&](double v) { return oy + v * scale; };
    c.disc(X(5.5), Yc(5.5), 4.6 * scale, C);
    c.ring(X(5.5), Yc(5.5), 4.4 * scale, N);
    c.dot(X(3.6), Yc(3.4), W);
    c.dot(X(4.6), Yc(3.0), W);
    c.dot(X(3.0), Yc(4.4), W);
    c.line(X(9), Yc(9), X(14), Yc(14), K);
    c.line(X(10), Yc(9), X(14.5), Yc(13.5), K);
    c.line(X(9), Yc(10), X(13.5), Yc(14.5), K);
    c.dot(X(10.2), Yc(10.2), SL);
}

QImage search()
{
    Canvas c;
    magnifier(c, 0, 0);
    return c.render();
}

QImage openNewSearch()
{
    Canvas c;
    magnifier(c, 3, 3, 0.8);
    // a yellow star burst at the upper left
    c.vline(2, 0, 4, Y);
    c.hline(0, 4, 2, Y);
    c.dot(1, 1, O);
    c.dot(3, 3, O);
    return c.render();
}

QImage hotlist()
{
    // one tall flame (red outside, yellow core) over a white card with a navy band
    Canvas c;
    c.poly({{2, 10}, {3, 5}, {5, 7}, {7, 1}, {10, 6}, {12, 3}, {14, 10}}, R);
    c.poly({{5, 10}, {6, 7}, {8, 4}, {10, 8}, {11, 10}}, Y);
    c.outlinePoly({{2, 9}, {3, 5}, {5, 7}, {7, 1}, {10, 6}, {12, 3}, {13, 9}}, MR, false);
    c.fill(1, 10, 14, 15, W);
    c.frame(1, 10, 14, 15, K);
    c.hline(3, 12, 12, N);
    c.hline(3, 8, 13, G);
    return c.render();
}

QImage transfers()
{
    // a lime up arrow and a lime down arrow, outlined in dark green, side by side
    Canvas c;
    c.poly({{1, 6}, {4.5, 1}, {8, 6}}, LG);
    c.fill(4, 6, 4, 14, LG);
    c.poly({{8, 9}, {11.5, 15}, {15, 9}}, LG);
    c.fill(11, 1, 11, 9, LG);
    c.outlinePoly({{1, 5}, {4, 1}, {7, 5}}, DG, false);
    c.hline(1, 3, 5, DG);
    c.hline(5, 7, 5, DG);
    c.vline(3, 5, 14, DG);
    c.vline(5, 5, 14, DG);
    c.dot(4, 14, DG);
    c.outlinePoly({{8, 9}, {11, 14}, {14, 9}}, DG, false);
    c.hline(8, 10, 9, DG);
    c.hline(12, 14, 9, DG);
    c.vline(10, 1, 9, DG);
    c.vline(12, 1, 9, DG);
    c.dot(11, 1, DG);
    return c.render();
}

QImage bandwidth()
{
    // two stacked areas (maroon, blue-grey) under a blue rate line, black axes
    Canvas c;
    c.poly({{1, 15}, {1, 11}, {4, 9}, {7, 12}, {10, 7}, {13, 10}, {16, 8}, {16, 15}}, G);
    c.poly({{1, 15}, {1, 13}, {5, 12}, {9, 14}, {13, 12}, {16, 13}, {16, 15}}, MR);
    c.outlinePoly({{1, 10}, {4, 8}, {7, 11}, {10, 6}, {13, 9}, {15, 7}}, B, false);
    c.vline(0, 2, 15, K);
    c.hline(0, 15, 15, K);
    return c.render();
}

QImage ignored()
{
    // a red prohibition sign: white disc, red ring, a "\" bar
    Canvas c;
    c.disc(8, 8, 5.6, W);
    c.ring(8, 8, 6.0, R, 1.8);
    c.line(4, 4, 11, 11, R);
    c.line(5, 4, 12, 11, R);
    c.line(4, 5, 11, 12, MR);
    return c.render();
}

QImage settings()
{
    // a hammer standing up (silver head, wooden handle) and a screwdriver (red grip) next to it
    Canvas c;
    c.fill(1, 1, 8, 3, SL);              // hammer head
    c.fill(4, 4, 4, 14, O);              // handle: lit side
    c.fill(5, 4, 5, 14, MR);             //         shade
    c.fill(11, 9, 13, 15, R);            // screwdriver grip
    c.frame(1, 1, 8, 3, K);
    c.hline(2, 7, 1, W);
    c.dot(0, 2, K);                      // claw
    c.hline(4, 5, 15, K);
    c.vline(12, 2, 8, G);                // shaft
    c.dot(12, 1, K);
    c.frame(11, 9, 13, 15, MR);
    c.vline(11, 10, 14, 0xffff8080);
    return c.render();
}

QImage help()
{
    // a slim yellow "?" with a black right / bottom shadow and a white glint, on a round white tip
    Canvas c;
    auto glyph = [&](double dx, double dy, QRgb col) {
        c.fillOnTop(6 + dx, 2 + dy, 9 + dx, 2 + dy, col);     // top
        c.fillOnTop(4 + dx, 3 + dy, 5 + dx, 5 + dy, col);     // left shoulder
        c.fillOnTop(10 + dx, 3 + dy, 11 + dx, 6 + dy, col);   // right side
        c.fillOnTop(8 + dx, 7 + dy, 10 + dx, 7 + dy, col);    // turning in
        c.fillOnTop(7 + dx, 8 + dy, 8 + dx, 10 + dy, col);    // stem
        c.fillOnTop(7 + dx, 12 + dy, 8 + dx, 13 + dy, col);   // dot
        c.dot(5 + dx, 2 + dy, col);
        c.dot(10 + dx, 2 + dy, col);
        c.dot(9 + dx, 6 + dy, col);
    };
    glyph(1, 1, K);
    glyph(0, 0, Y);
    c.hline(6, 8, 2, W);
    c.dot(4, 3, W);
    return c.render();
}

QImage shareFolder()
{
    // a lime plus with a dark-green outline
    Canvas c;
    c.fill(6, 2, 9, 13, LG);
    c.fill(2, 6, 13, 9, LG);
    c.outlinePoly({{6, 2}, {9, 2}, {9, 6}, {13, 6}, {13, 9}, {9, 9}, {9, 13}, {6, 13}, {6, 9}, {2, 9}, {2, 6}, {6, 6}}, DG);
    c.hline(7, 8, 3, W);
    return c.render();
}

QImage unshareFolder()
{
    Canvas c;
    c.fill(2, 6, 13, 9, R);
    c.frame(2, 6, 13, 9, MR);
    c.hline(3, 12, 7, W);
    return c.render();
}

QImage refresh()
{
    // a lime arc (three quarters, black edges) with an arrow head at its open end
    Canvas c;
    c.ring(8, 8.5, 5.0, LG, 2.4);
    c.ring(8, 8.5, 6.4, K, 0.6);
    c.ring(8, 8.5, 3.6, K, 0.6);
    c.erase(9, 0, 15, 6);   // the gap at the upper right
    // the arrow head points clockwise into the gap
    c.polyOnTop({{7, 0}, {13, 3.5}, {7, 7}}, K);
    c.polyOnTop({{8, 1.3}, {11.2, 3.5}, {8, 5.7}}, LG);
    return c.render();
}

void page(Canvas &c)
{
    // a white sheet with a dog-ear
    c.poly({{2, 0}, {10, 0}, {14, 4}, {14, 16}, {2, 16}}, W);
    c.vline(2, 0, 15, K);
    c.hline(2, 9, 0, K);
    c.line(10, 0, 13, 3, K);
    c.vline(13, 4, 15, K);
    c.hline(2, 13, 15, K);
    c.vline(10, 1, 3, G);
    c.hline(10, 12, 3, G);
}

QImage sharedFileTypes()
{
    Canvas c;
    page(c);
    // a blue "?" on the sheet
    c.hline(6, 9, 5, B);
    c.vline(10, 6, 7, B);
    c.dot(9, 8, B);
    c.vline(8, 9, 10, B);
    c.dot(8, 12, B);
    return c.render();
}

QImage scanForMedia()
{
    Canvas c;
    page(c);
    c.hline(4, 11, 3, G);
    magnifier(c, 0, 6, 0.62);
    return c.render();
}

QImage searchTab()
{
    Canvas c;
    magnifier(c, 1, 1, 0.9);
    return c.render();
}

QImage channelTab()
{
    // a white tile with a black "#"
    Canvas c;
    c.fill(1, 1, 14, 14, W);
    c.frame(1, 1, 14, 14, G);
    c.vline(6, 3, 12, K);
    c.vline(9, 3, 12, K);
    c.hline(3, 12, 5, K);
    c.hline(3, 12, 10, K);
    return c.render();
}

QImage browse()
{
    // binoculars: two navy barrels joined by a bridge, cyan lens glints
    Canvas c;
    c.fill(1, 3, 6, 13, N);
    c.fill(9, 3, 14, 13, N);
    c.fill(6, 5, 9, 9, K);
    c.disc(3.5, 12, 3.0, N);
    c.disc(11.5, 12, 3.0, N);
    c.dot(2, 11, C);
    c.dot(10, 11, C);
    c.vline(2, 4, 8, B);
    c.vline(10, 4, 8, B);
    return c.render();
}

QImage folder()
{
    // a closed yellow folder (list rows)
    Canvas c;
    c.fill(1, 4, 14, 13, Y);
    c.fill(1, 2, 6, 3, Y);
    c.frame(1, 4, 14, 13, K);
    c.hline(1, 6, 2, K);
    c.vline(1, 2, 4, K);
    c.vline(6, 2, 3, K);
    c.hline(2, 13, 5, W);
    c.hline(2, 13, 12, O);
    return c.render();
}

QImage file()
{
    // a white page (list rows)
    Canvas c;
    c.fill(3, 1, 12, 14, W);
    c.frame(3, 1, 12, 14, K);
    c.dot(11, 2, G);
    c.hline(5, 10, 5, SL);
    c.hline(5, 10, 8, SL);
    return c.render();
}

QImage draw(IconId id, int size)
{
    // every icon is drawn on a 16-unit canvas of `size` pixels
    struct Scope {
        int old;
        explicit Scope(int s) : old(g_size) { g_size = qMax(8, s); }
        ~Scope() { g_size = old; }
    } scope(size);
    switch (id) {
    case IconId::App: return appImage(size);
    case IconId::Networks: return networks();
    case IconId::SharedFiles: return sharedFiles();
    case IconId::Chat: return chat(false);
    case IconId::Search: return search();
    case IconId::Hotlist: return hotlist();
    case IconId::Transfers: return transfers();
    case IconId::Bandwidth: return bandwidth();
    case IconId::Ignored: return ignored();
    case IconId::Settings: return settings();
    case IconId::Help: return help();
    case IconId::ShareFolder: return shareFolder();
    case IconId::UnshareFolder: return unshareFolder();
    case IconId::Refresh: return refresh();
    case IconId::SharedFileTypes: return sharedFileTypes();
    case IconId::ScanForMedia: return scanForMedia();
    case IconId::OpenNewSearch: return openNewSearch();
    case IconId::SearchTab: return searchTab();
    case IconId::ListChannels: return chat(true);
    case IconId::ChannelTab: return channelTab();
    case IconId::Browse: return browse();
    case IconId::Folder: return folder();
    case IconId::File: return file();
    }
    return QImage();
}

} // namespace

QImage iconImage(IconId id, int size)
{
    return draw(id, size);
}

QPixmap iconPixmap(IconId id)
{
    static QHash<int, QPixmap> cache;
    static int cachedSize = 0;
    const int size = S(16);
    if (cachedSize != size) {
        cache.clear();
        cachedSize = size;
    }
    auto it = cache.constFind(int(id));
    if (it != cache.constEnd())
        return *it;
    QPixmap pm = QPixmap::fromImage(iconImage(id, size));
    cache.insert(int(id), pm);
    return pm;
}

QIcon icon(IconId id)
{
    QIcon i;
    for (int s : {16, 24, 32, 48})
        i.addPixmap(QPixmap::fromImage(iconImage(id, s)));
    return i;
}

QIcon appIcon()
{
    // [logo-1] every size a window title bar or a tray asks for at 100 ... 300 %, so none is scaled
    QIcon i;
    for (int s : {16, 20, 22, 24, 28, 32, 40, 48, 64})
        i.addPixmap(QPixmap::fromImage(appImage(s)));
    return i;
}

QList<IconId> allIcons()
{
    QList<IconId> out;
    for (int i = int(IconId::App); i <= int(IconId::File); ++i)
        out << IconId(i);
    return out;
}

QString iconName(IconId id)
{
    static const char *const names[] = {"app", "networks", "sharedfiles", "chat", "search", "hotlist", "transfers",
                                        "bandwidth", "ignored", "settings", "help", "sharefolder", "unsharefolder",
                                        "refresh", "sharedfiletypes", "scanformedia", "opennewsearch", "searchtab",
                                        "listchannels", "channeltab", "browse", "folder", "file"};
    const int i = int(id);
    return i >= 0 && i < int(sizeof(names) / sizeof(names[0])) ? QString::fromLatin1(names[i]) : QString();
}

QPixmap informationPixmap(int px)
{
    QImage img(px, px, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    const double u = px / 32.0;   // drawn on a 32-unit grid
    QRadialGradient g(QPointF(12 * u, 10 * u), 20 * u);
    g.setColorAt(0, QColor(0x6c, 0xa8, 0xff));
    g.setColorAt(1, QColor(0x10, 0x48, 0xc8));
    p.setPen(QPen(QColor(0x08, 0x28, 0x80), qMax(1.0, 1.2 * u)));
    p.setBrush(g);
    p.drawEllipse(QRectF(2 * u, 2 * u, 28 * u, 28 * u));
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::white);
    p.drawEllipse(QRectF(13.6 * u, 6.5 * u, 4.8 * u, 4.8 * u));                 // the dot
    p.drawRoundedRect(QRectF(13.8 * u, 13 * u, 4.4 * u, 12.5 * u), 1 * u, 1 * u);   // the stem
    p.drawRect(QRectF(11.5 * u, 13 * u, 4 * u, 2.2 * u));                       // serif top left
    p.drawRect(QRectF(11.5 * u, 23.6 * u, 9 * u, 2.2 * u));                     // foot
    p.end();
    return QPixmap::fromImage(img);
}

} // namespace mx::ui

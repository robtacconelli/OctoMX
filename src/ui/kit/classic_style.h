// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: the classic Windows 2000 style on every platform.
// A QProxyStyle over Qt's built-in "Windows" style (which already draws classic bevels, radio
// buttons, check boxes and scrollbars) with a fixed palette and the details the original WinMX shows fixed up:
// push buttons (incl. the dithered "latched" state), menus (raised edge, navy selection), header
// sections and labels ("..." elision, embossed sort arrows), tooltips, group boxes, pixel metrics.
#pragma once

#include <QtWidgets/QProxyStyle>

class QStyleOptionMenuItem;

namespace mx::ui {

class ClassicStyle : public QProxyStyle {
    Q_OBJECT
public:
    ClassicStyle();

    void polish(QPalette &pal) override;
    void polish(QWidget *w) override;
    QPalette standardPalette() const override;

    int pixelMetric(PixelMetric m, const QStyleOption *opt = nullptr, const QWidget *w = nullptr) const override;
    int styleHint(StyleHint h, const QStyleOption *opt = nullptr, const QWidget *w = nullptr,
                  QStyleHintReturn *ret = nullptr) const override;
    void drawPrimitive(PrimitiveElement pe, const QStyleOption *opt, QPainter *p,
                       const QWidget *w = nullptr) const override;
    void drawControl(ControlElement ce, const QStyleOption *opt, QPainter *p,
                     const QWidget *w = nullptr) const override;
    void drawComplexControl(ComplexControl cc, const QStyleOptionComplex *opt, QPainter *p,
                            const QWidget *w = nullptr) const override;
    QSize sizeFromContents(ContentsType ct, const QStyleOption *opt, const QSize &size,
                           const QWidget *w = nullptr) const override;
    QRect subControlRect(ComplexControl cc, const QStyleOptionComplex *opt, SubControl sc,
                         const QWidget *w = nullptr) const override;
    QRect subElementRect(SubElement se, const QStyleOption *opt, const QWidget *w = nullptr) const override;

private:
    // [ui-e] menu items above 100 % interface scale
    QSize scaledMenuItemSize(const QStyleOptionMenuItem *mi, const QSize &contents) const;
    void drawScaledMenuItem(const QStyleOptionMenuItem *mi, QPainter *p, const QWidget *w) const;
};

} // namespace mx::ui

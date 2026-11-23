// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: the 20-px status bar (UI_SPEC §1.4): borderless text fields on the left,
// the "Float"/"Dock" sheet tab and the size grip on the right.
#pragma once

#include <QtWidgets/QWidget>

class QSizeGrip;

namespace mx::ui {

class FloatTab : public QWidget {
    Q_OBJECT
public:
    explicit FloatTab(QWidget *parent = nullptr);
    void setText(const QString &t);
    QString text() const { return m_text; }
    QSize sizeHint() const override { return QSize(38, 12); }

signals:
    void clicked();

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;

private:
    QString m_text;
};

class StatusBar : public QWidget {
    Q_OBJECT
public:
    explicit StatusBar(QWidget *parent = nullptr);
    void setFields(const QStringList &fields);
    QStringList fields() const { return m_fields; }
    QString text() const { return m_fields.join(QStringLiteral("  ")); }
    FloatTab *floatTab() const { return m_float; }
    void setSizeGripVisible(bool v);
    QSize sizeHint() const override { return QSize(200, 20); }

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;

private:
    QStringList m_fields;
    FloatTab *m_float;
    QSizeGrip *m_grip;
};

} // namespace mx::ui

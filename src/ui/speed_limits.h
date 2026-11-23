// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI [bw-1]: the speed limits one click away.
//
//   * speed::  helpers shared by the Bandwidth page row, the tray menu, the title and the Settings
//     pages: the limits in force (mx/bandwidth_limits.h), setting a limit (key + remembered value,
//     a change during a schedule window wins until the window ends), "2.5 MB" style input, presets
//     derived from the connection type and the last custom value, status texts;
//   * RateSpinBox: KB/s spin box that also accepts "2.5 MB", "800 KB", "1 MB/s";
//   * SpeedLimitRow: `Limit download [x] [value] KB/s   Limit upload [x] [value] KB/s` plus a note
//     (schedule in force ...) and a `Settings...` button, on top of the Bandwidth page;
//   * createSpeedLimitsMenu(): tray menu `Speed Limits >` (Speed Limits On toggle, Download and
//     Upload sub menus with Unlimited / presets / Other..., schedule state, Bandwidth Settings...).
// All of them write the same settings (bandwidth/downLimitKBs, upLimitKBs, lastDown/UpLimitKBs)
// and follow Settings::changed, so every place shows the same values at once.
#pragma once

#include <QtCore/QList>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QWidget>

#include <functional>

#include "mx/bandwidth_limits.h"

class QCheckBox;
class QLabel;
class QMenu;

namespace mx {
class Settings;
}

namespace mx::ui {

class MainWindow;
class ThinButton;

namespace speed {
// "500" / "500 KB" / "2.5 MB" / "2,5 MB/s" / "1 GB" / "64000 B" -> KB/s (1024-based); -1 = not a rate
int parseKBs(const QString &text);
QString kbsText(int kbs);                          // "500 KB/s", "2.5 MB/s", "Unlimited" for 0
bwlimit::InForce inForce(const Settings *s);
// the normal limit (0 = off) and the remembered value shown while it is off
int limitKBs(const Settings *s, bool down);
int lastKBs(const Settings *s, bool down);
void setLimit(Settings *s, bool down, int kbs);    // kbs 0 = unlimited (the remembered value stays)
void setEnabled(Settings *s, bool down, bool on);  // on = the remembered value
void setRemembered(Settings *s, bool down, int kbs);   // the value; also the limit if it is on
bool limitsOn(const Settings *s);                  // any limit in force
void setLimitsOn(Settings *s, bool on);            // both off (values kept) / back on
void resumeSchedule(Settings *s);
QList<int> presets(const Settings *s, bool down);  // KB/s, ascending, without 0
// "DL limit 500 KB/s, UL limit 50 KB/s" (+ why: schedule); empty when nothing is limited
QString statusText(const Settings *s);
QString scheduleNote(const Settings *s);           // "Schedule: 100 / 32 KB/s until 18:00" ...; empty without schedule
QString toolTip(bool down);
// "Other..." asks for a value: tests replace it (return KB/s, 0 = unlimited, -1 = cancelled)
using AskHook = std::function<int(QWidget *parent, bool down, int current)>;
extern AskHook s_askHook;
int askLimit(QWidget *parent, bool down, int current);
} // namespace speed

class RateSpinBox : public QSpinBox {
    Q_OBJECT
public:
    explicit RateSpinBox(QWidget *parent = nullptr);

protected:
    int valueFromText(const QString &text) const override;
    QString textFromValue(int v) const override;
    QValidator::State validate(QString &input, int &pos) const override;
};

class SpeedLimitRow : public QWidget {
    Q_OBJECT
public:
    SpeedLimitRow(Settings *s, MainWindow *mw, QWidget *parent = nullptr);
    static int designHeight() { return 22; }
    QCheckBox *downCheck() const { return m_downCheck; }
    QCheckBox *upCheck() const { return m_upCheck; }
    RateSpinBox *downSpin() const { return m_downSpin; }
    RateSpinBox *upSpin() const { return m_upSpin; }
    QLabel *note() const { return m_note; }
    void refresh();

protected:
    void resizeEvent(QResizeEvent *) override;
    void paintEvent(QPaintEvent *) override;

private:
    Settings *m_s;
    MainWindow *m_mw;
    QCheckBox *m_downCheck, *m_upCheck;
    RateSpinBox *m_downSpin, *m_upSpin;
    QLabel *m_downUnit, *m_upUnit, *m_note;
    ThinButton *m_settings;
};

// tray menu `Speed Limits >` (objectName speedLimitsMenu)
QMenu *createSpeedLimitsMenu(MainWindow *mw, QWidget *parent);

} // namespace mx::ui

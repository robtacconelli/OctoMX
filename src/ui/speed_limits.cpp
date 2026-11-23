// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/speed_limits.h"

#include <QtCore/QRegularExpression>
#include <QtGui/QPainter>
#include <QtGui/QActionGroup>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>

#include <cmath>

#include "mx/services.h"
#include "mx/settings_keys.h"
#include "ui/kit/colors.h"
#include "ui/kit/format.h"
#include "ui/kit/scale.h"
#include "ui/kit/theme.h"
#include "ui/kit/widgets.h"
#include "ui/main_window.h"

namespace mx::ui {

namespace speed {

AskHook s_askHook;

int parseKBs(const QString &text)
{
    QString t = text.trimmed().toLower();
    t.remove(QLatin1Char(' '));
    for (const char *suffix : {"persecond", "/sec", "/s", "ps"})
        if (t.endsWith(QLatin1String(suffix))) {
            t.chop(int(qstrlen(suffix)));
            break;
        }
    static const QRegularExpression re(QStringLiteral("^([0-9]+(?:[.,][0-9]*)?)(k|kb|kib|m|mb|mib|g|gb|gib|b|byte|bytes)?$"));
    const QRegularExpressionMatch m = re.match(t);
    if (!m.hasMatch())
        return -1;
    QString num = m.captured(1);
    num.replace(QLatin1Char(','), QLatin1Char('.'));
    if (num.endsWith(QLatin1Char('.')))
        num.chop(1);
    bool ok = false;
    const double v = num.toDouble(&ok);
    if (!ok || v < 0)
        return -1;
    const QString unit = m.captured(2);
    double kb = v;
    if (unit.startsWith(QLatin1Char('m')))
        kb = v * 1024;
    else if (unit.startsWith(QLatin1Char('g')))
        kb = v * 1024 * 1024;
    else if (unit.startsWith(QLatin1Char('b')))
        kb = v > 0 ? qMax(1.0, v / 1024) : 0;
    if (kb > 10000000)
        return -1;
    return int(std::lround(kb));
}

QString kbsText(int kbs)
{
    if (kbs <= 0)
        return QObject::tr("Unlimited");
    if (kbs < 1024)
        return QObject::tr("%1 KB/s").arg(kbs);
    const double mb = kbs / 1024.0;
    const bool whole = kbs % 1024 == 0;
    QString n = QString::number(mb, 'f', whole ? 0 : (mb < 10 ? 2 : 1));
    if (!whole)
        while (n.endsWith(QLatin1Char('0')))
            n.chop(1);
    if (n.endsWith(QLatin1Char('.')))
        n.chop(1);
    return QObject::tr("%1 MB/s").arg(n.replace(QLatin1Char('.'), fmt::decimalPoint()));
}

bwlimit::InForce inForce(const Settings *s)
{
    return bwlimit::inForce(s);
}

int limitKBs(const Settings *s, bool down)
{
    return qMax(0, s->i(down ? keys::bandwidth::DownLimitKBs : keys::bandwidth::UpLimitKBs));
}

int lastKBs(const Settings *s, bool down)
{
    const int v = qMax(1, s->i(down ? keys::bandwidth::LastDownLimitKBs : keys::bandwidth::LastUpLimitKBs));
    return down ? v : bwlimit::clampUp(v);
}

void setRemembered(Settings *s, bool down, int kbs)
{
    if (kbs <= 0)
        return;
    if (!down)
        kbs = bwlimit::clampUp(kbs);
    s->setValue(down ? keys::bandwidth::LastDownLimitKBs : keys::bandwidth::LastUpLimitKBs, kbs);
    if (limitKBs(s, down) > 0)
        s->setValue(down ? keys::bandwidth::DownLimitKBs : keys::bandwidth::UpLimitKBs, kbs);
    bwlimit::noteManualChange(s);
}

void setLimit(Settings *s, bool down, int kbs)
{
    if (kbs > 0) {
        if (!down)
            kbs = bwlimit::clampUp(kbs);
        s->setValue(down ? keys::bandwidth::LastDownLimitKBs : keys::bandwidth::LastUpLimitKBs, kbs);
    }
    s->setValue(down ? keys::bandwidth::DownLimitKBs : keys::bandwidth::UpLimitKBs, qMax(0, kbs));
    bwlimit::noteManualChange(s);   // also when the value did not change: the user chose it now
}

void setEnabled(Settings *s, bool down, bool on)
{
    setLimit(s, down, on ? lastKBs(s, down) : 0);
}

bool limitsOn(const Settings *s)
{
    const bwlimit::InForce f = bwlimit::inForce(s);
    return f.downKBs > 0 || f.upKBs > 0;
}

void setLimitsOn(Settings *s, bool on)
{
    if (!on) {
        const int mask = (limitKBs(s, true) > 0 ? 1 : 0) | (limitKBs(s, false) > 0 ? 2 : 0);
        if (mask)
            s->setValue(keys::bandwidth::LimitsOnMask, mask);
        setLimit(s, true, 0);
        setLimit(s, false, 0);
        return;
    }
    int mask = s->i(keys::bandwidth::LimitsOnMask);
    if (!(mask & 3))
        mask = 3;
    setLimit(s, true, mask & 1 ? lastKBs(s, true) : 0);
    setLimit(s, false, mask & 2 ? lastKBs(s, false) : 0);
}

void resumeSchedule(Settings *s)
{
    s->setValue(keys::bandwidth::ScheduleOverrideUntil, QString());
}

namespace {
// typical line speed (KB/s) per connection type: download, upload
QPair<int, int> typicalKBs(const QString &type)
{
    struct Row { const char *type; int down, up; };
    static const Row rows[] = {
        {"Unknown", 2000, 128},     {"DSL", 2000, 128},         {"4G", 4000, 1000},          {"VDSL", 6000, 2000},
        {"Satellite", 6000, 400},   {"FTTC", 8000, 2000},       {"Cable", 25000, 3000},      {"5G", 25000, 4000},
        {"FTTB", 60000, 12000},     {"FTTH", 60000, 30000},     {"2.5 Gbit", 150000, 60000}, {"5 Gbit", 300000, 120000},
        {"10 Gbit+", 600000, 240000},
    };
    for (const Row &r : rows)
        if (type == QLatin1String(r.type))
            return {r.down, r.up};
    return {2000, 128};
}

double niceValue(double v)
{
    const double mag = std::pow(10.0, std::floor(std::log10(v)));
    double best = mag;
    for (double m : {1.0, 1.5, 2.0, 2.5, 3.0, 4.0, 5.0, 6.0, 7.5, 8.0, 10.0})
        if (std::abs(m * mag - v) < std::abs(best - v))
            best = m * mag;
    return best;
}

// a round KB/s value; from 1 MB/s on round in MB/s (1.5 MB/s, not 1.46 MB/s)
int nice(double kbs)
{
    if (kbs <= 10)
        return 10;
    if (kbs < 1024)
        return int(niceValue(kbs));
    return int(std::lround(niceValue(kbs / 1024.0) * 1024));
}
} // namespace

QList<int> presets(const Settings *s, bool down)
{
    const QPair<int, int> t = typicalKBs(s->s(keys::identity::Connection));
    const int line = down ? t.first : t.second;
    QList<int> out;
    for (double f : {0.1, 0.25, 0.5, 0.75}) {
        int v = nice(line * f);
        if (!down)
            v = bwlimit::clampUp(v);
        if (!out.contains(v))
            out << v;
    }
    for (int v : {lastKBs(s, down), limitKBs(s, down)})
        if (v > 0 && !out.contains(v))
            out << v;
    std::sort(out.begin(), out.end());
    return out;
}

QString statusText(const Settings *s)
{
    const bwlimit::InForce f = bwlimit::inForce(s);
    QStringList parts;
    if (f.downKBs > 0)
        parts << QObject::tr("DL limit %1").arg(kbsText(f.downKBs));
    if (f.upKBs > 0)
        parts << QObject::tr("UL limit %1").arg(kbsText(f.upKBs));
    if (parts.isEmpty())
        return QString();
    QString t = parts.join(QStringLiteral(", "));
    if (f.source == bwlimit::InForce::Schedule)
        t += QObject::tr(" (schedule)");
    return t;
}

QString scheduleNote(const Settings *s)
{
    const bwlimit::InForce f = bwlimit::inForce(s);
    if (!f.scheduleEnabled)
        return QString();
    const QString at = f.until.isValid() ? QLocale::c().toString(f.until.time(), QStringLiteral("HH:mm")) : QString();
    const bool today = f.until.isValid() && f.until.date() == QDate::currentDate();
    const QString when = !f.until.isValid() ? QString()
                         : today ? at
                                 : QStringLiteral("%1 %2").arg(QLocale::c().dayName(f.until.date().dayOfWeek(), QLocale::ShortFormat), at);
    auto pair = [](int d, int u) { return QObject::tr("DL %1, UL %2").arg(kbsText(d), kbsText(u)); };
    switch (f.source) {
    case bwlimit::InForce::Schedule:
        return QObject::tr("Schedule in force until %1: %2").arg(when, pair(f.downKBs, f.upKBs));
    case bwlimit::InForce::Manual:
        return QObject::tr("Your limits win until %1, then the schedule applies again").arg(when);
    default:
        return when.isEmpty() ? QObject::tr("Schedule on, no day selected") : QObject::tr("Schedule starts %1").arg(when);
    }
}

QString toolTip(bool down)
{
    if (down)
        return QObject::tr("Limit the total download speed of all networks together (BitTorrent, eD2k, Kad, Gnutella, "
                           "HTTP, OctoMX). Unchecked = unlimited. Type KB/s, or for example \"2.5 MB\".");
    return QObject::tr("Limit the total upload speed of all networks together. Unchecked = unlimited. At least %1 KB/s: "
                       "BitTorrent and eD2k peers give download speed to those who upload, so a lower limit would "
                       "starve your downloads.")
        .arg(bwlimit::kMinUploadKBs);
}

int askLimit(QWidget *parent, bool down, int current)
{
    if (s_askHook)
        return s_askHook(parent, down, current);
    QInputDialog dlg(parent);
    dlg.setObjectName(QStringLiteral("speedLimitDialog"));
    dlg.setWindowTitle(down ? QObject::tr("Download Limit") : QObject::tr("Upload Limit"));
    dlg.setLabelText(down ? QObject::tr("Download limit for all networks (KB/s, \"2.5 MB\" works too, 0 = unlimited):")
                          : QObject::tr("Upload limit for all networks (KB/s, \"2.5 MB\" works too, 0 = unlimited, at least %1 KB/s):")
                                .arg(bwlimit::kMinUploadKBs));
    dlg.setTextValue(QString::number(current > 0 ? current : 0));
    for (;;) {
        if (dlg.exec() != QDialog::Accepted)
            return -1;
        const int v = parseKBs(dlg.textValue());
        if (v >= 0)
            return v;
        QMessageBox::warning(parent, QObject::tr("OctoMX"), QObject::tr("\"%1\" is not a speed. Type for example 500 or 2.5 MB.").arg(dlg.textValue()));
    }
}

} // namespace speed

// ------------------------------------------------------------------------------------ RateSpinBox

RateSpinBox::RateSpinBox(QWidget *parent) : QSpinBox(parent)
{
    setRange(1, 10000000);
    setKeyboardTracking(false);
    setAccelerated(true);
    setCorrectionMode(QAbstractSpinBox::CorrectToNearestValue);
}

int RateSpinBox::valueFromText(const QString &text) const
{
    const int v = speed::parseKBs(text);
    return v < 0 ? value() : qBound(minimum(), v, maximum());
}

QString RateSpinBox::textFromValue(int v) const
{
    return QString::number(v);
}

QValidator::State RateSpinBox::validate(QString &input, int &pos) const
{
    Q_UNUSED(pos)
    const QString t = input.trimmed();
    if (t.isEmpty())
        return QValidator::Intermediate;
    static const QRegularExpression partial(QStringLiteral("^[0-9]*([.,][0-9]*)?\\s*[kKmMgGbBiIyYtTeEsS/pP ]*$"));
    if (!partial.match(t).hasMatch())
        return QValidator::Invalid;
    const int v = speed::parseKBs(t);
    if (v < 0)
        return QValidator::Intermediate;
    return v >= minimum() && v <= maximum() ? QValidator::Acceptable : QValidator::Intermediate;
}

// ---------------------------------------------------------------------------------- SpeedLimitRow

SpeedLimitRow::SpeedLimitRow(Settings *s, MainWindow *mw, QWidget *parent) : QWidget(parent), m_s(s), m_mw(mw)
{
    setObjectName(QStringLiteral("speedLimitRow"));
    bwlimit::installOverrideRule(m_s);   // also with a core that does not (demo core)
    m_downCheck = new QCheckBox(tr("Limit download"), this);
    m_downCheck->setObjectName(QStringLiteral("limitDownCheck"));
    m_upCheck = new QCheckBox(tr("Limit upload"), this);
    m_upCheck->setObjectName(QStringLiteral("limitUpCheck"));
    m_downSpin = new RateSpinBox(this);
    m_downSpin->setObjectName(QStringLiteral("limitDownValue"));
    m_upSpin = new RateSpinBox(this);
    m_upSpin->setObjectName(QStringLiteral("limitUpValue"));
    m_upSpin->setMinimum(bwlimit::kMinUploadKBs);
    m_downUnit = new QLabel(tr("KB/s"), this);
    m_upUnit = new QLabel(tr("KB/s"), this);
    m_note = new QLabel(this);
    m_note->setObjectName(QStringLiteral("limitNote"));
    m_settings = new ThinButton(tr("Settings..."), this);
    m_settings->setObjectName(QStringLiteral("limitSettings"));
    m_settings->setCentered(true);
    m_settings->setToolTip(tr("Settings, Bandwidth Throttle: the same limits, the bandwidth schedule and whether LAN peers are limited"));
    for (QWidget *w : {static_cast<QWidget *>(m_downCheck), static_cast<QWidget *>(m_downSpin), static_cast<QWidget *>(m_downUnit)})
        w->setToolTip(speed::toolTip(true));
    for (QWidget *w : {static_cast<QWidget *>(m_upCheck), static_cast<QWidget *>(m_upSpin), static_cast<QWidget *>(m_upUnit)})
        w->setToolTip(speed::toolTip(false));

    connect(m_downCheck, &QCheckBox::toggled, this, [this](bool on) { speed::setEnabled(m_s, true, on); });
    connect(m_upCheck, &QCheckBox::toggled, this, [this](bool on) { speed::setEnabled(m_s, false, on); });
    connect(m_downSpin, &QSpinBox::valueChanged, this, [this](int v) { speed::setRemembered(m_s, true, v); });
    connect(m_upSpin, &QSpinBox::valueChanged, this, [this](int v) { speed::setRemembered(m_s, false, v); });
    connect(m_settings, &QAbstractButton::clicked, this, [this] {
        if (m_mw)
            m_mw->openSettings(QStringLiteral("throttle"));
    });
    connect(m_s, &Settings::changed, this, [this](const QString &key) {
        if (key.startsWith(QLatin1String("bandwidth/")) || key == QLatin1String(keys::identity::Connection))
            refresh();
    });
    connect(Colors::instance(), &Colors::changed, this, qOverload<>(&QWidget::update));
    refresh();
}

void SpeedLimitRow::refresh()
{
    const struct { QCheckBox *c; RateSpinBox *sp; bool down; } rows[] = {{m_downCheck, m_downSpin, true}, {m_upCheck, m_upSpin, false}};
    for (const auto &r : rows) {
        QSignalBlocker b1(r.c), b2(r.sp);
        r.c->setChecked(speed::limitKBs(m_s, r.down) > 0);
        if (!r.sp->hasFocus() || r.sp->value() != speed::lastKBs(m_s, r.down))
            r.sp->setValue(speed::lastKBs(m_s, r.down));
    }
    // why another limit is in force (schedule), or which one is
    const bwlimit::InForce f = speed::inForce(m_s);
    QString note = speed::scheduleNote(m_s);
    if (note.isEmpty() && !m_s->b(keys::bandwidth::LimitLan) && (f.downKBs > 0 || f.upKBs > 0))
        note = tr("LAN peers are not limited");
    m_note->setToolTip(note);
    m_note->setProperty("fullText", note);
    m_note->setText(fmt::elide(note, m_note->fontMetrics(), qMax(0, m_note->width())));
}

void SpeedLimitRow::resizeEvent(QResizeEvent *)
{
    // design pixels: check box, value, unit; twice; the note fills up to the Settings button
    const int h = S(designHeight());
    const int cy = (h - S(16)) / 2, sy = (h - S(20)) / 2;
    const QFontMetrics fm(m_downCheck->font());
    const int cw1 = S(18) + fm.horizontalAdvance(m_downCheck->text()) + S(4);
    const int cw2 = S(18) + fm.horizontalAdvance(m_upCheck->text()) + S(4);
    const int unitW = fm.horizontalAdvance(m_downUnit->text()) + S(6);
    int x = S(4);
    m_downCheck->setGeometry(x, cy, cw1, S(16));
    x += cw1;
    m_downSpin->setGeometry(x, sy, S(68), S(20));
    x += S(68) + S(4);
    m_downUnit->setGeometry(x, cy, unitW, S(16));
    x += unitW + S(14);
    m_upCheck->setGeometry(x, cy, cw2, S(16));
    x += cw2;
    m_upSpin->setGeometry(x, sy, S(68), S(20));
    x += S(68) + S(4);
    m_upUnit->setGeometry(x, cy, unitW, S(16));
    x += unitW + S(10);
    const int bw = fm.horizontalAdvance(m_settings->text()) + S(16);
    const int bx = width() - S(2) - bw;
    m_settings->setGeometry(bx, sy, bw, S(20));
    m_note->setGeometry(x, cy, qMax(0, bx - S(8) - x), S(16));
    m_note->setText(fmt::elide(m_note->property("fullText").toString(), m_note->fontMetrics(), m_note->width()));
}

void SpeedLimitRow::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), chrome::Face);
}

// ---------------------------------------------------------------------------------- tray menu

QMenu *createSpeedLimitsMenu(MainWindow *mw, QWidget *parent)
{
    Settings *s = mw->core()->settings();
    bwlimit::installOverrideRule(s);
    auto *m = new QMenu(QObject::tr("Speed Limits"), parent);
    m->setObjectName(QStringLiteral("speedLimitsMenu"));
    m->setToolTipsVisible(true);
    m->menuAction()->setToolTip(QObject::tr("Download and upload speed limits for all networks together"));

    QAction *toggle = m->addAction(QObject::tr("Speed Limits On"));
    toggle->setObjectName(QStringLiteral("speedLimitsOn"));
    toggle->setCheckable(true);
    toggle->setToolTip(QObject::tr("Switch both limits off (full speed) and back on; the values are kept"));
    QObject::connect(toggle, &QAction::triggered, mw, [s](bool on) { speed::setLimitsOn(s, on); });
    m->addSeparator();

    auto sub = [m](bool down) {
        auto *sm = m->addMenu(QString());
        sm->setObjectName(down ? QStringLiteral("speedLimitsDownMenu") : QStringLiteral("speedLimitsUpMenu"));
        sm->setToolTipsVisible(true);
        sm->menuAction()->setToolTip(speed::toolTip(down));
        return sm;
    };
    QMenu *downMenu = sub(true);
    QMenu *upMenu = sub(false);
    m->addSeparator();
    QAction *sched = m->addAction(QString());
    sched->setObjectName(QStringLiteral("speedLimitsSchedule"));
    sched->setEnabled(false);
    QAction *resume = m->addAction(QObject::tr("Resume Schedule"));
    resume->setObjectName(QStringLiteral("speedLimitsResume"));
    resume->setToolTip(QObject::tr("Use the scheduled limits again now"));
    QObject::connect(resume, &QAction::triggered, mw, [s] { speed::resumeSchedule(s); });
    QAction *settingsAct = m->addAction(QObject::tr("Bandwidth Settings..."));
    settingsAct->setObjectName(QStringLiteral("speedLimitsSettings"));
    settingsAct->setToolTip(QObject::tr("Settings, Bandwidth Throttle: limits, schedule, LAN peers"));
    QObject::connect(settingsAct, &QAction::triggered, mw, [mw] { mw->openSettings(QStringLiteral("throttle")); });

    auto fill = [s, mw](QMenu *sm, bool down) {
        sm->clear();
        const int cur = speed::limitKBs(s, down);
        const bwlimit::InForce f = speed::inForce(s);
        const int inForce = down ? f.downKBs : f.upKBs;
        sm->setTitle(QObject::tr("%1: %2").arg(down ? QObject::tr("Download") : QObject::tr("Upload"), speed::kbsText(inForce)));
        auto *group = new QActionGroup(sm);
        group->setExclusive(true);
        auto add = [&](const QString &text, int kbs, const QString &tip) {
            QAction *a = sm->addAction(text);
            a->setCheckable(true);
            a->setChecked(kbs == cur);
            a->setData(kbs);
            a->setToolTip(tip);
            group->addAction(a);
            QObject::connect(a, &QAction::triggered, mw, [s, down, kbs] { speed::setLimit(s, down, kbs); });
        };
        add(QObject::tr("Unlimited"), 0, QObject::tr("No limit"));
        sm->addSeparator();
        const int last = speed::lastKBs(s, down);
        for (int v : speed::presets(s, down))
            add(speed::kbsText(v), v,
                v == last ? QObject::tr("Your last value")
                          : QObject::tr("A preset for a %1 connection (Settings, Connection type)").arg(s->s(keys::identity::Connection)));
        sm->addSeparator();
        QAction *other = sm->addAction(QObject::tr("Other..."));
        other->setObjectName(down ? QStringLiteral("speedLimitsDownOther") : QStringLiteral("speedLimitsUpOther"));
        other->setToolTip(QObject::tr("Type a limit in KB/s (or for example 2.5 MB)"));
        QObject::connect(other, &QAction::triggered, mw, [s, mw, down] {
            const int v = speed::askLimit(mw, down, speed::limitKBs(s, down) > 0 ? speed::limitKBs(s, down) : speed::lastKBs(s, down));
            if (v >= 0)
                speed::setLimit(s, down, v);
        });
    };
    auto sync = [=] {
        QSignalBlocker b(toggle);
        toggle->setChecked(speed::limitsOn(s));
        fill(downMenu, true);
        fill(upMenu, false);
        const QString note = speed::scheduleNote(s);
        sched->setText(note);
        sched->setVisible(!note.isEmpty());
        resume->setVisible(speed::inForce(s).source == bwlimit::InForce::Manual);
    };
    sync();
    // rebuilt when shown (never from Settings::changed: that may run inside one of its own actions)
    QObject::connect(m, &QMenu::aboutToShow, m, sync);
    return m;
}

} // namespace mx::ui

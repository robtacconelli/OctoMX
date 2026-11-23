// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/kit/colors.h"

#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtGui/QStyleHints>

#include <cmath>

#include "mx/services.h"

namespace mx::ui {

namespace {
struct Def { const char *key; const char *rgb; };
// UI_SPEC §2 defaults ("Standard Dark Background / Light Text").
const Def kDefaults[] = {
    {"colors/listBackground", "#000000"},
    {"colors/listText", "#ffffff"},
    {"colors/listSelectedBackground", "#808080"},
    {"colors/listSelectedText", "#ffffff"},
    {"colors/listGrid", "#404040"},
    {"colors/listHeaderText", "#000000"},
    {"colors/partialText", "#ff8040"},
    {"colors/bitrateLow", "#ff0000"},
    {"colors/bitrateMedium", "#ffff00"},
    {"colors/bitrateHigh", "#00ff00"},
    {"colors/speedSlow", "#ff0000"},
    {"colors/speedMedium", "#ffff00"},
    {"colors/speedFast", "#00ff00"},
    {"colors/available", "#00ff00"},
    {"colors/unavailable", "#ffffff"},
    {"colors/pingGood", "#00ff00"},
    {"colors/pingMedium", "#ffff00"},
    {"colors/pingBad", "#ff0000"},
    {"colors/matchIncomplete", "#ffff00"},
    {"colors/canJoinDownload", "#80ffff"},
    {"colors/currentlyDownloading", "#004080"},
    {"colors/alreadyShared", "#808080"},
    {"colors/networkLabel", "#c0c0c0"},
    {"colors/transferActive", "#00ff00"},
    {"colors/transferQueued", "#4060ff"},
    {"colors/transferComplete", "#0080ff"},
    {"colors/transferError", "#ff0000"},
    {"colors/transferPaused", "#ffffff"},
    {"colors/progressBar", "#0080ff"},
    {"colors/progressBackground", "#000000"},
    {"colors/progressActive", "#00ff00"},   // [ui-i]
    {"colors/userOnline", "#00ff00"},
    {"colors/userOffline", "#ff4040"},
    {"colors/serverConnected", "#00ff00"},
    {"colors/serverConnecting", "#ffff00"},
    {"colors/serverDisconnected", "#ff0000"},
    {"colors/chatBackground", "#000000"},
    {"colors/chatText", "#ffffff"},
    {"colors/chatOwnName", "#ffff00"},
    {"colors/chatOtherName", "#ffff00"},
    {"colors/chatAction", "#ff00ff"},
    {"colors/chatJoin", "#80ff00"},
    {"colors/chatPart", "#ff8080"},
    {"colors/chatTopic", "#0000ff"},
    {"colors/chatMotd", "#80ffff"},
    {"colors/chatSystem", "#ffff00"},
    {"colors/chatPrivate", "#ff9933"},
    {"colors/chatError", "#ff0000"},
    {"colors/chatTimestamp", "#808080"},
    {"colors/chatLink", "#0000ff"},
    {"colors/chatInputBackground", "#000000"},
    {"colors/chatInputText", "#ffffff"},
    {"colors/chatUserListBackground", "#000000"},
    {"colors/chatUserListText", "#ffffff"},
    {"colors/chatUserAdmin", "#ff0000"},
    {"colors/chatUserVoiced", "#00ff00"},
    {"colors/graphBackground", "#000000"},
    {"colors/graphGrid", "#808080"},
    {"colors/graphDown", "#ffffff"},
    {"colors/graphUp", "#ffff00"},
    {"colors/graphText", "#ffffff"},
    {"colors/statusLedOn", "#00ff00"},
    {"colors/statusLedOff", "#000000"},
    // UI-only keys (mx::ui::ckeys)
    {"colors/slotOne", "#ff8040"},
    {"colors/transferConnecting", "#ffff00"},
    {"colors/transferNegotiating", "#ffffff"},
    {"colors/transferCancelled", "#ffffff"},
    {"colors/transferRemoteQueued", "#c080ff"},
    {"colors/transferBusy", "#ff80ff"},
    {"colors/hotlistHeader", "#ff8080"},
    {"colors/hotlistTemporary", "#ffffcc"},
    {"colors/ignoredText", "#ffffcc"},
    {"colors/statusLedConnecting", "#ffff00"},
    {"colors/statusLedError", "#ff0000"},
    {"colors/newsBackground", "#000000"},
    {"colors/newsText", "#ffff00"},
    {"colors/newsLink", "#ff00ff"},
    {"colors/inactiveText", "#808080"},
    {"colors/channelName", "#ffffff"},
    {"colors/channelUsers", "#ffffff"},
    {"colors/channelTopic", "#ffffff"},
    {"colors/chatUserListOpenNap", "#000000"},
    {"colors/chatUserListOther", "#808080"},
    {"colors/graphOverlay", "#ff8000"},
    {"colors/graphPackets", "#ff00ff"},
    {"colors/graphIncomingPeers", "#0080c0"},
    {"colors/graphOutgoingPeers", "#00ffff"},
    {"colors/graphServerIn", "#80ff00"},
    {"colors/graphServerOut", "#00ff00"},
    {"colors/graphOther", "#ff0000"},
    {"colors/suspectText", "#c08080"},   // [ui-d] SearchHit::suspect rows (dusty rose: none of the original WinMX colours)
};

const QHash<QString, QColor> &defaultTable()
{
    static const QHash<QString, QColor> t = [] {
        QHash<QString, QColor> h;
        for (const Def &d : kDefaults)
            h.insert(QString::fromLatin1(d.key), QColor(QString::fromLatin1(d.rgb)));
        return h;
    }();
    return t;
}

int g_systemOverride = -1;

const QHash<QString, QColor> &lightTable()
{
    static const QHash<QString, QColor> t = [] {
        QHash<QString, QColor> h;
        const QVariantMap m = mx::colorSchemePreset(true);
        for (auto it = m.constBegin(); it != m.constEnd(); ++it)
            h.insert(it.key(), QColor(it.value().toString()));
        return h;
    }();
    return t;
}
} // namespace

Colors::Colors(QObject *parent) : QObject(parent) {}

Colors *Colors::instance()
{
    static Colors *c = new Colors;
    return c;
}

bool Colors::showGrid() const
{
    return m_settings && m_settings->b(keys::ui::ShowGrid);
}

void Colors::attach(mx::Settings *s)
{
    // [post-1] "Follow system": the OS switched between light and dark (connected once, as soon as
    // a GUI application exists)
    static bool hinted = false;
    if (!hinted) {
        if (auto *app = qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
            hinted = true;
            connect(app->styleHints(), &QStyleHints::colorSchemeChanged, this, [this] { scheduleApply(); });
        }
    }
    if (m_conn)
        disconnect(m_conn);
    m_settings = s;
    if (s) {
        connect(s, &QObject::destroyed, this, [this, s] {
            if (m_settings == s) {
                m_settings = nullptr;
                m_cache.clear();
            }
        });
        m_conn = connect(s, &mx::Settings::changed, this, [this](const QString &key, const QVariant &) {
            if (key == QLatin1String(keys::ui::ColorTheme) || key == QLatin1String(keys::colors::ThemeApplied))
                scheduleApply();
            if (key.startsWith(QLatin1String("colors/")) && !m_batch)
                reload();
            if (key == QLatin1String(keys::ui::ShowGrid))   // [ui-h]
                emit changed();
        });
    }
    reload();
    applyTheme();   // e.g. "Follow system" and the system is now light, or a profile edited by hand
}

void Colors::reload()
{
    m_cache.clear();
    emit changed();
}

void Colors::scheduleApply()
{
    if (m_applyPending)
        return;
    m_applyPending = true;
    // queued: never rewrite colours from inside another Settings::changed handler
    QTimer::singleShot(0, this, [this] {
        m_applyPending = false;
        applyTheme();
    });
}

void Colors::setSystemSchemeOverride(int v)
{
    g_systemOverride = v;
    Colors::instance()->scheduleApply();
}

int Colors::systemScheme()
{
    if (g_systemOverride >= 0)
        return g_systemOverride;
    auto *app = qobject_cast<QGuiApplication *>(QCoreApplication::instance());
    if (!app)
        return 0;
    switch (app->styleHints()->colorScheme()) {
    case Qt::ColorScheme::Light: return 1;
    case Qt::ColorScheme::Dark: return 2;
    default: return 0;
    }
}

ColorTheme Colors::wantedTheme(const mx::Settings *s)
{
    if (!s)
        return ColorTheme::Dark;
    const QString t = s->s(QString(keys::ui::ColorTheme));
    if (t == QLatin1String("light"))
        return ColorTheme::Light;
    if (t == QLatin1String("system"))
        return systemScheme() == 1 ? ColorTheme::Light : ColorTheme::Dark;   // no answer: Dark
    return ColorTheme::Dark;
}

ColorTheme Colors::theme() const
{
    if (!m_settings)
        return ColorTheme::Dark;
    return m_settings->s(QString(keys::colors::ThemeApplied)) == QLatin1String("light") ? ColorTheme::Light : ColorTheme::Dark;
}

QColor Colors::themeDefault(const QString &key, ColorTheme t)
{
    if (t == ColorTheme::Light) {
        const auto it = lightTable().constFind(key);
        if (it != lightTable().constEnd())
            return *it;
    }
    return defaultTable().value(key);
}

void Colors::applyTheme()
{
    if (!m_settings)
        return;
    const ColorTheme from = theme(), to = wantedTheme(m_settings);
    if (from == to)
        return;
    m_batch = true;
    int replaced = 0, kept = 0;
    for (const QString &key : knownKeys()) {
        const QColor oldDef = themeDefault(key, from), newDef = themeDefault(key, to);
        if (oldDef == newDef)
            continue;
        const QColor cur(m_settings->s(key));
        if (!cur.isValid() || cur == oldDef) {
            m_settings->setValue(key, newDef.name());
            ++replaced;
        } else {
            ++kept;   // the user's own colour
        }
    }
    m_settings->setValue(QString(keys::colors::ThemeApplied), themeId(to));
    m_batch = false;
    qInfo("octomx: colour theme %s (%d colours replaced, %d customised kept)", qPrintable(themeId(to)), replaced, kept);
    reload();
    emit themeChanged(to);
}

double Colors::contrast(const QColor &a, const QColor &b)
{
    auto lum = [](const QColor &c) {
        auto ch = [](double v) { return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
        return 0.2126 * ch(c.redF()) + 0.7152 * ch(c.greenF()) + 0.0722 * ch(c.blueF());
    };
    double la = lum(a), lb = lum(b);
    if (la < lb)
        std::swap(la, lb);
    return (la + 0.05) / (lb + 0.05);
}

QColor Colors::specDefault(const QString &key)
{
    return defaultTable().value(key);
}

QStringList Colors::knownKeys()
{
    QStringList l;
    for (const Def &d : kDefaults)
        l << QString::fromLatin1(d.key);
    return l;
}

QColor Colors::get(const QString &key) const
{
    auto it = m_cache.constFind(key);
    if (it != m_cache.constEnd())
        return *it;
    QColor c;
    if (m_settings) {
        const QVariant v = m_settings->value(key);
        if (v.isValid()) {
            const QColor sc(v.toString());
            if (sc.isValid())
                c = sc;
        }
    }
    if (!c.isValid())
        c = themeDefault(key, theme()).isValid() ? themeDefault(key, theme()) : QColor(Qt::white);
    m_cache.insert(key, c);
    return c;
}

} // namespace mx::ui

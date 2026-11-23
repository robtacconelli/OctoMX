// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: the one palette object every widget takes its colours from.
// Initialised from mx::Settings ("colors/..." keys, see mx/settings_keys.h); defaults are the
// "Standard Dark Background / Light Text" scheme of docs/UI_SPEC.md §2. When a colours setting
// changes (e.g. from the Settings ▸ Appearance page) `changed()` is emitted and every list view /
// page repaints, so colours can be edited live.
//
//   QColor c = mx::ui::color(mx::keys::colors::BitrateHigh);
//   connect(mx::ui::Colors::instance(), &mx::ui::Colors::changed, this, qOverload<>(&QWidget::update));
#pragma once

#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtGui/QColor>

#include "mx/settings_keys.h"

namespace mx {
class Settings;
}

namespace mx::ui {

// Colour keys the UI uses beyond the original catalogue (most are in the schema now; the news-bar
// ones are UI-only and fall back to the defaults below).
namespace ckeys {
using K = QLatin1StringView;
inline constexpr K OneSlotAvailable{"colors/slotOne"};                // "1 of 4 available" (#FF8040)
inline constexpr K TransferWaiting{"colors/transferConnecting"};      // Finding sources / Connecting (yellow)
inline constexpr K TransferNegotiating{"colors/transferNegotiating"}; // "Connected, Negotiating..." (white)
inline constexpr K TransferCancelled{"colors/transferCancelled"};     // "Cancelled" (white)
inline constexpr K TransferRemoteQueued{"colors/transferRemoteQueued"}; // "Remotely Queued 7" (purple)
inline constexpr K TransferBusy{"colors/transferBusy"};               // "Busy (4 Users in Queue)" (pink)
inline constexpr K HotlistHeader{"colors/hotlistHeader"};             // section titles + rules (#FF8080)
inline constexpr K HotlistTemporary{"colors/hotlistTemporary"};       // temporary entries (#FFFFCC)
inline constexpr K IgnoredText{"colors/ignoredText"};                 // Ignored Users entries (#FFFFCC)
inline constexpr K StatusLedConnecting{"colors/statusLedConnecting"}; // yellow lamp
inline constexpr K StatusLedError{"colors/statusLedError"};           // red lamp
inline constexpr K NewsBackground{"colors/newsBackground"};
inline constexpr K NewsText{"colors/newsText"};
inline constexpr K NewsLink{"colors/newsLink"};
inline constexpr K ListInactive{"colors/inactiveText"};               // gray text (#808080)
inline constexpr K SuspectText{"colors/suspectText"};                 // [ui-d] search hits flagged as suspected fakes
}

// [post-1] colour themes: Dark (classic, the defaults above) and Light (white lists, darker state
// colours, mx::colorSchemePreset(true)). Setting ui/colorTheme = dark | light | system; "system"
// follows QStyleHints::colorScheme() live and is Dark when the system gives no answer.
enum class ColorTheme { Dark, Light };

class Colors : public QObject {
    Q_OBJECT
public:
    static Colors *instance();

    // Read every "colors/..." key from `s` and follow its changes. May be called again with
    // another Settings (or nullptr = defaults only).
    void attach(mx::Settings *s);

    QColor get(const QString &key) const;
    QColor get(QLatin1StringView key) const { return get(QString(key)); }
    // The built-in default (UI_SPEC §2) for a key; invalid if unknown.
    static QColor specDefault(const QString &key);
    // All keys with a built-in default (for the Settings ▸ Appearance page).
    static QStringList knownKeys();

    // ---- [post-1] themes
    // The default of `key` in a theme (Dark = specDefault).
    static QColor themeDefault(const QString &key, ColorTheme t);
    // The theme the setting asks for now ("system" resolved; no settings = Dark).
    static ColorTheme wantedTheme(const mx::Settings *s);
    // The theme the current colours were made for (colors/themeApplied).
    ColorTheme theme() const;
    // Make the colours match wantedTheme(): every colour still at the old theme's default gets the
    // new theme's default; colours the user customised stay. Runs by itself when ui/colorTheme,
    // colors/themeApplied or the system colour scheme change; public for tests.
    void applyTheme();
    // tests: pretend the system colour scheme is -1 = ask Qt, 0 = unknown, 1 = light, 2 = dark
    static void setSystemSchemeOverride(int v);
    static int systemScheme();   // 0 = unknown, 1 = light, 2 = dark
    // WCAG contrast ratio of two colours (1 ... 21)
    static double contrast(const QColor &a, const QColor &b);
    static QString themeId(ColorTheme t) { return t == ColorTheme::Light ? QStringLiteral("light") : QStringLiteral("dark"); }
    // [ui-h] Settings > Interface and Logs "Show grid lines in lists" (ui/showGrid): every MxListView
    // draws its cell borders in colors/listGrid; changed() is emitted when it is switched
    bool showGrid() const;

signals:
    void changed();
    void themeChanged(mx::ui::ColorTheme t);   // [post-1] after applyTheme() switched the colours

private:
    explicit Colors(QObject *parent = nullptr);
    void reload();
    void scheduleApply();
    mx::Settings *m_settings = nullptr;
    QMetaObject::Connection m_conn;
    mutable QHash<QString, QColor> m_cache;
    bool m_batch = false;
    bool m_applyPending = false;
};

inline QColor color(QLatin1StringView key) { return Colors::instance()->get(key); }
inline QColor color(const QString &key) { return Colors::instance()->get(key); }

// Fixed chrome colours of the classic Windows 2000 scheme (UI_SPEC §1.3). Not user-configurable.
namespace chrome {
inline const QColor Face{0xD4, 0xD0, 0xC8};
inline const QColor Light{0xFF, 0xFF, 0xFF};
inline const QColor Shadow{0x80, 0x80, 0x80};
inline const QColor DarkShadow{0x40, 0x40, 0x40};
inline const QColor Text{0x00, 0x00, 0x00};
inline const QColor DisabledText{0x80, 0x80, 0x80};
inline const QColor MenuHighlight{0x0A, 0x24, 0x6A};
inline const QColor Info{0xFF, 0xFF, 0xE1};               // tooltip background
}

} // namespace mx::ui

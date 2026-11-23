// SPDX-License-Identifier: GPL-3.0-or-later
// [post-1] Dark / Light / Follow system colour theme: the light table covers every colour, state
// colours stay readable on white (contrast), switching replaces only colours still at the other
// theme's default, "Follow system" follows the system scheme live, and every list, chat view,
// graph and filter box of every window (main pages, floating window, Connection Test, chat room,
// search tabs) changes at once. Also the Settings ▸ Appearance switch and the quick menus.
#include <QtTest/QtTest>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMenu>

#include "uib_helpers.h"
#include "mx/settings_keys.h"
#include "mx/simple_settings.h"
#include "ui/dialogs/connection_test.h"
#include "ui/dialogs/settings_dialog.h"
#include "ui/kit/colors.h"
#include "ui/kit/list_view.h"
#include "ui/pages/chat_page.h"
#include "ui/pages/chat_view.h"
#include "ui/os_integration.h"
#include "ui/tray.h"

using namespace mx;
using namespace mx::ui;

namespace {

// keys drawn as text on the list background (or the chat background for chat keys)
QStringList textKeys()
{
    QStringList out;
    static const QStringList notText = {
        "colors/listBackground", "colors/listSelectedBackground", "colors/listGrid", "colors/listHeaderText",
        "colors/progressBar", "colors/progressBackground", "colors/progressActive", "colors/chatBackground", "colors/chatInputBackground",
        "colors/chatUserListBackground", "colors/chatUserListOpenNap", "colors/chatUserListOther", "colors/newsBackground"};
    for (const QString &k : Colors::knownKeys()) {
        if (notText.contains(k) || k.startsWith(QLatin1String("colors/graph")) || k.startsWith(QLatin1String("colors/statusLed")))
            continue;
        out << k;
    }
    return out;
}

QString backgroundOf(const QString &key)
{
    if (key.startsWith(QLatin1String("colors/news")))
        return QStringLiteral("colors/newsBackground");
    if (key == QLatin1String("colors/chatInputText"))
        return QStringLiteral("colors/chatInputBackground");
    if (key.startsWith(QLatin1String("colors/chatUser")))
        return QStringLiteral("colors/chatUserListBackground");
    if (key.startsWith(QLatin1String("colors/chat")))
        return QStringLiteral("colors/chatBackground");
    return QStringLiteral("colors/listBackground");
}

} // namespace

class TstTheme : public QObject {
    Q_OBJECT
private:
    QTemporaryDir m_dir;

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        Colors::setSystemSchemeOverride(0);   // offscreen: no answer from the system
    }

    void lightTableComplete()
    {
        // every colour of the schema has a hand-picked light value, and Colors knows each key
        int n = 0;
        for (const SettingDef &d : settingsSchema()) {
            if (d.type != SettingType::Color)
                continue;
            ++n;
            QVERIFY2(hasLightColor(d.key), qPrintable(d.key));
            QVERIFY2(Colors::knownKeys().contains(d.key), qPrintable(d.key));
            QVERIFY2(Colors::themeDefault(d.key, ColorTheme::Light).isValid(), qPrintable(d.key));
        }
        QVERIFY(n > 80);
        for (const QString &k : Colors::knownKeys())
            QVERIFY2(findSetting(k) && findSetting(k)->type == SettingType::Color, qPrintable(k));
        QCOMPARE(Colors::themeDefault(QString(keys::colors::ListBackground), ColorTheme::Light), QColor(Qt::white));
        QCOMPARE(Colors::themeDefault(QString(keys::colors::ListBackground), ColorTheme::Dark), QColor(Qt::black));
        // the gray chrome is not a theme colour: the classic palette stays
        QVERIFY(!Colors::knownKeys().contains(QStringLiteral("colors/face")));
    }

    void lightContrast()
    {
        // every text colour of the Light theme: >= 4.5:1 on its background and on the selection
        const QColor sel = Colors::themeDefault(QString(keys::colors::ListSelectedBackground), ColorTheme::Light);
        for (const QString &k : textKeys()) {
            const QColor fg = Colors::themeDefault(k, ColorTheme::Light);
            const QColor bg = Colors::themeDefault(backgroundOf(k), ColorTheme::Light);
            const double c = Colors::contrast(fg, bg);
            QVERIFY2(c >= 4.5, qPrintable(QStringLiteral("%1 %2 on %3: %4").arg(k, fg.name(), bg.name()).arg(c, 0, 'f', 2)));
            if (backgroundOf(k) == QLatin1String("colors/listBackground")) {
                const double cs = Colors::contrast(fg, sel);
                QVERIFY2(cs >= 4.5, qPrintable(QStringLiteral("%1 %2 on the selection: %3").arg(k, fg.name()).arg(cs, 0, 'f', 2)));
            }
        }
        // the yellow / cyan / green texts of the dark theme got darker counterparts of the same hue
        for (const char *k : {"colors/bitrateMedium", "colors/canJoinDownload", "colors/available"}) {
            const QColor d = Colors::themeDefault(QString::fromLatin1(k), ColorTheme::Dark);
            const QColor l = Colors::themeDefault(QString::fromLatin1(k), ColorTheme::Light);
            QVERIFY2(qAbs(d.hsvHue() - l.hsvHue()) <= 25, k);
            QVERIFY2(l.valueF() < 0.6, k);
        }
    }

    void switchKeepsCustomColours()
    {
        SimpleSettings s;
        Colors::instance()->attach(&s);
        QCOMPARE(Colors::instance()->theme(), ColorTheme::Dark);
        QCOMPARE(color(keys::colors::ListBackground), QColor(Qt::black));
        s.setValue(QString(keys::colors::BitrateHigh), QStringLiteral("#123456"));   // the user's own colour
        QSignalSpy themeSpy(Colors::instance(), &Colors::themeChanged);
        s.setValue(QString(keys::ui::ColorTheme), QStringLiteral("light"));
        QTRY_COMPARE(Colors::instance()->theme(), ColorTheme::Light);
        QCOMPARE(themeSpy.count(), 1);
        QCOMPARE(color(keys::colors::ListBackground), QColor(Qt::white));
        QCOMPARE(color(keys::colors::ListText), QColor(Qt::black));
        QCOMPARE(color(keys::colors::BitrateMedium), QColor(QStringLiteral("#6e4a00")));
        QCOMPARE(color(keys::colors::BitrateHigh), QColor(QStringLiteral("#123456")));   // kept
        QCOMPARE(s.s(keys::colors::ThemeApplied), QStringLiteral("light"));
        // a colour changed while Light is on is kept when going back to Dark
        s.setValue(QString(keys::colors::ChatText), QStringLiteral("#203040"));
        s.setValue(QString(keys::ui::ColorTheme), QStringLiteral("dark"));
        QTRY_COMPARE(Colors::instance()->theme(), ColorTheme::Dark);
        QCOMPARE(color(keys::colors::ListBackground), QColor(Qt::black));
        QCOMPARE(color(keys::colors::BitrateMedium), QColor(QStringLiteral("#ffff00")));
        QCOMPARE(color(keys::colors::BitrateHigh), QColor(QStringLiteral("#123456")));
        QCOMPARE(color(keys::colors::ChatText), QColor(QStringLiteral("#203040")));
        // a colour reset ("Reset Defaults" of the colours) also resets the theme bookkeeping
        s.setValue(QString(keys::ui::ColorTheme), QStringLiteral("light"));
        QTRY_COMPARE(Colors::instance()->theme(), ColorTheme::Light);
        s.resetToDefaults(QStringLiteral("colors/"));
        QTRY_COMPARE(color(keys::colors::ListBackground), QColor(Qt::white));   // converted again
        QCOMPARE(Colors::instance()->theme(), ColorTheme::Light);
        Colors::instance()->attach(nullptr);
    }

    void followSystem()
    {
        SimpleSettings s;
        Colors::instance()->attach(&s);
        Colors::setSystemSchemeOverride(0);
        s.setValue(QString(keys::ui::ColorTheme), QStringLiteral("system"));
        QTest::qWait(20);
        QCOMPARE(Colors::instance()->theme(), ColorTheme::Dark);   // no answer: Dark
        Colors::setSystemSchemeOverride(1);                         // the system turned light
        QTRY_COMPARE(Colors::instance()->theme(), ColorTheme::Light);
        QCOMPARE(color(keys::colors::ChatBackground), QColor(Qt::white));
        Colors::setSystemSchemeOverride(2);                         // and dark again, live
        QTRY_COMPARE(Colors::instance()->theme(), ColorTheme::Dark);
        QCOMPARE(color(keys::colors::ChatBackground), QColor(Qt::black));
        // the real hint: Qt answers Unknown / Light / Dark; -1 asks it (offscreen: unknown or whatever)
        Colors::setSystemSchemeOverride(-1);
        QVERIFY(Colors::systemScheme() >= 0 && Colors::systemScheme() <= 2);
        Colors::setSystemSchemeOverride(0);
        QTest::qWait(20);
        Colors::instance()->attach(nullptr);
    }

    void everyWindowFollows()
    {
        uib::App app;
        app.start(m_dir.filePath(QStringLiteral("p1")));
        MainWindow *mw = app.mw;
        // open what has its own lists / views: a floating page, a second search tab, a chat room,
        // the bandwidth graph, the Connection Test window
        mw->showPage(QStringLiteral("search"));
        mw->startSearch(QStringLiteral("sintel"));
        mw->floatPage(QStringLiteral("bandwidth"));
        if (auto *chat = qobject_cast<ChatPage *>(mw->page(QStringLiteral("chat"))))
            chat->join(QStringLiteral("Linux Users"));
        ConnectionTestDialog::open(app.core, mw);
        QTest::qWait(150);

        auto check = [&](ColorTheme t) {
            const QColor listBg = Colors::themeDefault(QString(keys::colors::ListBackground), t);
            const QColor listText = Colors::themeDefault(QString(keys::colors::ListText), t);
            const QColor chatBg = Colors::themeDefault(QString(keys::colors::ChatBackground), t);
            int lists = 0, chats = 0;
            for (QWidget *w : QApplication::allWidgets()) {
                if (auto *l = qobject_cast<MxListView *>(w)) {
                    ++lists;
                    QVERIFY2(l->palette().color(QPalette::Base) == listBg, qPrintable(l->objectName()));
                    QVERIFY2(l->palette().color(QPalette::Text) == listText, qPrintable(l->objectName()));
                } else if (auto *c = qobject_cast<ChatOutput *>(w)) {
                    ++chats;
                    QVERIFY2(c->palette().color(QPalette::Base) == chatBg, qPrintable(c->objectName()));
                }
            }
            QVERIFY2(lists >= 8, qPrintable(QString::number(lists)));
            QVERIFY(chats >= 1);
            auto *filter = mw->findChild<QLineEdit *>(QStringLiteral("chatFilterText"));
            QVERIFY(filter);
            QCOMPARE(filter->palette().color(QPalette::Base), listBg);
            // the bandwidth graph (floating) is painted with the theme's background
            QWidget *graph = nullptr;
            for (QWidget *w : QApplication::allWidgets())
                if (w->objectName() == QLatin1String("bandwidthIncoming") && w->isVisible())
                    graph = w;
            QVERIFY(graph);
            const QImage img = graph->grab().toImage();
            const QColor gbg = Colors::themeDefault(QString(keys::colors::GraphBackground), t);
            int hits = 0;
            for (int y = 0; y < img.height(); y += 7)
                for (int x = 0; x < img.width(); x += 7)
                    if (img.pixelColor(x, y) == gbg)
                        ++hits;
            QVERIFY2(hits > 20, qPrintable(QStringLiteral("graph background %1: %2 samples").arg(gbg.name()).arg(hits)));
        };
        check(ColorTheme::Dark);
        app.core->settings()->setValue(QString(keys::ui::ColorTheme), QStringLiteral("light"));
        QTRY_COMPARE(Colors::instance()->theme(), ColorTheme::Light);
        QTest::qWait(50);
        check(ColorTheme::Light);
        app.core->settings()->setValue(QString(keys::ui::ColorTheme), QStringLiteral("dark"));
        QTRY_COMPARE(Colors::instance()->theme(), ColorTheme::Dark);
        QTest::qWait(50);
        check(ColorTheme::Dark);
        if (ConnectionTestDialog *d = ConnectionTestDialog::instance())
            d->close();
        app.stop();
    }

    void settingsAndQuickMenus()
    {
        uib::App app;
        app.start(m_dir.filePath(QStringLiteral("p2")));
        MainWindow *mw = app.mw;
        // Settings ▸ Appearance: one obvious combo box with the three choices
        mw->openSettings(QStringLiteral("appearance"));
        auto *dlg = mw->findChild<SettingsDialog *>();
        QVERIFY(dlg);
        auto *combo = dlg->findChild<QComboBox *>(QStringLiteral("set_ui_colorTheme"));
        QVERIFY(combo);
        QCOMPARE(combo->count(), 3);
        QCOMPARE(combo->itemText(0), QStringLiteral("Dark (classic)"));
        QCOMPARE(combo->itemText(1), QStringLiteral("Light"));
        QCOMPARE(combo->itemText(2), QStringLiteral("Follow system"));
        QCOMPARE(combo->currentData().toString(), QStringLiteral("dark"));   // default
        combo->setCurrentIndex(1);
        emit combo->activated(1);
        QTRY_COMPARE(Colors::instance()->theme(), ColorTheme::Light);
        QCOMPARE(app.core->settings()->s(keys::ui::ColorTheme), QStringLiteral("light"));
        // "Load Standard Dark ... Defaults" = Dark theme and every colour back to its default
        app.core->settings()->setValue(QString(keys::colors::PingGood), QStringLiteral("#010203"));
        dlg->findChild<QAbstractButton *>(QStringLiteral("appearanceDark"))->click();
        QTRY_COMPARE(Colors::instance()->theme(), ColorTheme::Dark);
        QCOMPARE(combo->currentData().toString(), QStringLiteral("dark"));
        QCOMPARE(color(keys::colors::PingGood), QColor(QStringLiteral("#00ff00")));
        dlg->findChild<QAbstractButton *>(QStringLiteral("appearanceLight"))->click();
        QTRY_COMPARE(Colors::instance()->theme(), ColorTheme::Light);
        QCOMPARE(color(keys::colors::PingGood), QColor(QStringLiteral("#006000")));
        dlg->close();
        QTest::qWait(20);

        // tray menu: Color Theme ▸ Dark (classic) / Light / Follow system
        TrayController *tray = TrayController::of(mw);
        QVERIFY(tray);
        QMenu *sub = nullptr;
        for (QAction *a : tray->trayMenu()->actions())
            if (a->menu() && a->text() == QLatin1String("Color Theme"))
                sub = a->menu();
        QVERIFY(sub);
        emit sub->aboutToShow();
        QStringList texts;
        for (QAction *a : sub->actions())
            texts << a->text() + (a->isChecked() ? QStringLiteral("*") : QString());
        QCOMPARE(texts, QStringList({"Dark (classic)", "Light*", "Follow system"}));
        sub->actions().at(0)->trigger();
        QTRY_COMPARE(Colors::instance()->theme(), ColorTheme::Dark);
        // the Settings button's right-click menu: Settings... / Color Theme ▸
        QMenu *sm = mw->settingsButtonMenu();
        QCOMPARE(sm->actions().first()->text(), QStringLiteral("Settings..."));
        QMenu *sub2 = sm->actions().last()->menu();
        QVERIFY(sub2);
        QCOMPARE(sub2->actions().size(), 3);
        sub2->actions().at(2)->trigger();   // Follow system (no answer here: Dark)
        QCOMPARE(app.core->settings()->s(keys::ui::ColorTheme), QStringLiteral("system"));
        QTest::qWait(30);
        QCOMPARE(Colors::instance()->theme(), ColorTheme::Dark);
        Colors::setSystemSchemeOverride(1);
        QTRY_COMPARE(Colors::instance()->theme(), ColorTheme::Light);
        Colors::setSystemSchemeOverride(0);
        QTRY_COMPARE(Colors::instance()->theme(), ColorTheme::Dark);
        app.stop();
    }

    void rememberedAcrossRestart()
    {
        const QString prof = m_dir.filePath(QStringLiteral("p3"));
        {
            uib::App app;
            app.start(prof);
            app.core->settings()->setValue(QString(keys::colors::BitrateLow), QStringLiteral("#445566"));
            app.core->settings()->setValue(QString(keys::ui::ColorTheme), QStringLiteral("light"));
            QTRY_COMPARE(Colors::instance()->theme(), ColorTheme::Light);
            app.stop();
        }
        uib::App app;
        app.start(prof);
        QCOMPARE(Colors::instance()->theme(), ColorTheme::Light);
        QCOMPARE(app.core->settings()->s(keys::ui::ColorTheme), QStringLiteral("light"));
        QCOMPARE(color(keys::colors::ListBackground), QColor(Qt::white));
        QCOMPARE(color(keys::colors::BitrateLow), QColor(QStringLiteral("#445566")));
        app.stop();
    }

    void cleanupTestCase()
    {
        Colors::setSystemSchemeOverride(-1);
    }
};

QTEST_MAIN(TstTheme)
#include "tst_theme.moc"

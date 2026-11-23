// SPDX-License-Identifier: GPL-3.0-or-later
// [ui-g] Settings > Appearance > Load Language From File... loads a Qt translation (.qm) through
// QTranslator; Reset Language To English removes it; a file that is no translation is refused.
// The .qm is written by the test itself (the format lrelease writes), so no Qt tool is needed.
#include <QtCore/QDataStream>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QApplication>

#include "mock/mock_core.h"
#include "mx/services.h"
#include "mx/settings_keys.h"
#include "ui/dialogs/about_dialog.h"
#include "ui/dialogs/settings_binding.h"
#include "ui/dialogs/settings_dialog.h"
#include "ui/kit/colors.h"
#include "ui/kit/translation.h"
#include "ui/kit/widgets.h"

using namespace mx;
using namespace mx::ui;

namespace {

// the hash QTranslator looks messages up by (ELF hash of source text + comment, UTF-8)
quint32 elfHash(const QByteArray &ba)
{
    quint32 h = 0;
    for (const char ch : ba) {
        h = (h << 4) + quint8(ch);
        const quint32 g = h & 0xf0000000u;
        if (g)
            h ^= g >> 24;
        h &= ~g;
    }
    return h ? h : 1;
}

// A minimal .qm with one message: magic, language, hash table, messages, numerus rules (as lrelease).
QByteArray makeQm(const QByteArray &context, const QByteArray &source, const QString &translation)
{
    QByteArray msg;
    {
        QDataStream m(&msg, QIODevice::WriteOnly);   // big endian, as the format wants
        m << quint8(0x03);                           // translation, UTF-16 BE
        m << quint32(translation.size() * 2);
        for (const QChar c : translation)
            m << quint16(c.unicode());
        m << quint8(0x08) << quint32(0);             // comment (empty)
        m << quint8(0x06) << quint32(source.size());
        m.writeRawData(source.constData(), int(source.size()));
        m << quint8(0x07) << quint32(context.size());
        m.writeRawData(context.constData(), int(context.size()));
        m << quint8(0x01);                           // end of message
    }
    QByteArray out;
    QDataStream s(&out, QIODevice::WriteOnly);
    static const quint8 magic[16] = {0x3c, 0xb8, 0x64, 0x18, 0xca, 0xef, 0x9c, 0x95,
                                     0xcd, 0x21, 0x1c, 0xbf, 0x60, 0xa1, 0xbd, 0xdd};
    s.writeRawData(reinterpret_cast<const char *>(magic), 16);
    s << quint8(0xa7) << quint32(2);                 // language "xx" (a test language)
    s.writeRawData("xx", 2);
    s << quint8(0x42) << quint32(8) << elfHash(source) << quint32(0);   // hash table: one entry at offset 0
    s << quint8(0x69) << quint32(msg.size());
    s.writeRawData(msg.constData(), int(msg.size()));
    s << quint8(0x88) << quint32(2) << quint8(1) << quint8(1);         // numerus rules
    return out;
}

} // namespace

class TstUigLang : public QObject {
    Q_OBJECT
private:
    QTemporaryDir m_dir;
    Core *m_core = nullptr;
    QString m_qm;
    QStringList m_messages;
    QString m_pick;

    QString aboutTitle()
    {
        AboutDialog d(m_core);
        return d.windowTitle();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        MockOptions o;
        o.frozen = true;
        m_core = createMockCoreWithOptions(m_dir.filePath(QStringLiteral("profile")), o);
        Colors::instance()->attach(m_core->settings());
        m_core->start();
        // the hash of the lrelease output for this message (checked once against lrelease 6.12)
        QCOMPARE(elfHash("About OctoMX"), 0x0123e048u);
        m_qm = m_dir.filePath(QStringLiteral("test_xx.qm"));
        QFile f(m_qm);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(makeQm("mx::ui::AboutDialog", "About OctoMX", QStringLiteral("OctoMX Test Language Title")));
        f.close();
        SettingsBinder::setMessageHook([this](QWidget *, const QString &, const QString &t) { m_messages << t; });
        PathField::setBrowseHook([this](const QString &, bool, QWidget *) { return m_pick; });
        translation::follow(m_core->settings());   // what app/main.cpp does at start
    }

    void cleanupTestCase()
    {
        translation::apply(QString());
        PathField::setBrowseHook({});
        SettingsBinder::setMessageHook({});
        delete m_core;
        Colors::instance()->attach(nullptr);
    }

    void translationFileIsRecognised()
    {
        QVERIFY(translation::isTranslationFile(m_qm));
        const QString lng = m_dir.filePath(QStringLiteral("old.lng"));
        QFile f(lng);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("[WinMX Language File]\nAbout OctoMX=Something\n");
        f.close();
        QVERIFY(!translation::isTranslationFile(lng));
        QVERIFY(!translation::isTranslationFile(m_dir.filePath(QStringLiteral("missing.qm"))));
        QString why;
        QVERIFY(!translation::apply(lng, &why));
        QVERIFY(why.contains(QStringLiteral("old.lng")));
        QVERIFY(translation::current().isEmpty());
    }

    void loadFromAppearancePageAndReset()
    {
        QCOMPARE(aboutTitle(), QStringLiteral("About OctoMX"));
        SettingsDialog d(m_core, nullptr);
        QVERIFY(d.showPage(QStringLiteral("appearance")));
        auto *field = d.findChild<PathField *>(QStringLiteral("set_misc_languageFile"));
        if (!field)   // the object name comes from PageKit::nameFor()
            for (PathField *p : d.findChildren<PathField *>())
                if (p->button()->text() == QLatin1String("Load Language From File..."))
                    field = p;
        QVERIFY(field);
        QVERIFY(!field->button()->toolTip().isEmpty());
        // a WinMX 3.x .lng file is refused: a message, the setting and the language stay
        m_pick = m_dir.filePath(QStringLiteral("old.lng"));
        m_messages.clear();
        field->button()->click();
        QCOMPARE(m_messages.size(), 1);
        QVERIFY(m_messages.first().contains(QStringLiteral("not a Qt translation file")));
        QVERIFY(m_core->settings()->s(keys::misc::LanguageFile).isEmpty());
        QCOMPARE(aboutTitle(), QStringLiteral("About OctoMX"));
        // the .qm is taken: stored, installed, and a window built now speaks the new language
        m_pick = m_qm;
        field->button()->click();
        QCOMPARE(m_core->settings()->s(keys::misc::LanguageFile), m_qm);
        QCOMPARE(translation::current(), m_qm);
        QCOMPARE(aboutTitle(), QStringLiteral("OctoMX Test Language Title"));
        // Reset Language To English removes it again
        auto *english = d.findChild<QAbstractButton *>(QStringLiteral("appearanceEnglish"));
        QVERIFY(english);
        QVERIFY(!english->toolTip().isEmpty());
        english->click();
        QVERIFY(m_core->settings()->s(keys::misc::LanguageFile).isEmpty());
        QVERIFY(translation::current().isEmpty());
        QCOMPARE(aboutTitle(), QStringLiteral("About OctoMX"));
    }

    void storedLanguageAppliesAtStart()
    {
        // a profile that names the file: follow() (called by main() before any window) installs it
        m_core->settings()->setValue(keys::misc::LanguageFile, m_qm);
        translation::apply(QString());
        QCOMPARE(aboutTitle(), QStringLiteral("About OctoMX"));
        translation::follow(m_core->settings());
        QCOMPARE(aboutTitle(), QStringLiteral("OctoMX Test Language Title"));
        m_core->settings()->setValue(keys::misc::LanguageFile, QVariant());
        QCOMPARE(aboutTitle(), QStringLiteral("About OctoMX"));
    }
};

QTEST_MAIN(TstUigLang)
#include "tst_uig_lang.moc"

// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/kit/translation.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QFileInfo>
#include <QtCore/QPointer>
#include <QtCore/QTranslator>

#include "mx/services.h"
#include "mx/settings_keys.h"

namespace mx::ui::translation {

namespace {
QPointer<QTranslator> g_translator;
QString g_current;
QMetaObject::Connection g_follow;
} // namespace

bool isTranslationFile(const QString &path)
{
    if (path.isEmpty() || !QFileInfo(path).isFile())
        return false;
    QTranslator t;
    return t.load(path);
}

bool apply(const QString &qmPath, QString *error)
{
    QCoreApplication *app = QCoreApplication::instance();
    if (!app)
        return false;
    if (qmPath.isEmpty()) {
        if (g_translator) {
            QCoreApplication::removeTranslator(g_translator);
            delete g_translator;
        }
        g_current.clear();
        return true;
    }
    auto *t = new QTranslator(app);
    if (!QFileInfo(qmPath).isFile() || !t->load(qmPath)) {
        delete t;
        if (error)
            *error = QCoreApplication::translate("mx::ui::translation", "%1 is not a Qt translation file (.qm).")
                         .arg(QFileInfo(qmPath).fileName());
        qWarning("octomx: language file %s could not be loaded, keeping English", qPrintable(qmPath));
        return false;
    }
    if (g_translator) {
        QCoreApplication::removeTranslator(g_translator);
        delete g_translator;
    }
    g_translator = t;
    QCoreApplication::installTranslator(t);
    g_current = qmPath;
    return true;
}

QString current()
{
    return g_current;
}

void follow(mx::Settings *settings)
{
    if (!settings)
        return;
    QObject::disconnect(g_follow);
    apply(settings->s(keys::misc::LanguageFile));
    g_follow = QObject::connect(settings, &Settings::changed, settings, [](const QString &key, const QVariant &v) {
        if (key == QLatin1String(keys::misc::LanguageFile))
            apply(v.toString());
    });
}

} // namespace mx::ui::translation

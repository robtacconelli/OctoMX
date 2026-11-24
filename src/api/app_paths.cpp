// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX [mac-1]: where the program is (see mx/app_paths.h).
#include "mx/app_paths.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>

namespace mx::paths {

QString bundleOf(const QString &exeDir)
{
    if (exeDir.isEmpty())
        return QString();
    const QString d = QDir::cleanPath(QDir::fromNativeSeparators(exeDir));
    // .../Name.app/Contents/MacOS
    if (!d.endsWith(QLatin1String("/Contents/MacOS")))
        return QString();
    const QString bundle = d.left(d.size() - int(qstrlen("/Contents/MacOS")));
    if (!bundle.endsWith(QLatin1String(".app"), Qt::CaseInsensitive))
        return QString();
    return bundle;
}

QString programDir(const QString &exeDir)
{
    const QString b = bundleOf(exeDir);
    return b.isEmpty() ? exeDir : b;
}

QString programDir()
{
    return programDir(QCoreApplication::applicationDirPath());
}

QString portableDir(const QString &exeDir)
{
    const QString b = bundleOf(exeDir);
    return b.isEmpty() ? exeDir : QFileInfo(b).absolutePath();
}

QString portableDir()
{
    return portableDir(QCoreApplication::applicationDirPath());
}

} // namespace mx::paths

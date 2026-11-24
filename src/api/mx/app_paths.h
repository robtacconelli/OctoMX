// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX [mac-1]: where the program is, as the user sees it.
//
// Windows and Linux: the folder of the executable. macOS: the program is the app bundle
// (OctoMX.app); the executable sits three levels down in OctoMX.app/Contents/MacOS, and a
// portable.txt (with its profile/ folder) is put beside the bundle, not inside it (the bundle is
// sealed by its code signature).
#pragma once

#include <QtCore/QString>

namespace mx::paths {

// the .app bundle that holds exeDir ("/Applications/OctoMX.app" for
// "/Applications/OctoMX.app/Contents/MacOS"), empty when exeDir is not inside a bundle
QString bundleOf(const QString &exeDir);
// the program as the user sees it: the bundle on macOS, else the executable's folder
QString programDir(const QString &exeDir);
QString programDir();   // of the running program
// where portable.txt is looked for and the portable profile/ lives: the folder that holds the
// bundle on macOS, else the executable's folder
QString portableDir(const QString &exeDir);
QString portableDir();  // of the running program

} // namespace mx::paths

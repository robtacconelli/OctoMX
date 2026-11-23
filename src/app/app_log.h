// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX app: log handler used when the real core (which has its own, core/log.h) is not
// linked: appends to <profile>/logs/octomx.log (rotated to octomx.1.log at 2 MB) and stderr.
#pragma once

#include <QtCore/QString>

namespace mx::app {

void installAppLog(const QString &profileDir);

}

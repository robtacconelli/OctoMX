// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: the Help / About window (version, networks, credits, licences of bundled parts).
#pragma once

#include <QtWidgets/QDialog>

namespace mx {
class Core;
}

namespace mx::ui {

class AboutDialog : public QDialog {
    Q_OBJECT
public:
    explicit AboutDialog(mx::Core *core, QWidget *parent = nullptr);

signals:
    void checkForUpdatesRequested();   // [post-1] the "Check for Updates..." button
};

} // namespace mx::ui

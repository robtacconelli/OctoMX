// SPDX-License-Identifier: GPL-3.0-or-later
// [safe-1] The waiting part of the safe quit: the transfers stop, then OctoMX waits until every
// download is written and synced to the disk and the torrent state is saved (as long as it takes).
// When that takes longer than about a second a small plain window says so ("Closing: saving
// downloads...", what is left) with a "Close now" button: closing then keeps every byte that is
// already recorded; what was not saved yet is downloaded again.
#pragma once

#include <QtWidgets/QDialog>

namespace mx {
class Core;
}

namespace mx::ui {

class StaticText;

class ClosingWindow : public QDialog {
    Q_OBJECT
public:
    explicit ClosingWindow(QWidget *parent = nullptr);
    void setLeft(qint64 bytesLeft, qint64 bytesAtStart);   // the progress figure
signals:
    void closeNowClicked();

protected:
    // the title bar's X and Esc do nothing: only the button closes without waiting
    void closeEvent(QCloseEvent *e) override;
    void reject() override {}

private:
    StaticText *m_figure = nullptr;
};

// core->beginShutdown(), then wait (the event loop runs) until !core->shutdownBusy(); the window
// appears after showAfterMs. A last resort for a hung disk: capMs, then it counts as "Close now".
// Returns false when the user (or the cap) closed it before everything was saved.
bool waitForSafeQuit(Core *core, int showAfterMs = 1000, int capMs = 10 * 60 * 1000);

} // namespace mx::ui

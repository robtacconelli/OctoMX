// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI [post-1, upd-1]: Help ▸ Check for Updates..., the check at start and the signed
// self-update.
//
// One UpdateController per MainWindow (child "updateController"). The manual check always answers
// (a newer version, "You have the latest version", or why the check failed); the automatic one runs
// at most once a day unless updates/mode is "off", stays quiet unless a newer version that was not
// skipped exists, and logs one line otherwise. What a newer release does depends on updates/mode:
//   auto   (default) download it quietly, check the signature, unpack and check it next to the
//          program, then the box "OctoMX 5.8 is ready to install" with Restart Now / Later / Skip
//          This Version (Later: installed when OctoMX is closed, a tray balloon says so once)
//   ask    the box "OctoMX 5.8 is available" with Update Now (progress bar, then the same restart
//          question) / Remind Me Later / Skip This Version and the download page as a link
//   notify the classic message box: Open Download Page / Remind Me Later / Skip This Version
//   off    no check at start; Help ▸ Check for Updates... still asks
// Help ▸ Check for Updates... always asks (the ask box). When the program cannot update itself
// (no published package layout, a folder it cannot write, a release without signature) or the
// download or a check fails, the classic box with Open Download Page is shown, with the reason.
// The install itself runs when OctoMX closes (finishOnExit(), called by main.cpp after the core has
// shut down); after Restart Now the new version is started with the same options. At the next start
// startupRecovery() finishes or undoes an interrupted swap and removes the old files; the first
// start after an update shows "Updated to OctoMX 5.8" with a What's New link.
//
// Network policy (tests and scripted runs never contact GitHub): the app sets On for a real core with
// a user profile, IfUrlSet for a test or scripted run (MX_OS_ROOT is set), Off for
// the demo core. The default, for windows built by tests, is IfUrlSet: only a test that points
// updates/url at its own server gets a check.
#pragma once

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QStringList>

#include <functional>

#include "mx/update_check.h"
#include "mx/update_install.h"

class QDialog;
class QLabel;
class QMessageBox;
class QProgressBar;

namespace mx::upd {
class UpdateDownloader;
}

namespace mx::ui {

class MainWindow;

class UpdateController : public QObject {
    Q_OBJECT
public:
    enum class Policy { Off, IfUrlSet, On };
    enum Answer { OpenPage = 0, RemindLater = 1, SkipVersion = 2, UpdateNow = 3, RestartNow = 4, InstallLater = 5, Cancel = 6 };
    enum class Kind {
        Notify,    // the classic box: Open Download Page / Remind Me Later / Skip This Version (+ a reason)
        Ask,       // Update Now / Remind Me Later / Skip This Version, the page as a link
        Ready,     // Restart Now / Later / Skip This Version
        Updated,   // first start after an update: What's New (OpenPage) / OK (RemindLater)
    };
    enum class State { Idle, Checking, Downloading, Preparing, Ready };
    struct Offer {
        Kind kind = Kind::Notify;
        QString version;   // "5.8"
        QString current;   // "5.7"
        QString notes;     // plain text excerpt
        QString pageUrl;   // what Open Download Page / What's New opens
        QString reason;    // why there is no automatic install (Notify), empty otherwise
        QString text;      // the whole box text
    };

    explicit UpdateController(MainWindow *mw);
    ~UpdateController() override;
    static UpdateController *of(const MainWindow *mw);

    static void setPolicy(Policy p);
    static Policy policy();
    static bool demoMode();   // policy Off because of the demo core
    static void setDemoMode(bool on);
    // may this window's settings contact the update server now?
    bool networkAllowed() const;

    // Help ▸ Check for Updates... (manual = true) or the start check
    void checkNow(bool manual);
    // the automatic check: mode not off, allowed, and the last one more than a day ago
    bool startCheckDue() const;
    void scheduleStartCheck(int delayMs = -1);
    bool isChecking() const;
    QString currentVersion() const;   // MX_VERSION_FULL, e.g. "5.7.0"
    QString mode() const;             // updates/mode: auto | ask | notify | off
    State state() const { return m_state; }
    QString readyVersion() const;     // the prepared update (State::Ready)
    // one line for the About box: "OctoMX 5.7 is up to date" / "OctoMX 5.8 is available" / ...
    QString statusLine() const;

    // show what a check with this answer would show (manual = the answers of Help ▸ Check for
    // Updates...); used by the script verb "update-show <release.json>" for screenshots, no network
    void showResult(const mx::UpdateChecker::Result &r, bool manual) { m_manual = manual; onResult(r); }
    // screenshots: one of the boxes for this release without any network ("notify", "ask",
    // "progress", "ready", "failed", "updated")
    void showDemoBox(const QString &kind, const ReleaseInfo &r);

    // can this release be installed automatically here? (*why: the plain reason when not)
    bool canAutoInstall(const ReleaseInfo &r, QString *why = nullptr) const;
    // download, verify and stage it (quiet: no progress box, the auto mode)
    void startUpdate(const ReleaseInfo &r, bool quiet);

    // the classic message box texts (also used by tests)
    static QString offerText(const ReleaseInfo &r, const QString &current, QString *notesOut = nullptr);

    // ---- the program folder (startup / exit; main.cpp)
    // the installation the program runs from (detected once; tests may set one)
    static mx::upd::Layout layout();
    static void setLayoutForTests(const mx::upd::Layout &l);
    static void resetLayout();
    // the options the program was started with (main.cpp; restarts reuse them, links are not repeated)
    static void setRestartArguments(const QStringList &args);
    // very early in main(): wait for the process that restarted us (MX_UPDATE_WAIT_PID); then finish
    // or undo an interrupted swap and remove old files (call after the single-instance check).
    // Returns true when the program must start again at once (files were moved under it).
    // [safe-1] as long as the previous process lives (it may still be saving downloads; it is never
    // killed), a last resort of kPreviousWaitMs: false = it is still there, this one must not start
    static bool waitForPreviousProcess();
    static constexpr int kPreviousWaitMs = 15 * 60 * 1000;
    static bool startupRecovery();
    // after the core has shut down: install a ready update (and start the new version after Restart
    // Now). Returns true when a new process was started.
    static bool finishOnExit();
    static bool hasPendingInstall();
    // the result of startupRecovery() for the first window: "Updated to ..." (from, to) or a problem
    static QString justUpdatedFrom();
    static QString justUpdatedTo();
    static void setJustUpdatedForTests(const QString &from, const QString &to);   // as startupRecovery() would

    // tests: replace the offer box (return an Answer), the plain message boxes, the browser, the tray
    // balloon, starting the new process and quitting the app
    static std::function<int(const Offer &)> s_offerHook;
    static std::function<void(const QString &text)> s_messageHook;
    static std::function<void(const QString &url)> s_openUrlHook;
    static std::function<void(const QString &text)> s_balloonHook;
    static std::function<bool(const QString &exe, const QStringList &args)> s_startHook;
    static std::function<void()> s_quitHook;
    static int s_startDelayMs;   // default 6000

signals:
    void checkFinished(int outcome, bool manual);   // UpdateChecker::Outcome
    void stateChanged(int state);                   // State
    void updateReady(const QString &version);
    void updateFailed(const QString &error);

private:
    void onResult(const mx::UpdateChecker::Result &r);
    void offer(const ReleaseInfo &r, Kind kind, const QString &reason = QString());
    void showBox(const Offer &o, const std::function<void(int)> &answer);
    void showProgressBox();
    void onDownloaded(const ReleaseInfo &r, bool ok, const QString &archive, const QString &error, bool quiet);
    void onPrepared(const ReleaseInfo &r, bool ok, const QString &stageTop, const mx::upd::Manifest &m, const QString &error, bool quiet);
    void failed(const ReleaseInfo &r, const QString &error, bool quiet);
    void showReady();
    void showUpdated();
    void answerReady(int a);
    void setState(State s);
    void message(const QString &text, bool warning = false);
    void openPage(const QString &url);
    void balloon(const QString &text);
    void requestQuit();
    QString notesFor(const ReleaseInfo &r) const;

    MainWindow *m_mw;
    UpdateChecker *m_checker = nullptr;
    mx::upd::UpdateDownloader *m_download = nullptr;
    bool m_manual = false;
    bool m_quiet = false;             // the running download has no progress box (auto mode)
    State m_state = State::Idle;
    ReleaseInfo m_release;            // the release being downloaded / ready
    QString m_balloonShownFor;
    QPointer<QMessageBox> m_box;
    QPointer<QDialog> m_dialog;
    QPointer<QProgressBar> m_progress;
    QPointer<QLabel> m_progressText;
};

} // namespace mx::ui

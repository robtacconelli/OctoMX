// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI [post-1, upd-1]: Check for Updates and the signed self-update (see update_ui.h).
#include "ui/update_ui.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QLocale>
#include <QtCore/QProcess>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QApplication>
#include <QtWidgets/QDialog>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QStyle>
#include <QtWidgets/QSystemTrayIcon>
#include <QtWidgets/QVBoxLayout>

#include <memory>

#include "mx/services.h"
#include "mx/settings_keys.h"
#include "mx/types.h"
#include "mx/update_download.h"
#include "ui/kit/icons.h"
#include "ui/kit/scale.h"
#include "ui/kit/widgets.h"
#include "ui/main_window.h"
#include "ui/os_integration.h"
#include "ui/script_driver.h"
#include "ui/tray.h"

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#endif

namespace mx::ui {

using namespace mx::keys;

namespace {
UpdateController::Policy g_policy = UpdateController::Policy::IfUrlSet;
bool g_demo = false;
bool g_layoutKnown = false;
mx::upd::Layout g_layout;
bool g_argsKnown = false;
QStringList g_restartArgs;
QString g_updatedFrom, g_updatedTo, g_recoveryNote;
QString g_waited;   // logged by startupRecovery(), once the profile log exists

// the update that is staged and waits for OctoMX to close
struct Pending {
    bool valid = false;
    mx::upd::SwapPlan plan;
    bool restart = false;
    QString version;
};
Pending g_pending;

QString releaseTagPage(const QString &version)
{
    return QStringLiteral("https://github.com/robtacconelli/OctoMX/releases/tag/v") + version;
}

// start the program file with the options of this run; it waits for this process to end first
bool startProgram(const QString &exe, QStringList args)
{
    // scripted runs (tests): the restarted program may get another script
    const QString nextScript = qEnvironmentVariable("MX_UPDATE_RESTART_SCRIPT");
    if (!nextScript.isEmpty()) {
        const int i = int(args.indexOf(QStringLiteral("--script")));
        if (i >= 0 && i + 1 < args.size())
            args[i + 1] = nextScript;
    }
    if (UpdateController::s_startHook)
        return UpdateController::s_startHook(exe, args);
    QProcess p;
    p.setProgram(exe);
    p.setArguments(args);
    p.setWorkingDirectory(QDir::currentPath());
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("MX_UPDATE_WAIT_PID"), QString::number(QCoreApplication::applicationPid()));
    env.remove(QStringLiteral("MX_UPDATE_RESTART_SCRIPT"));
    p.setProcessEnvironment(env);
    qint64 pid = 0;
    const bool ok = p.startDetached(&pid);
    qInfo("octomx: update: %s %s (pid %lld)", ok ? "started" : "could not start", qPrintable(QDir::toNativeSeparators(exe)), pid);
    return ok;
}

QStringList restartArguments()
{
    return g_argsKnown ? g_restartArgs : QCoreApplication::arguments().mid(1);
}

// ---- script verbs (UI automation, screenshots, the end-to-end test)
QWidget *visibleTopLevel(const QString &name)
{
    for (QWidget *w : QApplication::topLevelWidgets())
        if (w->isVisible() && w->objectName() == name)
            return w;
    return nullptr;
}

bool updateVerbs(MainWindow *mw, const QString &verb, const QString &rest, const QStringList &args, QString *error)
{
    UpdateController *u = UpdateController::of(mw);
    if (!u || !verb.startsWith(QLatin1String("update-")))
        return false;
    if (verb == QLatin1String("update-check")) {   // the check at start, now (or "manual")
        u->checkNow(rest == QLatin1String("manual"));
        return true;
    }
    if (verb == QLatin1String("update-wait")) {    // update-wait <window objectName | state:ready> [ms]
        const QString what = args.value(0);
        const int ms = args.value(1, QStringLiteral("60000")).toInt();
        ScriptDriver *d = ScriptDriver::current();
        if (!d) {
            *error = QStringLiteral("update-wait outside a script");
            return true;
        }
        QPointer<UpdateController> pu(u);
        d->holdUntil([pu, what] {
            if (what.startsWith(QLatin1String("state:"))) {
                static const QStringList names = {QStringLiteral("idle"), QStringLiteral("checking"), QStringLiteral("downloading"),
                                                  QStringLiteral("preparing"), QStringLiteral("ready")};
                return pu && names.value(int(pu->state())) == what.mid(6);
            }
            return visibleTopLevel(what) != nullptr;
        }, ms, QStringLiteral("%1 did not appear").arg(what));
        return true;
    }
    if (verb == QLatin1String("update-demo")) {    // update-demo <kind> <release.json>: a box, no network
        QFile f(args.value(1));
        ReleaseInfo r;
        QString why;
        if (!f.open(QIODevice::ReadOnly) || !parseLatestRelease(f.readAll(), &r, &why)) {
            *error = QStringLiteral("cannot read release %1 %2").arg(args.value(1), why);
            return true;
        }
        u->showDemoBox(args.value(0), r);
        return true;
    }
    if (verb == QLatin1String("update-about")) {   // the About box (modal: the script goes on inside it)
        QTimer::singleShot(0, mw, &MainWindow::openHelp);
        return true;
    }
    if (verb == QLatin1String("update-expect-version")) {   // the running program's version
        if (compareVersions(u->currentVersion(), rest) != 0)
            *error = QStringLiteral("this is version %1, not %2").arg(u->currentVersion(), rest);
        return true;
    }
    return false;
}

QLabel *boxIcon(QWidget *parent)
{
    auto *icon = new QLabel(parent);
    icon->setPixmap(informationPixmap(S(32)));   // [ui-h] our own sign: wine showed its generic one
    icon->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    return icon;
}

} // namespace

std::function<int(const UpdateController::Offer &)> UpdateController::s_offerHook;
std::function<void(const QString &)> UpdateController::s_messageHook;
std::function<void(const QString &)> UpdateController::s_openUrlHook;
std::function<void(const QString &)> UpdateController::s_balloonHook;
std::function<bool(const QString &, const QStringList &)> UpdateController::s_startHook;
std::function<void()> UpdateController::s_quitHook;
int UpdateController::s_startDelayMs = 6000;

UpdateController::UpdateController(MainWindow *mw) : QObject(mw), m_mw(mw)
{
    setObjectName(QStringLiteral("updateController"));
    static bool verbs = false;
    if (!verbs) {
        verbs = true;
        ScriptDriver::addCommandHandler(&updateVerbs);
    }
    // [upd-1] the earlier switch updates/checkAtStart ("Check for a new version at start"): off stays off
    Settings *s = mw->core()->settings();
    if (!s->b(updates::ModeMigrated)) {
        if (!s->b(updates::CheckAtStart))
            s->setValue(updates::Mode, QStringLiteral("off"));
        s->setValue(updates::ModeMigrated, true);
    }
    if (!g_updatedTo.isEmpty() || !g_recoveryNote.isEmpty())
        QTimer::singleShot(1200, this, &UpdateController::showUpdated);
}

void UpdateController::setJustUpdatedForTests(const QString &from, const QString &to)
{
    g_updatedFrom = from;
    g_updatedTo = to;
}

UpdateController::~UpdateController() = default;

UpdateController *UpdateController::of(const MainWindow *mw)
{
    return mw ? mw->findChild<UpdateController *>(QStringLiteral("updateController"), Qt::FindDirectChildrenOnly) : nullptr;
}

void UpdateController::setPolicy(Policy p)
{
    g_policy = p;
}

UpdateController::Policy UpdateController::policy()
{
    return g_policy;
}

bool UpdateController::demoMode()
{
    return g_demo;
}

void UpdateController::setDemoMode(bool on)
{
    g_demo = on;
}

QString UpdateController::currentVersion() const
{
    return QStringLiteral(MX_VERSION_FULL);
}

QString UpdateController::mode() const
{
    const QString m = m_mw->core()->settings()->s(updates::Mode);
    static const QStringList known = {QStringLiteral("auto"), QStringLiteral("ask"), QStringLiteral("notify"), QStringLiteral("off")};
    return known.contains(m) ? m : QStringLiteral("auto");
}

QString UpdateController::readyVersion() const
{
    return m_state == State::Ready ? m_release.version : QString();
}

bool UpdateController::networkAllowed() const
{
    if (g_demo || g_policy == Policy::Off)
        return false;
    if (g_policy == Policy::On)
        return true;
    // IfUrlSet: only a test (or a user) that chose the address itself
    Settings *s = m_mw->core()->settings();
    const QString url = s->s(updates::Url).trimmed();
    return !url.isEmpty() && url != s->defaultValue(updates::Url).toString();
}

bool UpdateController::isChecking() const
{
    return m_checker && m_checker->isRunning();
}

bool UpdateController::startCheckDue() const
{
    Settings *s = m_mw->core()->settings();
    if (mode() == QLatin1String("off") || !networkAllowed())
        return false;
    const QDateTime last = QDateTime::fromString(s->s(updates::LastCheck), Qt::ISODate);
    return !last.isValid() || last.secsTo(QDateTime::currentDateTimeUtc()) >= 24 * 3600 ||
           last > QDateTime::currentDateTimeUtc().addSecs(3600);   // a clock that was set back
}

void UpdateController::scheduleStartCheck(int delayMs)
{
    QTimer::singleShot(delayMs >= 0 ? delayMs : s_startDelayMs, this, [this] {
        if (startCheckDue())
            checkNow(false);
    });
}

void UpdateController::checkNow(bool manual)
{
    if (!networkAllowed()) {
        if (manual)
            message(g_demo ? tr("Update checks are off in demo mode.\n\nThe releases are listed at %1").arg(QString::fromLatin1(kReleasesPage))
                           : tr("Update checks are off in this test run.\n\n"
                                "The releases are listed at %1").arg(QString::fromLatin1(kReleasesPage)));
        else
            qInfo("octomx: update check skipped (demo or test run)");
        return;
    }
    if (!m_checker) {
        m_checker = new UpdateChecker(m_mw->core()->settings(), currentVersion(), this);
        connect(m_checker, &UpdateChecker::finished, this, &UpdateController::onResult);
    }
    if (m_checker->isRunning()) {
        m_manual = m_manual || manual;   // the running check answers the manual request too
        return;
    }
    m_manual = manual;
    if (!manual)
        m_mw->core()->settings()->setValue(updates::LastCheck, QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    qInfo("octomx: checking for updates (%s, mode %s)", manual ? "manual" : "at start", qPrintable(mode()));
    if (m_state == State::Idle)   // a download or a ready update keeps its state
        setState(State::Checking);
    m_checker->check();
}

QString UpdateController::offerText(const ReleaseInfo &r, const QString &current, QString *notesOut)
{
    const QString notes = releaseNotesExcerpt(r.body);
    if (notesOut)
        *notesOut = notes;
    QString t = tr("OctoMX %1 is available (you have %2).").arg(displayVersion(r.version), displayVersion(current));
    if (!notes.isEmpty())
        t += QStringLiteral("\n\n") + tr("Release notes:") + QLatin1Char('\n') + notes;
    t += QStringLiteral("\n\n") + tr("Open Download Page shows the release in your web browser. OctoMX does not download or "
                                     "install anything by itself.");
    return t;
}

QString UpdateController::notesFor(const ReleaseInfo &r) const
{
    return releaseNotesExcerpt(r.body);
}

void UpdateController::setState(State s)
{
    if (m_state == s)
        return;
    m_state = s;
    emit stateChanged(int(s));
}

QString UpdateController::statusLine() const
{
    const QString cur = displayVersion(currentVersion());
    if (m_state == State::Ready)
        return tr("OctoMX %1 is ready to install (you have %2).").arg(displayVersion(m_release.version), cur);
    if (m_state == State::Downloading || m_state == State::Preparing)
        return tr("Downloading OctoMX %1...").arg(displayVersion(m_release.version));
    const QString latest = m_mw->core()->settings()->s(updates::LatestSeen);
    if (latest.isEmpty())
        return tr("OctoMX %1, not checked for updates yet.").arg(cur);
    if (compareVersions(latest, currentVersion()) > 0)
        return tr("OctoMX %1 is available (you have %2).").arg(displayVersion(latest), cur);
    return tr("OctoMX %1 is up to date.").arg(cur);
}

void UpdateController::onResult(const UpdateChecker::Result &r)
{
    const bool manual = m_manual;
    m_manual = false;
    if (m_state == State::Checking)
        setState(State::Idle);
    Settings *s = m_mw->core()->settings();
    const QString cur = displayVersion(currentVersion());
    if (r.outcome != UpdateChecker::Outcome::Failed && !r.release.version.isEmpty())
        s->setValue(updates::LatestSeen, r.release.version);
    switch (r.outcome) {
    case UpdateChecker::Outcome::Newer: {
        qInfo("octomx: update check: %s is available (this is %s)", qPrintable(r.release.tag), qPrintable(currentVersion()));
        const QString skip = s->s(updates::SkipVersion);
        if (!manual && !skip.isEmpty() && compareVersions(r.release.version, skip) == 0)
            break;
        const bool same = compareVersions(m_release.version, r.release.version) == 0;
        if (same && (m_state == State::Downloading || m_state == State::Preparing)) {
            if (manual)
                showProgressBox();
            break;
        }
        if (same && m_state == State::Ready) {
            showReady();
            break;
        }
        const QString md = manual ? QStringLiteral("ask") : mode();
        QString why;
        const bool can = md != QLatin1String("notify") && canAutoInstall(r.release, &why);
        if (md == QLatin1String("auto") && can)
            startUpdate(r.release, true);
        else if (can)
            offer(r.release, Kind::Ask);
        else {
            if (md != QLatin1String("notify"))
                qInfo("octomx: update: no automatic install of %s: %s", qPrintable(r.release.tag), qPrintable(why));
            offer(r.release, Kind::Notify, md == QLatin1String("notify") ? QString() : why);
        }
        break;
    }
    case UpdateChecker::Outcome::UpToDate:
    case UpdateChecker::Outcome::NotOffered:
        qInfo("octomx: update check: up to date (%s, latest release %s%s)", qPrintable(currentVersion()), qPrintable(r.release.tag),
              r.outcome == UpdateChecker::Outcome::NotOffered ? ", a pre-release or draft, not offered" : "");
        if (manual)
            message(tr("You have the latest version (OctoMX %1).").arg(cur));
        break;
    case UpdateChecker::Outcome::Older:
        qInfo("octomx: update check: this build (%s) is newer than the latest release %s", qPrintable(currentVersion()),
              qPrintable(r.release.tag));
        if (manual)
            message(tr("You have the latest version (OctoMX %1). The latest published release is %2.")
                        .arg(cur, displayVersion(r.release.version)));
        break;
    case UpdateChecker::Outcome::Failed:
        qInfo("octomx: update check failed: %s (HTTP %d)", qPrintable(r.error), r.httpStatus);
        if (manual)
            message(tr("OctoMX could not check for updates.\n\n%1").arg(r.error), true);
        break;
    }
    emit checkFinished(int(r.outcome), manual);
}

bool UpdateController::canAutoInstall(const ReleaseInfo &r, QString *why) const
{
    auto no = [why](const QString &w) {
        if (why)
            *why = w;
        return false;
    };
    if (r.draft || r.prerelease || parseVersion(r.tag).isEmpty() || compareVersions(r.version, currentVersion()) <= 0)
        return no(tr("OctoMX only installs a newer published release by itself."));
    const mx::upd::Layout l = layout();
    if (!l.ok)
        return no(l.why);
    if (!mx::upd::UpdateDownloader::platformAsset(r, mx::upd::platformTag()))
        return no(tr("The release has no package for this system."));
    if (!r.asset(QStringLiteral("SHA256SUMS")) || !r.asset(QStringLiteral("SHA256SUMS.sig")))
        return no(tr("This release is not signed, so OctoMX does not install it by itself."));
    return true;
}

void UpdateController::startUpdate(const ReleaseInfo &r, bool quiet)
{
    QString why;
    if (!canAutoInstall(r, &why)) {   // also refuses a downgrade or the same version
        failed(r, why, quiet);
        return;
    }
    m_release = r;
    m_quiet = quiet;
    setState(State::Downloading);
    if (!m_download) {
        const QString dir = QDir(m_mw->core()->profileDir()).filePath(QStringLiteral("updates"));
        m_download = new mx::upd::UpdateDownloader(m_mw->core()->settings(), dir, QStringLiteral("OctoMX/") + currentVersion(), this);
        connect(m_download, &mx::upd::UpdateDownloader::finished, this, [this](const mx::upd::UpdateDownloader::Result &res) {
            onDownloaded(m_release, res.ok, res.archivePath, res.error, m_quiet);
        });
        connect(m_download, &mx::upd::UpdateDownloader::progress, this, [this](qint64 done, qint64 total) {
            if (m_progress) {
                m_progress->setRange(0, 1000);
                m_progress->setValue(total > 0 ? int(done * 1000 / total) : 0);
            }
            if (m_progressText)
                m_progressText->setText(tr("%1 of %2 KB").arg(QLocale().toString(done / 1024)).arg(QLocale().toString(total / 1024)));
        });
    }
    qInfo("octomx: update: downloading %s (%s)", qPrintable(r.tag), quiet ? "in the background" : "Update Now");
    if (!quiet)
        showProgressBox();
    m_download->start(r, mx::upd::platformTag());
}

void UpdateController::onDownloaded(const ReleaseInfo &r, bool ok, const QString &archive, const QString &error, bool quiet)
{
    if (!ok) {
        failed(r, error, quiet);
        return;
    }
    setState(State::Preparing);
    if (m_progressText)
        m_progressText->setText(tr("Checking and unpacking..."));
    const mx::upd::Layout l = layout();
    const QString stage = mx::upd::stageDirFor(l.programDir, r.version);
    struct Prep {
        bool ok = false;
        QString top, error;
        mx::upd::Manifest manifest;
    };
    auto res = std::make_shared<Prep>();
    QThread *t = QThread::create([res, stage, archive, version = r.version] {
        QDir(stage).removeRecursively();
        QString err;
        if (!mx::upd::unpackArchive(archive, stage, &err)) {
            res->error = QObject::tr("The package could not be unpacked: %1").arg(err);
        } else if (!mx::upd::checkStagedTree(stage, version, mx::upd::platformTag(), &res->top, &res->manifest, &err)) {
            res->error = QObject::tr("The unpacked package failed its check: %1").arg(err);
        } else {
            res->ok = true;
            return;
        }
        QDir(stage).removeRecursively();
    });
    QPointer<UpdateController> self(this);
    connect(t, &QThread::finished, t, &QObject::deleteLater);
    connect(t, &QThread::finished, this, [self, res, r, quiet] {
        if (self)
            self->onPrepared(r, res->ok, res->top, res->manifest, res->error, quiet);
    });
    t->start();
}

void UpdateController::onPrepared(const ReleaseInfo &r, bool ok, const QString &stageTop, const mx::upd::Manifest &m,
                                  const QString &error, bool quiet)
{
    if (!ok) {
        failed(r, error, quiet);
        return;
    }
    const mx::upd::Layout l = layout();
    g_pending.valid = true;
    g_pending.restart = false;
    g_pending.version = r.version;
    g_pending.plan.programDir = l.programDir;
    g_pending.plan.stageTop = stageTop;
    g_pending.plan.fromVersion = currentVersion();
    g_pending.plan.toVersion = r.version;
    g_pending.plan.oldManifest = l.manifest;
    g_pending.plan.newManifest = m;
    qInfo("octomx: update: OctoMX %s is ready to install (%d files staged in %s)", qPrintable(r.version), int(m.files.size()),
          qPrintable(QDir::toNativeSeparators(stageTop)));
    setState(State::Ready);
    if (m_dialog && m_dialog->objectName() == QLatin1String("updateProgressBox"))
        m_dialog->close();
    emit updateReady(r.version);
    showReady();
}

void UpdateController::failed(const ReleaseInfo &r, const QString &error, bool quiet)
{
    Q_UNUSED(quiet);
    qInfo("octomx: update: no automatic install of %s: %s", qPrintable(r.tag), qPrintable(error));
    setState(State::Idle);
    if (m_dialog && m_dialog->objectName() == QLatin1String("updateProgressBox"))
        m_dialog->close();
    emit updateFailed(error);
    // the old way still works: the download page
    offer(r, Kind::Notify, tr("OctoMX could not install it by itself: %1").arg(error));
}

void UpdateController::offer(const ReleaseInfo &r, Kind kind, const QString &reason)
{
    Offer o;
    o.kind = kind;
    o.version = displayVersion(r.version);
    o.current = displayVersion(currentVersion());
    o.pageUrl = releasePageUrl(r);
    o.reason = reason;
    if (kind == Kind::Notify && reason.isEmpty()) {
        o.text = offerText(r, currentVersion(), &o.notes);
    } else if (kind == Kind::Notify) {   // no automatic install here: why, and the download page
        o.notes = notesFor(r);
        o.text = tr("OctoMX %1 is available (you have %2).").arg(o.version, o.current);
        if (!o.notes.isEmpty())
            o.text += QStringLiteral("\n\n") + tr("Release notes:") + QLatin1Char('\n') + o.notes;
        o.text += QStringLiteral("\n\n") + reason + QStringLiteral("\n\n") +
                  tr("Open Download Page shows the release in your web browser, to install it by hand.");
    } else {
        o.notes = notesFor(r);
        o.text = tr("OctoMX %1 is available (you have %2).").arg(o.version, o.current);
        if (!o.notes.isEmpty())
            o.text += QStringLiteral("\n\n") + tr("Release notes:") + QLatin1Char('\n') + o.notes;
        o.text += QStringLiteral("\n\n") + tr("Update Now downloads it, checks its signature and installs it when OctoMX restarts. "
                                              "Your profile, shared folders and downloads stay as they are.");
    }
    const QString version = r.version;
    const ReleaseInfo rel = r;
#ifdef Q_OS_MACOS
    // [mac-1] no install by itself on macOS: the box names the file to take from the download page
    if (kind == Kind::Notify) {
        const ReleaseAsset *a = mx::upd::UpdateDownloader::platformAsset(r, mx::upd::platformTag());
        o.text += QStringLiteral("\n\n") + (a ? tr("The file for this Mac on the download page: %1").arg(a->name)
                                               : tr("This release has no macOS package yet."));
    }
#endif
    showBox(o, [this, o, version, rel](int a) {
        if (a == OpenPage)
            openPage(o.pageUrl);
        else if (a == SkipVersion)
            m_mw->core()->settings()->setValue(updates::SkipVersion, version);
        else if (a == UpdateNow)
            startUpdate(rel, false);
        // Remind Me Later: nothing; the next automatic check is a day later
    });
}

void UpdateController::showReady()
{
    if (m_state != State::Ready)
        return;
    Offer o;
    o.kind = Kind::Ready;
    o.version = displayVersion(m_release.version);
    o.current = displayVersion(currentVersion());
    o.pageUrl = releasePageUrl(m_release);
    o.notes = notesFor(m_release);
    o.text = tr("OctoMX %1 is ready to install (you have %2).").arg(o.version, o.current);
    if (!o.notes.isEmpty())
        o.text += QStringLiteral("\n\n") + tr("Release notes:") + QLatin1Char('\n') + o.notes;
    o.text += QStringLiteral("\n\n") + tr("The download was checked against the OctoMX release signature.");
    const TrayController *tray = TrayController::of(m_mw);
    const int active = tray ? tray->activeDownloads() : 0;
    if (active > 0)
        o.text += QLatin1Char(' ') + (active == 1 ? tr("Your running download continues after the restart.")
                                                  : tr("Your %1 running downloads continue after the restart.").arg(active));
    o.text += QStringLiteral("\n\n") + tr("Restart Now installs it and starts OctoMX again. Later installs it when you close OctoMX.");
    showBox(o, [this](int a) { answerReady(a); });
}

void UpdateController::answerReady(int a)
{
    if (!g_pending.valid)
        return;
    if (a == RestartNow) {
        qInfo("octomx: update: Restart Now (OctoMX %s)", qPrintable(g_pending.version));
        g_pending.restart = true;
        requestQuit();
    } else if (a == SkipVersion) {
        qInfo("octomx: update: OctoMX %s skipped, the staged files are removed", qPrintable(g_pending.version));
        m_mw->core()->settings()->setValue(updates::SkipVersion, g_pending.version);
        QDir(mx::upd::stageDirFor(g_pending.plan.programDir, g_pending.version)).removeRecursively();
        g_pending.valid = false;
        setState(State::Idle);
    } else {   // Later (also: the box closed without an answer)
        qInfo("octomx: update: OctoMX %s is installed when OctoMX closes", qPrintable(g_pending.version));
        if (m_balloonShownFor != g_pending.version) {
            m_balloonShownFor = g_pending.version;
            balloon(tr("OctoMX %1 will be installed when you close OctoMX.").arg(displayVersion(g_pending.version)));
        }
    }
}

void UpdateController::showUpdated()
{
    if (!g_recoveryNote.isEmpty()) {
        const QString note = g_recoveryNote;
        g_recoveryNote.clear();
        message(note, true);
        return;
    }
    if (g_updatedTo.isEmpty())
        return;
    Offer o;
    o.kind = Kind::Updated;
    o.version = displayVersion(g_updatedTo);
    o.current = displayVersion(g_updatedFrom);
    o.pageUrl = releaseTagPage(g_updatedTo);
    o.text = tr("Updated to OctoMX %1 (from %2).").arg(o.version, o.current) + QStringLiteral("\n\n") +
             tr("What's New shows the release notes in your web browser.");
    g_updatedFrom.clear();
    g_updatedTo.clear();
    showBox(o, [this, o](int a) {
        if (a == OpenPage)
            openPage(o.pageUrl);
    });
}

void UpdateController::showBox(const Offer &o, const std::function<void(int)> &answer)
{
    if (s_offerHook) {
        answer(s_offerHook(o));
        return;
    }
    if (m_box)
        m_box->close();
    if (m_dialog)
        m_dialog->close();
    QWidget *parent = m_mw->isVisible() ? m_mw : nullptr;
    if (o.kind == Kind::Notify) {   // the classic message box
        auto *box = new QMessageBox(QMessageBox::Information, tr("OctoMX"), o.text, QMessageBox::NoButton, parent);
        box->setIconPixmap(informationPixmap(S(32)));   // [ui-h]
        box->setObjectName(QStringLiteral("updateBox"));
        box->setTextFormat(Qt::PlainText);   // the release notes are never interpreted as HTML
        box->setWindowIcon(appIcon());
        box->setAttribute(Qt::WA_DeleteOnClose);
        QPushButton *open = box->addButton(tr("Open Download Page"), QMessageBox::AcceptRole);
        open->setObjectName(QStringLiteral("updateOpen"));
        open->setToolTip(tr("Open the download page of the new version in your web browser."));
        QPushButton *later = box->addButton(tr("Remind Me Later"), QMessageBox::RejectRole);
        later->setObjectName(QStringLiteral("updateLater"));
        later->setToolTip(tr("Close this message. OctoMX asks again at the next check, at most once a day."));
        QPushButton *skip = box->addButton(tr("Skip This Version"), QMessageBox::DestructiveRole);
        skip->setObjectName(QStringLiteral("updateSkip"));
        skip->setToolTip(tr("Do not tell about this version again. You hear about the next newer one."));
        box->setDefaultButton(open);
        box->setEscapeButton(later);
        connect(box, &QMessageBox::buttonClicked, this, [answer, open, skip](QAbstractButton *b) {
            answer(b == open ? OpenPage : b == skip ? SkipVersion : RemindLater);
        });
        m_box = box;
        box->open();
        return;
    }
    // Ask / Ready / Updated: a small classic box of our own (a link and, for Update Now, a progress bar)
    auto *d = new QDialog(parent, Qt::Dialog | Qt::MSWindowsFixedSizeDialogHint | Qt::WindowTitleHint | Qt::WindowCloseButtonHint);
    d->setObjectName(o.kind == Kind::Ask ? QStringLiteral("updateBox")
                     : o.kind == Kind::Ready ? QStringLiteral("updateReadyBox")
                                               : QStringLiteral("updatedBox"));
    d->setWindowTitle(tr("OctoMX"));
    d->setWindowIcon(appIcon());
    d->setAttribute(Qt::WA_DeleteOnClose);
    auto *top = new QHBoxLayout;
    top->setSpacing(S(12));
    top->addWidget(boxIcon(d), 0, Qt::AlignTop);
    auto *col = new QVBoxLayout;
    auto *text = new QLabel(o.text, d);
    text->setObjectName(QStringLiteral("updateText"));
    text->setTextFormat(Qt::PlainText);
    text->setWordWrap(true);
    text->setMinimumWidth(S(340));
    text->setMaximumWidth(S(420));
    col->addWidget(text);
    if (o.kind == Kind::Ask) {
        auto *link = new QLabel(QStringLiteral("<a href=\"page\">%1</a>").arg(tr("Open Download Page")), d);
        link->setObjectName(QStringLiteral("updateLink"));
        link->setTextFormat(Qt::RichText);
        link->setToolTip(tr("Open the download page in your web browser and install it by hand instead."));
        const QString url = o.pageUrl;
        connect(link, &QLabel::linkActivated, this, [this, url] { openPage(url); });
        col->addWidget(link);
    }
    top->addLayout(col, 1);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    QList<QPair<PushButton *, int>> list;
    auto add = [&](const QString &t, const char *name, const QString &tip, int a) {
        auto *b = new PushButton(t, d);
        b->setObjectName(QString::fromLatin1(name));
        b->setToolTip(tip);
        b->setMinimumWidth(S(88));
        buttons->addWidget(b);
        list << qMakePair(b, a);
        return b;
    };
    PushButton *def = nullptr;
    int escape = RemindLater;
    if (o.kind == Kind::Ask) {
        def = add(tr("Update Now"), "updateNow",
                  tr("Download the new version, check its signature and install it when OctoMX restarts. Nothing of yours is changed."),
                  UpdateNow);
        add(tr("Remind Me Later"), "updateLater", tr("Close this message. OctoMX asks again at the next check, at most once a day."),
            RemindLater);
        add(tr("Skip This Version"), "updateSkip", tr("Do not tell about this version again. You hear about the next newer one."),
            SkipVersion);
    } else if (o.kind == Kind::Ready) {
        def = add(tr("Restart Now"), "updateRestart",
                  tr("Close OctoMX, install the new version and start it again. Transfers continue after the restart."), RestartNow);
        add(tr("Later"), "updateInstallLater", tr("Keep working. The new version is installed when you close OctoMX."), InstallLater);
        add(tr("Skip This Version"), "updateSkip", tr("Do not install this version. The downloaded files are removed."), SkipVersion);
        escape = InstallLater;
    } else {
        add(tr("What's New"), "updatedWhatsNew", tr("Show the release notes of this version in your web browser."), OpenPage);
        def = add(tr("OK"), "updatedOk", tr("Close this message."), RemindLater);
    }
    buttons->addStretch(1);
    auto *outer = new QVBoxLayout(d);
    outer->setContentsMargins(S(12), S(12), S(12), S(10));
    outer->setSpacing(S(12));
    outer->addLayout(top);
    outer->addLayout(buttons);
    if (def) {
        def->setDefault(true);
        def->setFocus();
    }
    auto done = std::make_shared<bool>(false);
    for (const auto &p : std::as_const(list)) {
        const int a = p.second;
        connect(p.first, &QPushButton::clicked, d, [d, done, answer, a] {
            *done = true;
            d->close();
            answer(a);
        });
    }
    // Escape or the close button: Remind Me Later / Later
    connect(d, &QDialog::finished, this, [done, answer, escape] {
        if (!*done) {
            *done = true;
            answer(escape);
        }
    });
    m_dialog = d;
    d->open();
}

void UpdateController::showProgressBox()
{
    if (s_offerHook)   // tests answer everything without windows
        return;
    if (m_dialog && m_dialog->objectName() == QLatin1String("updateProgressBox")) {
        m_dialog->raise();
        return;
    }
    if (m_dialog)
        m_dialog->close();
    auto *d = new QDialog(m_mw->isVisible() ? m_mw : nullptr, Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint);
    d->setObjectName(QStringLiteral("updateProgressBox"));
    d->setWindowTitle(tr("OctoMX"));
    d->setWindowIcon(appIcon());
    d->setAttribute(Qt::WA_DeleteOnClose);
    auto *top = new QHBoxLayout;
    top->setSpacing(S(12));
    top->addWidget(boxIcon(d), 0, Qt::AlignTop);
    auto *col = new QVBoxLayout;
    auto *text = new QLabel(tr("Downloading OctoMX %1...").arg(displayVersion(m_release.version)), d);
    text->setObjectName(QStringLiteral("updateText"));
    text->setMinimumWidth(S(320));
    col->addWidget(text);
    auto *bar = new QProgressBar(d);
    bar->setObjectName(QStringLiteral("updateProgress"));
    bar->setRange(0, 0);
    bar->setTextVisible(false);
    bar->setFixedHeight(S(16));
    col->addWidget(bar);
    auto *status = new QLabel(tr("Connecting..."), d);
    status->setObjectName(QStringLiteral("updateProgressText"));
    col->addWidget(status);
    top->addLayout(col, 1);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    auto *bg = new PushButton(tr("Hide"), d);
    bg->setObjectName(QStringLiteral("updateHide"));
    bg->setToolTip(tr("Close this window and keep downloading. OctoMX asks to restart when the new version is ready."));
    bg->setMinimumWidth(S(88));
    auto *cancel = new PushButton(tr("Cancel"), d);
    cancel->setObjectName(QStringLiteral("updateCancel"));
    cancel->setToolTip(tr("Stop the download. Nothing is installed."));
    cancel->setMinimumWidth(S(88));
    buttons->addWidget(bg);
    buttons->addWidget(cancel);
    buttons->addStretch(1);
    auto *outer = new QVBoxLayout(d);
    outer->setContentsMargins(S(12), S(12), S(12), S(10));
    outer->setSpacing(S(10));
    outer->addLayout(top);
    outer->addLayout(buttons);
    connect(bg, &QPushButton::clicked, d, &QDialog::close);
    connect(cancel, &QPushButton::clicked, this, [this, d] {
        d->close();
        if (m_download && m_download->isRunning()) {
            qInfo("octomx: update: download cancelled by the user");
            m_quiet = true;
            disconnect(m_download, nullptr, this, nullptr);
            m_download->cancel();
            m_download->deleteLater();
            m_download = nullptr;
            setState(State::Idle);
        }
    });
    m_progress = bar;
    m_progressText = status;
    m_dialog = d;
    d->open();
}

void UpdateController::showDemoBox(const QString &kind, const ReleaseInfo &r)
{
    if (kind == QLatin1String("notify")) {
        offer(r, Kind::Notify);
    } else if (kind == QLatin1String("ask")) {
        offer(r, Kind::Ask);
    } else if (kind == QLatin1String("failed")) {
        offer(r, Kind::Notify, tr("OctoMX could not install it by itself: %1")
                                   .arg(tr("The downloaded package does not match the signed checksum list: it was changed or damaged.")));
    } else if (kind == QLatin1String("progress")) {
        m_release = r;
        showProgressBox();
        if (m_progress) {
            m_progress->setRange(0, 1000);
            m_progress->setValue(420);
        }
        if (m_progressText)
            m_progressText->setText(tr("%1 of %2 KB").arg(QLocale().toString(9216)).arg(QLocale().toString(21934)));
    } else if (kind == QLatin1String("ready")) {
        const State keep = m_state;
        const ReleaseInfo keepR = m_release;
        m_release = r;
        m_state = State::Ready;
        const bool had = g_pending.valid;
        g_pending.valid = false;   // a picture only: no answer installs anything
        showReady();
        m_state = keep;
        m_release = keepR;
        g_pending.valid = had;
    } else if (kind == QLatin1String("updated")) {
        g_updatedFrom = currentVersion();
        g_updatedTo = r.version;
        showUpdated();
    }
}

void UpdateController::message(const QString &text, bool warning)
{
    if (s_messageHook) {
        s_messageHook(text);
        return;
    }
    auto *box = new QMessageBox(warning ? QMessageBox::Warning : QMessageBox::Information, tr("OctoMX"), text, QMessageBox::Ok,
                                m_mw->isVisible() ? m_mw : nullptr);
    box->setObjectName(QStringLiteral("updateMessage"));
    if (!warning)
        box->setIconPixmap(informationPixmap(S(32)));   // [ui-h] the same sign on every platform
    box->setTextFormat(Qt::PlainText);
    box->setWindowIcon(appIcon());
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->open();
}

void UpdateController::openPage(const QString &url)
{
    if (s_openUrlHook) {
        s_openUrlHook(url);
        return;
    }
    if (qEnvironmentVariableIsSet("MX_LAUNCH_LOG") || qEnvironmentVariableIsSet("MX_OS_ROOT")) {   // scripted runs record instead
        qInfo("octomx: would open %s", qPrintable(url));
        return;
    }
    QDesktopServices::openUrl(QUrl(url));
}

void UpdateController::balloon(const QString &text)
{
    qInfo("octomx: update: %s", qPrintable(text));
    if (s_balloonHook) {
        s_balloonHook(text);
        return;
    }
    if (TrayController *tray = TrayController::of(m_mw))
        if (QSystemTrayIcon *icon = tray->trayIcon())
            if (icon->isVisible() && QSystemTrayIcon::supportsMessages())
                icon->showMessage(tr("OctoMX update"), text, appIcon(), 8000);
}

void UpdateController::requestQuit()
{
    if (s_quitHook) {
        s_quitHook();
        return;
    }
    if (TrayController *tray = TrayController::of(m_mw))
        tray->quitNow();
    else
        emit m_mw->quitRequested();
}

// ======================================================================= start-up and exit

mx::upd::Layout UpdateController::layout()
{
    if (!g_layoutKnown) {
        g_layout = mx::upd::detectLayout(QCoreApplication::applicationDirPath());
        g_layoutKnown = true;
    }
    return g_layout;
}

void UpdateController::setLayoutForTests(const mx::upd::Layout &l)
{
    g_layout = l;
    g_layoutKnown = true;
}

void UpdateController::resetLayout()
{
    g_layoutKnown = false;
    g_pending = Pending();
}

void UpdateController::setRestartArguments(const QStringList &args)
{
    g_restartArgs = args;
    g_argsKnown = true;
}

bool UpdateController::hasPendingInstall()
{
    return g_pending.valid;
}

QString UpdateController::justUpdatedFrom()
{
    return g_updatedFrom;
}

QString UpdateController::justUpdatedTo()
{
    return g_updatedTo;
}

void UpdateController::waitForPreviousProcess()
{
    const qint64 pid = qEnvironmentVariable("MX_UPDATE_WAIT_PID").toLongLong();
    qunsetenv("MX_UPDATE_WAIT_PID");
    if (pid <= 0 || pid == QCoreApplication::applicationPid())
        return;
    QElapsedTimer t;
    t.start();
#ifdef Q_OS_WIN
    if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid))) {
        WaitForSingleObject(h, 60000);
        CloseHandle(h);
    }
#else
    while (t.elapsed() < 60000 && !(::kill(pid_t(pid), 0) == -1 && errno == ESRCH))
        QThread::msleep(50);
#endif
    g_waited = QStringLiteral("waited %1 ms for the previous OctoMX (pid %2) to end").arg(t.elapsed()).arg(pid);
}

bool UpdateController::startupRecovery()
{
    if (!g_waited.isEmpty())
        qInfo("octomx: update: %s", qPrintable(g_waited));
    const QString prog = mx::upd::programDirOf(QCoreApplication::applicationDirPath());
    if (prog.isEmpty())
        return false;
    const mx::upd::Recovery rec = mx::upd::recoverInstall(prog);
    g_layoutKnown = false;
    const QString cur = QStringLiteral(MX_VERSION_FULL);
    switch (rec.what) {
    case mx::upd::Recovery::Nothing:
        return false;
    case mx::upd::Recovery::Busy:
        qInfo("octomx: update: another OctoMX is updating %s", qPrintable(QDir::toNativeSeparators(prog)));
        return false;
    case mx::upd::Recovery::Cleaned:
    case mx::upd::Recovery::Finished:
        if (!rec.to.isEmpty())
            qInfo("octomx: update: OctoMX %s replaced %s in %s (%s), old files removed", qPrintable(rec.to), qPrintable(rec.from),
                  qPrintable(QDir::toNativeSeparators(prog)), rec.what == mx::upd::Recovery::Finished ? "finished at this start" : "done");
        else
            qInfo("octomx: update: removed leftover update folders in %s", qPrintable(QDir::toNativeSeparators(prog)));
        if (!rec.to.isEmpty() && compareVersions(rec.to, cur) == 0) {
            g_updatedFrom = rec.from;
            g_updatedTo = rec.to;
        }
        break;
    case mx::upd::Recovery::RolledBack:
        qWarning("octomx: update: the interrupted update to %s was undone (%s)", qPrintable(rec.to), qPrintable(rec.error));
        g_recoveryNote = QObject::tr("The update to OctoMX %1 could not be finished and was undone. OctoMX %2 is installed.\n\n"
                                     "OctoMX offers the update again at the next check.")
                             .arg(displayVersion(rec.to), displayVersion(rec.from));
        break;
    case mx::upd::Recovery::Failed:
        qWarning("octomx: update: the interrupted update to %s could not be finished: %s", qPrintable(rec.to), qPrintable(rec.error));
        return false;
    }
    if (rec.moved > 0 && !qEnvironmentVariableIsSet("MX_UPDATE_RECOVERED")) {
        // files were moved under the running program: start the version that is now in place
        qputenv("MX_UPDATE_RECOVERED", "1");
        const QString exe = QDir(prog).filePath(mx::upd::programFile());
        return startProgram(exe, restartArguments());
    }
    return false;
}

bool UpdateController::finishOnExit()
{
    if (!g_pending.valid)
        return false;
    const Pending p = g_pending;
    g_pending = Pending();
    qInfo("octomx: update: installing OctoMX %s into %s", qPrintable(p.version), qPrintable(QDir::toNativeSeparators(p.plan.programDir)));
    QString err;
    if (!mx::upd::swapInstall(p.plan, &err)) {
        qWarning("octomx: update: OctoMX %s was not installed: %s", qPrintable(p.version), qPrintable(err));
        const QString text = QObject::tr("OctoMX %1 could not be installed: %2").arg(displayVersion(p.version), err);
        if (s_messageHook) {
            s_messageHook(text);
        } else if (qobject_cast<QApplication *>(QCoreApplication::instance())) {
            QMessageBox box(QMessageBox::Warning, QObject::tr("OctoMX"), text, QMessageBox::Ok);
            box.setTextFormat(Qt::PlainText);
            QTimer::singleShot(60000, &box, &QMessageBox::accept);
            box.exec();
        }
        return false;
    }
    qInfo("octomx: update: OctoMX %s installed (old files in .old-%s, removed at the next start)", qPrintable(p.version),
          qPrintable(p.plan.fromVersion));
    if (!p.restart)
        return false;
    const mx::upd::Layout l = layout();
    return startProgram(l.exe.isEmpty() ? QDir(p.plan.programDir).filePath(mx::upd::programFile())
                                        : l.exe,
                        restartArguments());
}

} // namespace mx::ui

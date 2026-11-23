// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/script_driver.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QTextStream>
#include <QtCore/QElapsedTimer>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QPainter>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QApplication>
#include <QtWidgets/QDialog>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMenu>
#include <QtWidgets/QPlainTextEdit>

#include "mx/services.h"
#include "ui/kit/list_view.h"
#include "ui/kit/status_bar.h"
#include "ui/main_window.h"
#include "ui/pages/page.h"

namespace mx::ui {

ScriptDriver::ScriptDriver(MainWindow *mw, QObject *parent) : QObject(parent), m_main(mw) {}

static QList<ScriptDriver::CommandHandler> &commandHandlers()
{
    static QList<ScriptDriver::CommandHandler> h;
    return h;
}

void ScriptDriver::addCommandHandler(CommandHandler h)
{
    commandHandlers() << h;
}

bool ScriptDriver::loadFile(const QString &path, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error)
            *error = QStringLiteral("cannot open script %1").arg(path);
        return false;
    }
    QTextStream ts(&f);
    m_lines.clear();
    while (!ts.atEnd())
        m_lines << ts.readLine();
    if (m_baseDir.isEmpty())
        m_baseDir = QFileInfo(path).absolutePath();
    return true;
}

void ScriptDriver::start(int initialDelayMs)
{
    m_pos = 0;
    QTimer::singleShot(initialDelayMs, this, &ScriptDriver::step);
}

void ScriptDriver::step()
{
    if (m_done)
        return;
    while (m_pos < m_lines.size()) {
        const QString line = m_lines.at(m_pos).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
            ++m_pos;
            continue;
        }
        break;
    }
    if (m_pos >= m_lines.size()) {
        m_done = true;
        emit finished(m_errors.isEmpty() ? 0 : 2);
        return;
    }
    const QString line = m_lines.at(m_pos++).trimmed();
    int delay = 20;
    QString err;
    // Schedule the next step first: a command may open a modal dialog (nested event loop); the
    // script must keep running inside it.
    auto *t = new QTimer(this);
    t->setSingleShot(true);
    bool ok = true;
    connect(t, &QTimer::timeout, this, [this, t] {
        t->deleteLater();
        step();
    });
    // the delay is only known after parsing; parse "wait" up-front
    if (line.startsWith(QLatin1String("wait "))) {
        t->start(qMax(0, line.mid(5).trimmed().toInt()));
        return;
    }
    if (line == QLatin1String("quit")) {
        t->deleteLater();
        m_done = true;
        emit finished(m_errors.isEmpty() ? 0 : 2);
        return;
    }
    t->start(delay);
    s_current = this;
    ok = exec(line, &err, &delay);
    s_current = nullptr;
    if (ok && m_holdReady) {
        // [ui-e] the command asked to wait: no next step until ready() (or the timeout)
        t->stop();
        t->deleteLater();
        auto ready = m_holdReady;
        m_holdReady = nullptr;
        auto *poll = new QTimer(this);
        auto *clock = new QElapsedTimer;
        clock->start();
        const int timeout = m_holdTimeout;
        const QString fail = m_holdFail;
        const int pos = m_pos;
        connect(poll, &QTimer::timeout, this, [this, poll, clock, ready, timeout, fail, pos, line] {
            const bool done = ready();
            if (!done && clock->elapsed() < timeout)
                return;
            poll->stop();
            poll->deleteLater();
            delete clock;
            if (!done) {
                const QString msg = QStringLiteral("script line %1: %2: %3").arg(pos).arg(line, fail);
                m_errors << msg;
                qWarning("%s", qPrintable(msg));
                if (m_stopOnError) {
                    m_done = true;
                    emit finished(2);
                    return;
                }
            }
            step();
        });
        poll->start(300);
        return;
    }
    if (!ok) {
        const QString msg = QStringLiteral("script line %1: %2: %3").arg(m_pos).arg(line, err);
        m_errors << msg;
        qWarning("%s", qPrintable(msg));
        if (m_stopOnError) {
            t->stop();
            t->deleteLater();
            m_done = true;
            emit finished(2);
        }
    }
}

ScriptDriver *ScriptDriver::s_current = nullptr;

ScriptDriver *ScriptDriver::current()
{
    return s_current;
}

void ScriptDriver::holdUntil(std::function<bool()> ready, int timeoutMs, const QString &failMessage)
{
    m_holdReady = std::move(ready);
    m_holdTimeout = timeoutMs;
    m_holdFail = failMessage;
}

QWidget *ScriptDriver::findWidget(MainWindow *mw, const QString &name)
{
    if (mw->objectName() == name)
        return mw;
    // visible widgets first (several search tabs share object names)
    QWidget *fallback = nullptr;
    QList<QWidget *> roots;
    roots << mw;
    for (QWidget *w : QApplication::topLevelWidgets())
        if (w != mw && w->isVisible())
            roots << w;
    for (QWidget *r : roots) {
        if (r->objectName() == name && r->isVisible())
            return r;
        const QList<QWidget *> all = r->findChildren<QWidget *>(name);
        for (QWidget *w : all) {
            if (w->isVisible())
                return w;
            if (!fallback)
                fallback = w;
        }
    }
    return fallback;
}

MxListView *ScriptDriver::list(const QString &name, QString *error) const
{
    auto *l = qobject_cast<MxListView *>(findWidget(m_main, name));
    if (!l && error)
        *error = QStringLiteral("no list view named %1").arg(name);
    return l;
}

static QMenu *activeMenu()
{
    if (auto *m = qobject_cast<QMenu *>(QApplication::activePopupWidget()))
        return m;
    for (QWidget *w : QApplication::topLevelWidgets())
        if (auto *m = qobject_cast<QMenu *>(w); m && m->isVisible())
            return m;
    return nullptr;
}

static QString stripAmp(QString s)
{
    s.remove(QLatin1Char('&'));
    return s;
}

static void sendKey(QWidget *w, const QString &keyName)
{
    const QKeySequence seq = QKeySequence::fromString(keyName, QKeySequence::PortableText);
    if (seq.isEmpty())
        return;
    const QKeyCombination kc = seq[0];
    QString text;
    if (kc.key() == Qt::Key_Return || kc.key() == Qt::Key_Enter)
        text = QStringLiteral("\r");
    QKeyEvent press(QEvent::KeyPress, kc.key(), kc.keyboardModifiers(), text);
    QKeyEvent release(QEvent::KeyRelease, kc.key(), kc.keyboardModifiers(), text);
    QCoreApplication::sendEvent(w, &press);
    QCoreApplication::sendEvent(w, &release);
}

bool ScriptDriver::exec(const QString &line, QString *error, int *delay)
{
    Q_UNUSED(delay);
    const QString verb = line.section(QLatin1Char(' '), 0, 0).toLower();
    const QString rest = line.section(QLatin1Char(' '), 1).trimmed();
    const QStringList args = rest.split(QLatin1Char(' '), Qt::SkipEmptyParts);

    if (verb == QLatin1String("page")) {
        if (!MainWindow::pageIds().contains(rest) && rest != QLatin1String("hotlist")) {
            *error = QStringLiteral("unknown page");
            return false;
        }
        m_main->showPage(rest);
        return true;
    }
    if (verb == QLatin1String("subtab")) {
        Page *p = m_main->page(m_main->currentPageId());
        if (!p || !p->selectSubTab(rest)) {
            *error = QStringLiteral("unknown sub-tab");
            return false;
        }
        return true;
    }
    if (verb == QLatin1String("hotlist")) {
        m_main->setHotlistVisible(rest == QLatin1String("on"));
        return true;
    }
    if (verb == QLatin1String("resize")) {
        if (args.size() != 2) {
            *error = QStringLiteral("usage: resize W H");
            return false;
        }
        m_main->resize(args[0].toInt(), args[1].toInt());
        return true;
    }
    if (verb == QLatin1String("float")) {
        m_main->floatPage(rest.isEmpty() ? m_main->currentPageId() : rest);
        return true;
    }
    if (verb == QLatin1String("dock")) {
        m_main->dockPage(rest.isEmpty() ? m_main->currentPageId() : rest);
        return true;
    }
    if (verb == QLatin1String("set")) {
        const QString key = args.value(0);
        const QString value = rest.section(QLatin1Char(' '), 1);
        m_main->core()->settings()->setValue(key, value);
        return true;
    }
    if (verb == QLatin1String("echo")) {
        qInfo("%s", qPrintable(rest));
        return true;
    }
    if (verb == QLatin1String("shot")) {
        QString file = rest;
        if (QFileInfo(file).isRelative() && !m_baseDir.isEmpty())
            file = QDir(m_baseDir).filePath(file);
        return shot(m_main, file, error);
    }
    if (verb == QLatin1String("type") || verb == QLatin1String("settext")) {
        const QString name = args.value(0);
        const QString text = rest.section(QLatin1Char(' '), 1);
        QWidget *w = findWidget(m_main, name);
        if (auto *le = qobject_cast<QLineEdit *>(w)) {
            le->setFocus();
            le->setText(text);
            return true;
        }
        if (auto *pe = qobject_cast<QPlainTextEdit *>(w)) {
            pe->setFocus();
            pe->setPlainText(text);
            return true;
        }
        *error = QStringLiteral("no text field named %1").arg(name);
        return false;
    }
    if (verb == QLatin1String("key")) {
        QWidget *w = findWidget(m_main, args.value(0));
        if (!w) {
            *error = QStringLiteral("no widget named %1").arg(args.value(0));
            return false;
        }
        w->setFocus();
        sendKey(w, args.value(1));
        return true;
    }
    if (verb == QLatin1String("click")) {
        QWidget *w = findWidget(m_main, rest);
        if (!w) {
            *error = QStringLiteral("no widget named %1").arg(rest);
            return false;
        }
        if (auto *b = qobject_cast<QAbstractButton *>(w)) {
            QTimer::singleShot(0, b, [b] { b->click(); });
            return true;
        }
        if (auto *ft = qobject_cast<FloatTab *>(w)) {
            QTimer::singleShot(0, ft, [ft] { emit ft->clicked(); });
            return true;
        }
        const QPoint c = w->rect().center();
        QMouseEvent press(QEvent::MouseButtonPress, c, w->mapToGlobal(c), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent rel(QEvent::MouseButtonRelease, c, w->mapToGlobal(c), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(w, &press);
        QCoreApplication::sendEvent(w, &rel);
        return true;
    }
    if (verb == QLatin1String("select-row") || verb == QLatin1String("dblclick-row") || verb == QLatin1String("expand-row") ||
        verb == QLatin1String("collapse-row") || verb == QLatin1String("context-menu")) {
        MxListView *l = list(args.value(0), error);
        if (!l)
            return false;
        const QModelIndex idx = l->visibleRow(args.value(1).toInt());
        if (!idx.isValid()) {
            *error = QStringLiteral("row %1 out of range (%2 visible rows)").arg(args.value(1)).arg(l->visibleRowCount());
            return false;
        }
        if (verb == QLatin1String("select-row")) {
            l->selectRow(idx);
        } else if (verb == QLatin1String("dblclick-row")) {
            l->selectRow(idx);
            QTimer::singleShot(0, l, [l, idx = QPersistentModelIndex(idx)] {
                if (idx.isValid())
                    emit l->rowActivated(idx);
            });
        } else if (verb == QLatin1String("expand-row")) {
            l->expand(idx);
        } else if (verb == QLatin1String("collapse-row")) {
            l->collapse(idx);
        } else {
            l->selectRow(idx);
            const QRect r = l->visualRect(idx);
            const QPoint g = l->viewport()->mapToGlobal(QPoint(r.left() + 40, r.center().y()));
            emit l->contextMenuAt(idx, g);
        }
        return true;
    }
    if (verb == QLatin1String("menu-pick")) {
        QMenu *m = activeMenu();
        if (!m) {
            *error = QStringLiteral("no open menu");
            return false;
        }
        for (QAction *a : m->actions()) {
            if (stripAmp(a->text()) == rest) {
                if (a->menu()) {
                    m->setActiveAction(a);
                    a->menu()->popup(m->mapToGlobal(m->actionGeometry(a).topRight()));
                } else {
                    m->hide();
                    QTimer::singleShot(0, a, [a] { a->trigger(); });
                    // close the parent chain
                    for (QWidget *w : QApplication::topLevelWidgets())
                        if (auto *mm = qobject_cast<QMenu *>(w); mm && mm->isVisible())
                            mm->hide();
                }
                return true;
            }
        }
        *error = QStringLiteral("menu has no item '%1'").arg(rest);
        return false;
    }
    if (verb == QLatin1String("close-menus")) {
        for (QWidget *w : QApplication::topLevelWidgets())
            if (auto *mm = qobject_cast<QMenu *>(w); mm && mm->isVisible())
                mm->hide();
        return true;
    }
    if (verb == QLatin1String("close-dialogs")) {
        for (QWidget *w : QApplication::topLevelWidgets())
            if (auto *d = qobject_cast<QDialog *>(w); d && d->isVisible())
                d->reject();
        return true;
    }
    if (verb == QLatin1String("expect-rows")) {
        MxListView *l = list(args.value(0), error);
        if (!l)
            return false;
        const int n = l->visibleRowCount();
        if (n < args.value(1).toInt()) {
            *error = QStringLiteral("expected at least %1 rows, have %2").arg(args.value(1)).arg(n);
            return false;
        }
        return true;
    }
    // verbs registered by other UI parts ([ui-c]: settings, setvalue, tray-menu, ...)
    for (CommandHandler h : commandHandlers())
        if (h(m_main, verb, rest, args, error))
            return error->isEmpty();
    // page-specific commands
    if (Page *p = m_main->page(m_main->currentPageId())) {
        if (p->scriptCommand(verb, args, error))
            return error->isEmpty();
    }
    *error = QStringLiteral("unknown command");
    return false;
}

bool ScriptDriver::shot(MainWindow *mw, const QString &file, QString *error)
{
    QDir().mkpath(QFileInfo(file).absolutePath());
    QPixmap pm = mw->grab();
    const QPoint origin = mw->mapToGlobal(QPoint(0, 0));
    QPainter p(&pm);
    const QString base = QFileInfo(file).absolutePath() + QLatin1Char('/') + QFileInfo(file).completeBaseName();
    // menus in stacking order: parents before submenus
    QList<QMenu *> menus;
    for (QWidget *w : QApplication::topLevelWidgets())
        if (auto *m = qobject_cast<QMenu *>(w); m && m->isVisible())
            menus << m;
    std::sort(menus.begin(), menus.end(), [](QMenu *a, QMenu *b) {
        int da = 0, db = 0;
        for (QObject *o = a->parent(); o; o = o->parent()) ++da;
        for (QObject *o = b->parent(); o; o = o->parent()) ++db;
        return da < db;
    });
    int menuNo = 0;
    for (QMenu *m : std::as_const(menus)) {
        const QPixmap mp = m->grab();
        p.drawPixmap(m->mapToGlobal(QPoint(0, 0)) - origin, mp);
        // [qa-1] each popup also on its own (<file>.menu<N>.png, parents first) for menu comparisons
        mp.save(base + QStringLiteral(".menu%1.png").arg(++menuNo));
    }
    // [ui-g] a visible tool tip (verb "tooltip") is drawn over the window too
    auto isTip = [](QWidget *w) { return qstrcmp(w->metaObject()->className(), "QTipLabel") == 0; };
    for (QWidget *w : QApplication::topLevelWidgets())
        if (w->isVisible() && isTip(w))
            p.drawPixmap(w->mapToGlobal(QPoint(0, 0)) - origin, w->grab());
    p.end();
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (w == mw || !w->isVisible() || qobject_cast<QMenu *>(w) || isTip(w))
            continue;
        if (w->isWindow()) {
            const QString name = w->objectName().isEmpty() ? QString::fromLatin1(w->metaObject()->className()) : w->objectName();
            w->grab().save(base + QLatin1Char('.') + name + QStringLiteral(".png"));
        }
    }
    if (!pm.save(file)) {
        if (error)
            *error = QStringLiteral("cannot write %1").arg(file);
        return false;
    }
    return true;
}

} // namespace mx::ui

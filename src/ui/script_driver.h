// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: the --script driver: simple UI automation for tests and screenshots.
// One command per line, '#' starts a comment. Commands run one after another with a short gap
// (20 ms); `wait` adds a delay. Widgets are found by objectName in the main window and in every
// other top-level window (dialogs, floating pages, popup menus). Verbs: see src/ui/README.md.
#pragma once

#include <functional>

#include <QtCore/QObject>
#include <QtCore/QStringList>

class QWidget;

namespace mx::ui {

class MainWindow;
class MxListView;

class ScriptDriver : public QObject {
    Q_OBJECT
public:
    explicit ScriptDriver(MainWindow *mw, QObject *parent = nullptr);

    bool loadFile(const QString &path, QString *error = nullptr);
    void setLines(const QStringList &lines) { m_lines = lines; }
    void setBaseDir(const QString &dir) { m_baseDir = dir; }   // relative shot paths resolve here
    void start(int initialDelayMs = 300);
    // stop on the first failing command (default) or just log and continue
    void setStopOnError(bool s) { m_stopOnError = s; }
    QStringList errors() const { return m_errors; }

    // grab the main window with every visible popup menu composited on top; visible dialogs are
    // saved next to `file` as <base>.<dialogObjectName>.png
    static bool shot(MainWindow *mw, const QString &file, QString *error = nullptr);
    static QWidget *findWidget(MainWindow *mw, const QString &objectName);
    // [ui-c] extra verbs from other UI parts: return true if the verb was handled (set *error on failure)
    using CommandHandler = bool (*)(MainWindow *mw, const QString &verb, const QString &rest, const QStringList &args, QString *error);
    static void addCommandHandler(CommandHandler h);

    // [ui-e] a command that must wait for something (a real download): the script pauses until
    // ready() is true (polled every 300 ms) or fails after timeoutMs. Only valid during a command.
    static ScriptDriver *current();
    void holdUntil(std::function<bool()> ready, int timeoutMs, const QString &failMessage);

signals:
    void finished(int exitCode);

private:
    void step();
    std::function<bool()> m_holdReady;
    int m_holdTimeout = 0;
    QString m_holdFail;
    static ScriptDriver *s_current;
    bool exec(const QString &line, QString *error, int *delay);
    MxListView *list(const QString &name, QString *error) const;

    MainWindow *m_main;
    QStringList m_lines;
    QString m_baseDir;
    int m_pos = 0;
    bool m_stopOnError = true;
    bool m_done = false;
    QStringList m_errors;
};

} // namespace mx::ui

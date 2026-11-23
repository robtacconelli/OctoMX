// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX core: tracks uploads reported by every plugin (NetworkPlugin::uploadChanged /
// uploadFinished) for the lower Transfers pane. Final implementation (not part of the engine stub).
// Ids are made globally unique: "up:<net short name>:<plugin upload id>".
// Updates are coalesced to ≤ 2 per second per upload. Finished uploads become Completed (or keep
// Error/Cancelled) and are removed after transfers/clearCompleteUploadsSec when transfers/autoClearUploads is on.
// [up-1] Their text is "Complete" only when the whole file was sent; any other normal end reads
// "Finished" or the plugin's own last word ("Cancelled by remote ...", "Stopped by the other side: ...").
#pragma once

#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QTimer>

#include "mx/plugin.h"
#include "mx/types.h"

namespace mx {

class Library;

class UploadTracker : public QObject {
    Q_OBJECT
public:
    explicit UploadTracker(Settings *settings, QObject *parent = nullptr);

    void attach(NetworkPlugin *plugin);          // listen to its upload signals
    void setLibrary(Library *library) { m_library = library; }   // per-file upload statistics

    QList<TransferInfo> uploads() const;
    bool upload(const QString &id, TransferInfo *out) const;
    void clearUpload(const QString &id);         // remove a finished row (or cancel + remove a running one)
    void cancelUpload(const QString &id);        // ask the plugin to abort, then remove
    bool startNow(const QString &id);            // [glue-2] NetworkPlugin::startUploadNow
    bool setPriority(const QString &id, int priority);   // [glue-2] NetworkPlugin::setUploadPriority
    void clearFinished();
    int activeCount() const;
    // [up-1] the status of an upload that ended normally without sending the whole file
    static QString finishedUploadText() { return QStringLiteral("Finished"); }

signals:
    void uploadAdded(const mx::TransferInfo &t);
    void uploadChanged(const mx::TransferInfo &t);
    void uploadRemoved(const QString &id);

private:
    struct Item {
        TransferInfo info;
        QPointer<NetworkPlugin> plugin;
        QString pluginId;
        bool finished = false;
        bool dirty = false;
    };
    void onChanged(NetworkPlugin *p, const TransferInfo &t);
    void onFinished(NetworkPlugin *p, const QString &pluginId);
    void remove(const QString &id);
    static QString makeId(NetworkPlugin *p, const QString &pluginId);

    Settings *m_settings;
    Library *m_library = nullptr;
    QHash<QString, Item> m_items;
    QStringList m_order;
    QTimer m_flush;
};

} // namespace mx

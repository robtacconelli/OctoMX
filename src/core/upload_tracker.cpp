// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/upload_tracker.h"
#include "core/library.h"

#include "mx/settings_keys.h"

namespace mx {

UploadTracker::UploadTracker(Settings *settings, QObject *parent)
    : QObject(parent)
    , m_settings(settings)
{
    m_flush.setInterval(500);
    connect(&m_flush, &QTimer::timeout, this, [this] {
        bool any = false;
        for (const QString &id : std::as_const(m_order)) {
            Item &it = m_items[id];
            if (!it.dirty) continue;
            it.dirty = false;
            any = true;
            emit uploadChanged(it.info);
        }
        if (!any) m_flush.stop();
    });
}

QString UploadTracker::makeId(NetworkPlugin *p, const QString &pluginId)
{
    return QStringLiteral("up:") + netShortName(p->net()) + QLatin1Char(':') + pluginId;
}

void UploadTracker::attach(NetworkPlugin *plugin)
{
    if (!plugin) return;
    connect(plugin, &NetworkPlugin::uploadChanged, this, [this, plugin](const TransferInfo &t) { onChanged(plugin, t); });
    connect(plugin, &NetworkPlugin::uploadFinished, this, [this, plugin](const QString &id) { onFinished(plugin, id); });
}

void UploadTracker::onChanged(NetworkPlugin *p, const TransferInfo &t)
{
    const QString id = makeId(p, t.id);
    TransferInfo info = t;
    info.id = id;
    info.isUpload = true;
    if (info.networks.isEmpty()) info.networks << p->net();
    if (info.peer.net == Net::Unknown && !info.peer.id.isEmpty()) info.peer.net = p->net();
    if (info.statusText.isEmpty()) info.statusText = transferStateText(info.state);
    if (!info.added.isValid()) info.added = QDateTime::currentDateTime();
    auto it = m_items.find(id);
    if (it == m_items.end()) {
        Item item;
        item.info = info;
        item.plugin = p;
        item.pluginId = t.id;
        m_items.insert(id, item);
        m_order << id;
        emit uploadAdded(info);
        return;
    }
    if (it->finished) return;
    info.added = it->info.added;
    if (info.priority == 0) info.priority = it->info.priority;   // [glue-2] set via setPriority()
    it->info = info;
    it->dirty = true;
    if (!m_flush.isActive()) m_flush.start();
}

void UploadTracker::onFinished(NetworkPlugin *p, const QString &pluginId)
{
    const QString id = makeId(p, pluginId);
    auto it = m_items.find(id);
    if (it == m_items.end() || it->finished) return;
    if (it->info.done == 0 && it->info.state != TransferState::Error) {
        // [glue-3] a client that only waited in our queue (left, purged, cancelled): no row
        // [core-5] the same for any upload that ended without sending a byte ("Complete 0 of ...")
        remove(id);
        return;
    }
    it->finished = true;
    it->dirty = false;
    TransferInfo &info = it->info;
    info.rateUpBps = 0;
    info.rateDownBps = 0;
    info.etaSec = -1;
    if (info.state != TransferState::Error && info.state != TransferState::Cancelled
        && info.state != TransferState::TimedOut) {
        // [up-1] "Complete" only for an upload that sent the whole file. Any other end is the
        // classic "Finished" (the downloader got what it asked for: one range of a multi-source
        // download, a request of a Gnutella / eD2k client), or the plugin's own last word when its
        // final report already said how it ended ("Finished", "Cancelled by remote ...").
        const bool whole = info.size > 0 && info.done >= info.size;
        const bool pluginFinal = info.state == TransferState::Completed && !info.statusText.isEmpty()
                                 && info.statusText != transferStateText(TransferState::Completed);
        info.state = TransferState::Completed;
        if (whole) info.statusText = transferStateText(TransferState::Completed);
        else if (!pluginFinal) info.statusText = finishedUploadText();
    }
    info.completed = QDateTime::currentDateTime();
    if (m_library && info.done > 0) m_library->recordUpload(info.hashes, info.done);
    emit uploadChanged(info);
    if (m_settings && m_settings->b(keys::transfers::AutoClearUploads)) {
        const int delayMs = qMax(0, m_settings->i(keys::transfers::ClearCompleteUploadsSec)) * 1000;
        QTimer::singleShot(delayMs, this, [this, id] {
            auto i = m_items.constFind(id);
            if (i != m_items.constEnd() && i->finished) remove(id);
        });
    }
}

void UploadTracker::remove(const QString &id)
{
    if (!m_items.remove(id)) return;
    m_order.removeAll(id);
    emit uploadRemoved(id);
}

QList<TransferInfo> UploadTracker::uploads() const
{
    QList<TransferInfo> out;
    for (const QString &id : m_order) out << m_items.value(id).info;
    return out;
}

bool UploadTracker::upload(const QString &id, TransferInfo *out) const
{
    auto it = m_items.constFind(id);
    if (it == m_items.constEnd()) return false;
    if (out) *out = it->info;
    return true;
}

void UploadTracker::cancelUpload(const QString &id)
{
    auto it = m_items.find(id);
    if (it == m_items.end()) return;
    if (!it->finished && it->plugin) it->plugin->cancelUpload(it->pluginId);
    remove(id);
}

bool UploadTracker::startNow(const QString &id)
{
    auto it = m_items.find(id);
    if (it == m_items.end() || it->finished || !it->plugin) return false;
    return it->plugin->startUploadNow(it->pluginId);
}

bool UploadTracker::setPriority(const QString &id, int priority)
{
    auto it = m_items.find(id);
    if (it == m_items.end() || it->finished || !it->plugin) return false;
    const int p = qBound(-1, priority, 1);
    if (!it->plugin->setUploadPriority(it->pluginId, p)) return false;
    it->info.priority = p;
    it->dirty = true;
    if (!m_flush.isActive()) m_flush.start();
    return true;
}

void UploadTracker::clearUpload(const QString &id)
{
    cancelUpload(id);
}

void UploadTracker::clearFinished()
{
    const QStringList ids = m_order;
    for (const QString &id : ids)
        if (m_items.value(id).finished) remove(id);
}

int UploadTracker::activeCount() const
{
    int n = 0;
    for (const Item &i : m_items)
        if (!i.finished) ++n;
    return n;
}

} // namespace mx

// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: the Transfers models (UI_SPEC §3.6, §2.3, §8.7).
// One instance shows the downloads (file rows with their sources as children), another the
// uploads (flat). Rows are updated in place from mx::TransferService signals; source children
// are inserted / removed by count, so selection and expansion survive updates.
#pragma once

#include <QtCore/QAbstractItemModel>
#include <QtCore/QHash>
#include <QtGui/QColor>

#include "mx/types.h"
#include "ui/models/roles.h"

namespace mx {
class TransferService;
class Settings;
}

namespace mx::ui {

namespace transfers {
enum DownloadColumn { DFile = 0, DUser, DSpeed, DBytes, DStatus, DRate, DTimeLeft, DColumnCount };
enum UploadColumn { UFile = 0, UUser, USpeed, UBytes, UStatus, URate, UTimeLeft, UColumnCount };
// colour of a status string / state (UI_SPEC §2.3)
QColor statusColor(const QString &statusText, mx::TransferState st, bool upload);
QColor sourceStatusColor(const mx::TransferSource &s);
QString classicSourceStatus(const QString &raw);   // [qa-1] bare engine peer states -> original WinMX wording
QString displayStatus(const mx::TransferInfo &t);
QString networksText(const QList<mx::Net> &nets);           // [ui-e] "BT+eD2k"
QString sourceNetworkText(const mx::TransferSource &s);     // [ui-e] "eD2k", "OctoMX, DSL", "BT DHT"
bool isFinished(const mx::TransferInfo &t);
bool showsBar(const mx::TransferInfo &t);   // [ui-i] the Status cell of a download row is a bar (map)
} // namespace transfers

class TransferModel : public QAbstractItemModel {
    Q_OBJECT
public:
    TransferModel(mx::TransferService *svc, bool uploads, mx::Settings *settings = nullptr, QObject *parent = nullptr);

    QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation o, int role) const override;
    bool hasChildren(const QModelIndex &parent = QModelIndex()) const override;

    bool isUploads() const { return m_uploads; }
    int transferCount() const { return int(m_rows.size()); }
    const mx::TransferInfo &transferAt(int row) const { return m_rows.at(row); }
    int rowOfId(const QString &id) const { return m_rowOfId.value(id, -1); }
    bool isSourceRow(const QModelIndex &i) const { return i.isValid() && i.internalId() != 0; }
    mx::TransferInfo transferFor(const QModelIndex &i) const;   // the file row's transfer (also for source rows)
    mx::TransferSource sourceFor(const QModelIndex &i) const;

    // "<n> downloads from <u> users at <b> bytes per second total"
    QString summaryText() const;

    // direct feeding (tests); normally connected to the service signals
    void addTransfer(const mx::TransferInfo &t);
    void updateTransfer(const mx::TransferInfo &t);
    void removeTransfer(const QString &id);

public slots:
    void reload();

private:
    QVariant fileData(const mx::TransferInfo &t, int col, int role) const;
    QVariant sourceData(const mx::TransferInfo &t, const mx::TransferSource &s, int col, int role) const;
    QVariant uploadData(const mx::TransferInfo &t, int col, int role) const;
    int childCount(const mx::TransferInfo &t) const { return m_uploads ? 0 : int(t.sources.size()); }
    QString afsAeq(const mx::TransferInfo &t) const;

    mx::TransferService *m_svc;
    mx::Settings *m_settings;
    bool m_uploads;
    QList<mx::TransferInfo> m_rows;
    QHash<QString, int> m_rowOfId;
};

} // namespace mx::ui

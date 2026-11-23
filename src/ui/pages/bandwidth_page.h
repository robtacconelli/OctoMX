// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: the Bandwidth page (UI_SPEC §3.7, §2.5, §8.8; reference
// web/bandwidth_graph_page.png): two stacked-area graphs, incoming on top and outgoing below,
// one pixel column per second, newest at the right, auto-scaled y axis in bytes per second with
// grey grid lines and white labels, the latest value continued as a flat block in the right margin.
//
// Series (bottom -> top) and their colour keys:
//   Incoming Peers ("WPN Incoming Sec")   colors/graphIncomingPeers
//   Outgoing Peers ("WPN Outgoing Sec")   colors/graphOutgoingPeers
//   DHT/UDP Packets ("WPN Packets")       colors/graphPackets
//   Overlay Routing ("WPN Primary")       colors/graphOverlay
//   Server Traffic ("OpenNap Traffic")    colors/graphServerIn / colors/graphServerOut
//   Downloads / Uploads                   colors/graphDown / colors/graphUp
//   Other                                 colors/graphOther
//
// Split of each second (bw::split): the core's BandwidthSample carries the file-data part
// (payloadDownBps / payloadUpBps -> Downloads / Uploads) and every network's total (netDownBps /
// netUpBps); a network's protocol overhead = its total minus its share of the payload (the share
// from the transfers it carries), mapped to the series above; the unexplained rest is "Other".
// [bw-1] On top: the speed-limit row (SpeedLimitRow, ui/speed_limits.h); each graph draws the limit
// in force as a dashed line (in the text colour) with a "Limit ..." label.
// A core that reports only totals gets the older estimate (payload from TransferService, overhead
// from NetworkService::networks() rates). The history backlog is split the same way on page open.
#pragma once

#include <QtCore/QVector>
#include <QtWidgets/QFrame>

#include "mx/types.h"
#include "ui/pages/page.h"

class QSplitter;

namespace mx::ui {

class SpeedLimitRow;

namespace bw {
enum Series { InPeers = 0, OutPeers, Packets, Overlay, Server, Downloads, Uploads, Other, SeriesCount };
QString seriesLabel(int s);
struct Point {
    qint64 timeMs = 0;
    int v[SeriesCount] = {0, 0, 0, 0, 0, 0, 0, 0};
    int total() const;
};
// One second of traffic split into the series. `incoming` selects the down or up direction.
Point split(mx::Core *core, const mx::BandwidthSample &s, bool incoming);
bool isDetailed(const mx::BandwidthSample &s);   // carries payload / per-network numbers
// Scale the proportions of `shape` to the total of `s` (used for the history that predates the
// page, for which only totals are known).
Point reshape(const Point &shape, const mx::BandwidthSample &s, bool incoming);
// y-axis step: the smallest 1-2-5 step that labels `peak` with at most 9 lines
int niceStep(int peak);
} // namespace bw

class BandwidthGraph : public QFrame {
    Q_OBJECT
public:
    BandwidthGraph(bool incoming, QWidget *parent = nullptr);
    void setPoints(const QVector<bw::Point> *pts) { m_pts = pts; update(); }
    // [bw-1] the speed limit in force for this direction (bytes/s, 0 = none): a dashed line + label
    void setLimit(int bps) { if (bps != m_limit) { m_limit = bps; update(); } }
    int limit() const { return m_limit; }
    bool limitLineVisible() const;   // the line is inside the axis (else only the label at the top)
    bool isIncoming() const { return m_incoming; }
    // geometry helpers (also used by the tests)
    QRect plotRect() const;          // grid area: x from the labels to the right margin
    int axisMax() const;             // value of the top grid line for the visible samples
    int axisStep() const;

protected:
    void paintEvent(QPaintEvent *) override;
    bool event(QEvent *e) override;

private:
    QColor seriesColor(int s) const;
    int visibleCount() const;
    int peak() const;
    const QVector<bw::Point> *m_pts = nullptr;
    bool m_incoming;
    int m_limit = 0;
};

class BandwidthPage : public Page {
    Q_OBJECT
public:
    BandwidthPage(mx::Core *core, MainWindow *mw, QWidget *parent = nullptr);
    QStringList statusFields() const override;
    void saveState() override;
    BandwidthGraph *incomingGraph() const { return m_in; }
    BandwidthGraph *outgoingGraph() const { return m_out; }
    const QVector<bw::Point> &incomingPoints() const { return m_inPts; }
    const QVector<bw::Point> &outgoingPoints() const { return m_outPts; }
    QSplitter *splitter() const { return m_split; }
    SpeedLimitRow *limitRow() const { return m_row; }   // [bw-1]

protected:
    void resizeEvent(QResizeEvent *) override;
    void showEvent(QShowEvent *e) override;

private:
    void onSample(const mx::BandwidthSample &s);
    void push(QVector<bw::Point> &v, QVector<bw::Point> &raw, const bw::Point &p);
    void updateLimits();
    SpeedLimitRow *m_row = nullptr;
    QSplitter *m_split;
    bool m_splitRestored = false;
    BandwidthGraph *m_in;
    BandwidthGraph *m_out;
    QVector<bw::Point> m_inPts, m_outPts;      // displayed (rolling average)
    QVector<bw::Point> m_inRaw, m_outRaw;      // last few raw seconds for the average
    int m_lastIn = 0, m_lastOut = 0;
};

} // namespace mx::ui

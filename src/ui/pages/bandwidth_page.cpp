// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/pages/bandwidth_page.h"

#include <QtCore/QHash>
#include <QtGui/QHelpEvent>
#include <QtGui/QPainter>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QToolTip>
#include <QtWidgets/QVBoxLayout>

#include "mx/services.h"
#include "mx/settings_keys.h"
#include "ui/kit/colors.h"
#include "ui/kit/draw.h"
#include "ui/kit/format.h"
#include "ui/kit/theme.h"
#include "ui/kit/scale.h"
#include "ui/kit/widgets.h"
#include "ui/speed_limits.h"

namespace mx::ui {

namespace {
constexpr int kMaxPoints = 3600;
constexpr int kLabelRight = 42;   // labels end here (inside the frame), measured on the reference
constexpr int kPlotLeft = 48;     // grid lines / data start here
constexpr int kRightMargin = 18;  // grid lines stop this far from the right edge
constexpr int kTopLine = 8;       // the top grid line, below the frame's inner top

class FaceSplitter : public ProportionalSplitter {   // [ui-e] keeps the halves' proportions
public:
    using ProportionalSplitter::ProportionalSplitter;

protected:
    QSplitterHandle *createHandle() override
    {
        struct H : QSplitterHandle {
            using QSplitterHandle::QSplitterHandle;
            void paintEvent(QPaintEvent *) override
            {
                QPainter p(this);
                p.fillRect(rect(), chrome::Face);
            }
        };
        return new H(orientation(), this);
    }
};
} // namespace

namespace bw {

QString seriesLabel(int s)
{
    switch (s) {
    case InPeers: return QObject::tr("Incoming Peers");
    case OutPeers: return QObject::tr("Outgoing Peers");
    case Packets: return QObject::tr("DHT/UDP Packets");
    case Overlay: return QObject::tr("Overlay Routing");
    case Server: return QObject::tr("Server Traffic");
    case Downloads: return QObject::tr("Downloads");
    case Uploads: return QObject::tr("Uploads");
    default: return QObject::tr("Other");
    }
}

int Point::total() const
{
    int t = 0;
    for (int x : v)
        t += x;
    return t;
}

namespace {
int seriesOfNet(Net n, bool incoming)
{
    switch (n) {
    case Net::Overlay: return Overlay;
    case Net::Kad: return Packets;
    case Net::BitTorrent:
    case Net::Http: return incoming ? InPeers : OutPeers;
    case Net::ED2K:
    case Net::Gnutella:
    case Net::Web:
    case Net::Rats: return Server;
    default: return Other;
    }
}
} // namespace

bool isDetailed(const BandwidthSample &s)
{
    return !s.netDownBps.isEmpty() || !s.netUpBps.isEmpty() || s.payloadDownBps > 0 || s.payloadUpBps > 0;
}

namespace {
// Which networks carry the file data right now (fraction per network, from the running transfers'
// networks weighted by their rates). Only used to subtract each network's payload from its total.
QHash<int, double> payloadShares(mx::Core *core, bool incoming)
{
    QHash<int, double> w;
    double sum = 0;
    auto account = [&](const TransferInfo &t, int rate) {
        if (rate <= 0)
            return;
        QList<Net> nets = t.networks;
        if (nets.isEmpty() && t.peer.net != Net::Unknown)
            nets << t.peer.net;
        for (Net n : std::as_const(nets)) {
            w[int(n)] += double(rate) / nets.size();
            sum += double(rate) / nets.size();
        }
    };
    for (const TransferInfo &t : core->transfers()->downloads())
        account(t, incoming ? t.rateDownBps : t.rateUpBps);
    if (!incoming)
        for (const TransferInfo &t : core->transfers()->uploads())
            account(t, t.rateUpBps);
    if (sum > 0)
        for (auto it = w.begin(); it != w.end(); ++it)
            it.value() /= sum;
    return w;
}
} // namespace

Point splitDetailed(mx::Core *core, const BandwidthSample &s, bool incoming)
{
    // The core measured it: total, the file-data part and every network's total (incl. its payload).
    Point p;
    p.timeMs = s.timeMs;
    const int total = qMax(0, incoming ? s.downBps : s.upBps);
    const int payload = qBound(0, incoming ? s.payloadDownBps : s.payloadUpBps, total);
    const QHash<Net, int> &nets = incoming ? s.netDownBps : s.netUpBps;
    const QHash<int, double> share = payloadShares(core, incoming);
    qint64 w[SeriesCount] = {0, 0, 0, 0, 0, 0, 0, 0};
    qint64 wsum = 0;
    for (auto it = nets.constBegin(); it != nets.constEnd(); ++it) {
        // the network's payload: its share of the file data (all of it if it is the only carrier)
        double sh = share.value(int(it.key()), 0.0);
        if (share.isEmpty() && nets.size() == 1)
            sh = 1.0;
        const qint64 o = qMax<qint64>(0, qint64(it.value()) - qint64(payload * sh + 0.5));
        const int ser = seriesOfNet(it.key(), incoming);
        if (o <= 0 || ser == Other)
            continue;
        w[ser] += o;
        wsum += o;
    }
    const int room = total - payload;
    const double scale = wsum > room ? double(room) / double(wsum) : 1.0;
    int used = 0;
    for (int i = 0; i < SeriesCount; ++i) {
        if (!w[i])
            continue;
        p.v[i] = int(w[i] * scale);
        used += p.v[i];
    }
    p.v[incoming ? Downloads : Uploads] = payload;
    p.v[Other] = qMax(0, room - used);   // traffic of unknown kind (or from networks without a series)
    return p;
}

Point split(mx::Core *core, const BandwidthSample &s, bool incoming)
{
    if (isDetailed(s))
        return splitDetailed(core, s, incoming);
    // an older core that reports only totals: estimate the split from the transfers and the networks' rates
    Point p;
    p.timeMs = s.timeMs;
    const int total = qMax(0, incoming ? s.downBps : s.upBps);
    // payload of the file transfers, attributed to the networks that carry it
    QHash<int, qint64> payloadByNet;
    qint64 payload = 0;
    auto account = [&](const TransferInfo &t, int rate) {
        if (rate <= 0)
            return;
        payload += rate;
        QList<Net> nets = t.networks;
        if (nets.isEmpty() && t.peer.net != Net::Unknown)
            nets << t.peer.net;
        if (nets.isEmpty())
            return;
        for (Net n : std::as_const(nets))
            payloadByNet[int(n)] += rate / nets.size();
    };
    for (const TransferInfo &t : core->transfers()->downloads())
        account(t, incoming ? t.rateDownBps : t.rateUpBps);   // torrents upload while downloading
    if (!incoming)
        for (const TransferInfo &t : core->transfers()->uploads())
            account(t, t.rateUpBps);
    // 1. protocol overhead per network = the network's rate minus its share of the payload
    qint64 w[SeriesCount] = {0, 0, 0, 0, 0, 0, 0, 0};
    qint64 wsum = 0;
    for (const NetworkStatus &st : core->network()->networks()) {
        const qint64 r = incoming ? st.rateDownBps : st.rateUpBps;
        const qint64 o = qMax<qint64>(0, r - payloadByNet.value(int(st.net)));
        const int ser = seriesOfNet(st.net, incoming);
        if (o <= 0 || ser == Other)
            continue;
        w[ser] += o;
        wsum += o;
    }
    int rest = total;
    if (wsum > 0 && total > 0) {
        const double scale = wsum > total ? double(total) / double(wsum) : 1.0;
        for (int i = 0; i < SeriesCount; ++i) {
            if (!w[i])
                continue;
            p.v[i] = int(w[i] * scale);
            rest -= p.v[i];
        }
        rest = qMax(0, rest);
    }
    // 2. the file transfers, 3. whatever is left
    const int payloadSeries = incoming ? Downloads : Uploads;
    p.v[payloadSeries] = int(qMin<qint64>(rest, payload));
    p.v[Other] = rest - p.v[payloadSeries];
    return p;
}

Point reshape(const Point &shape, const BandwidthSample &s, bool incoming)
{
    // protocol overhead is steady, transfers come and go: keep today's absolute overhead values
    // (scaled down when the old total was smaller) and give the rest to the transfer payload
    Point p;
    p.timeMs = s.timeMs;
    const int total = qMax(0, incoming ? s.downBps : s.upBps);
    const int payloadSeries = incoming ? Downloads : Uploads;
    qint64 overhead = 0;
    for (int i = 0; i < SeriesCount; ++i)
        if (i != Downloads && i != Uploads)
            overhead += shape.v[i];
    const double scale = overhead > total ? double(total) / double(overhead) : 1.0;
    int used = 0;
    for (int i = 0; i < SeriesCount; ++i) {
        if (i == Downloads || i == Uploads)
            continue;
        p.v[i] = int(shape.v[i] * scale);
        used += p.v[i];
    }
    p.v[payloadSeries] = qMax(0, total - used);
    return p;
}

int niceStep(int peak)
{
    peak = qMax(peak, 1000);
    if (!fmt::exactSizes()) {
        // [ui-e] steps in KB/s / MB/s (1024-based), so the axis reads "100 200 300 KB/s"
        for (qint64 unit : {qint64(1024), qint64(1024 * 1024)})
            for (qint64 mag = 1; mag <= 100000; mag *= 10)
                for (int m : {1, 2, 5}) {
                    const qint64 step = unit * mag * m;
                    if (unit == 1024 && mag * m >= 1000)
                        continue;
                    if ((peak + step - 1) / step <= 9)
                        return int(qMin<qint64>(step, INT_MAX));
                }
    }
    for (qint64 mag = 100;; mag *= 10)
        for (int m : {1, 2, 5}) {
            const qint64 step = mag * m;
            if ((peak + step - 1) / step <= 9)
                return int(step);
        }
}

} // namespace bw

// ------------------------------------------------------------------------------- BandwidthGraph

BandwidthGraph::BandwidthGraph(bool incoming, QWidget *parent) : QFrame(parent), m_incoming(incoming)
{
    setObjectName(incoming ? QStringLiteral("bandwidthIncoming") : QStringLiteral("bandwidthOutgoing"));
    setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);
    setLineWidth(2 * uiLine());
    setMinimumHeight(S(40));
    setAttribute(Qt::WA_OpaquePaintEvent);
    connect(Colors::instance(), &Colors::changed, this, qOverload<>(&QWidget::update));
}

QRect BandwidthGraph::plotRect() const
{
    const QRect r = contentsRect();
    return QRect(QPoint(r.left() + S(kPlotLeft), r.top() + S(kTopLine)), QPoint(r.right() - S(kRightMargin), r.bottom()));
}

int BandwidthGraph::visibleCount() const
{
    // [ui-e] one sample per line width (1 px at 100 %)
    return qMax(0, plotRect().width() / uiLine());
}

int BandwidthGraph::peak() const
{
    int peak = 0;
    if (m_pts) {
        const int n = int(m_pts->size());
        const int first = qMax(0, n - visibleCount());
        for (int i = first; i < n; ++i)
            peak = qMax(peak, m_pts->at(i).total());
    }
    // [bw-1] the limit line stays in view once the traffic gets near it (a third of it)
    if (m_limit > 0 && peak * 3 >= m_limit)
        peak = qMax(peak, m_limit);
    return peak;
}

bool BandwidthGraph::limitLineVisible() const
{
    return m_limit > 0 && m_limit <= axisMax();
}

int BandwidthGraph::axisStep() const
{
    return bw::niceStep(peak());
}

int BandwidthGraph::axisMax() const
{
    const int peak = this->peak();
    const int step = bw::niceStep(peak);
    return int(qMax<qint64>(1, (qMax(peak, 1000) + step - 1) / step) * step);
}

QColor BandwidthGraph::seriesColor(int s) const
{
    switch (s) {
    case bw::InPeers: return color(keys::colors::GraphIncomingPeers);
    case bw::OutPeers: return color(keys::colors::GraphOutgoingPeers);
    case bw::Packets: return color(keys::colors::GraphPackets);
    case bw::Overlay: return color(keys::colors::GraphOverlay);
    case bw::Server: return color(m_incoming ? keys::colors::GraphServerIn : keys::colors::GraphServerOut);
    case bw::Downloads: return color(keys::colors::GraphDown);
    case bw::Uploads: return color(keys::colors::GraphUp);
    default: return color(keys::colors::GraphOther);
    }
}

void BandwidthGraph::paintEvent(QPaintEvent *e)
{
    QPainter p(this);
    const QRect r = contentsRect();
    p.fillRect(r, color(keys::colors::GraphBackground));
    const QRect plot = plotRect();
    const int maxV = axisMax();
    const int step = axisStep();
    const int bottom = r.bottom();
    const int span = qMax(1, bottom - plot.top());
    auto yOf = [&](qint64 v) { return bottom - int((v * span + maxV / 2) / maxV); };

    QColor cols[bw::SeriesCount];
    for (int s = 0; s < bw::SeriesCount; ++s)
        cols[s] = seriesColor(s);
    auto column = [&](int x, int w, const bw::Point &pt) {
        qint64 cum = 0;
        int y0 = bottom + 1;
        for (int s = 0; s < bw::SeriesCount; ++s) {
            if (pt.v[s] <= 0)
                continue;
            cum += pt.v[s];
            const int y1 = qMax(r.top(), yOf(cum));
            if (y1 < y0)
                p.fillRect(QRect(x, y1, w, y0 - y1), cols[s]);
            y0 = y1;
        }
    };
    if (m_pts && !m_pts->isEmpty()) {
        const int n = int(m_pts->size());
        // newest sample at the right end of the grid, then one pixel per second to the left
        const int t = uiLine();
        int x = plot.right() - t + 1;
        for (int i = n - 1; i >= 0 && x >= plot.left(); --i, x -= t) {
            if (x + t - 1 < e->rect().left() - 1 || x > e->rect().right() + 1)
                continue;
            column(x, t, m_pts->at(i));
        }
        // the latest value continues as a flat block in the right margin
        column(plot.right() + 1, r.right() - plot.right(), m_pts->last());
    }
    // grid lines (drawn over the areas, as the original WinMX does) and labels
    p.setFont(normalFont());
    for (int k = 1; qint64(k) * step <= maxV; ++k) {
        const int y = yOf(qint64(k) * step);
        p.fillRect(QRect(plot.left(), y, plot.width(), uiLine()), color(keys::colors::GraphGrid));
        p.setPen(color(keys::colors::GraphText));
        QString t = QString::number(qint64(k) * step);
        if (!fmt::exactSizes()) {   // [ui-e] in the unit of the axis; the unit itself at the top line
            const qint64 unit = maxV >= 1000 * 1024 ? 1024 * 1024 : 1024;
            const double v = double(qint64(k) * step) / unit;
            t = QString::number(v, 'f', v == qRound(v) ? 0 : 1).replace(QLatin1Char('.'), fmt::decimalPoint());
            if (qint64(k + 1) * step > maxV)
                t += unit == 1024 ? QStringLiteral(" KB/s") : QStringLiteral(" MB/s");
        }
        const int tw = p.fontMetrics().horizontalAdvance(t);
        // [qa-1] right-aligned at the label edge, but never cut off on the left: from 10 000 000 B/s on the
        // number is wider than the label margin and runs on into the plot instead (real-core finding)
        const int base = isScaled() ? y + p.fontMetrics().ascent() / 2 : y + 5;
        p.drawText(QPoint(qMax(r.left() + 1, r.left() + S(kLabelRight) + 1 - tw), base), t);
    }
    // [bw-1] the speed limit in force: dashed line in the text colour, "Limit 500 KB/s" above it at the
    // right (or at the top, without the line, while the traffic is far below it)
    if (m_limit > 0) {
        const QColor c = color(keys::colors::GraphText);
        const bool inView = m_limit <= maxV;
        const int y = inView ? yOf(m_limit) : plot.top();
        if (inView) {
            // text-colour dashes with background-colour gaps: visible over any series colour
            const int dash = S(4), gap = S(3);
            const QColor bg = color(keys::colors::GraphBackground);
            for (int x = plot.left(); x <= plot.right(); x += dash + gap) {
                p.fillRect(QRect(x, y, qMin(dash, plot.right() - x + 1), uiLine()), c);
                if (x + dash <= plot.right())
                    p.fillRect(QRect(x + dash, y, qMin(gap, plot.right() - x - dash + 1), uiLine()), bg);
            }
        }
        const QString amount = fmt::exactSizes() ? fmt::rateLong(m_limit) : speed::kbsText(qMax(1, qRound(m_limit / 1024.0)));
        const QString label = inView ? tr("Limit %1").arg(amount) : tr("Limit %1 (far above)").arg(amount);
        const QFontMetrics fm = p.fontMetrics();
        const int tw = fm.horizontalAdvance(label);
        const int tx = plot.right() - tw - S(4);
        const int base = inView ? qMax(r.top() + fm.ascent() + 1, y - S(3)) : r.top() + fm.ascent() + S(1);
        const QRect box(tx - S(2), base - fm.ascent(), tw + S(4), fm.height());
        p.fillRect(box, color(keys::colors::GraphBackground));
        p.setPen(c);
        p.drawText(QPoint(tx, base), label);
    }
    QFrame::paintEvent(e);   // the sunken frame
}

bool BandwidthGraph::event(QEvent *e)
{
    if (e->type() == QEvent::ToolTip) {
        auto *he = static_cast<QHelpEvent *>(e);
        const QRect plot = plotRect();
        if (m_pts && !m_pts->isEmpty() && he->pos().x() >= plot.left()) {
            const int back = qMax(0, plot.right() - he->pos().x()) / uiLine();
            const int i = int(m_pts->size()) - 1 - back;
            if (i >= 0) {
                const bw::Point &pt = m_pts->at(i);
                QStringList lines;
                lines << tr("%1: %2").arg(m_incoming ? tr("Incoming") : tr("Outgoing"), fmt::rateLong(pt.total()));
                for (int s = bw::SeriesCount - 1; s >= 0; --s)
                    if (pt.v[s] > 0)
                        lines << QStringLiteral("%1: %2").arg(bw::seriesLabel(s), fmt::exactSizes() ? fmt::number(pt.v[s]) : fmt::rate(pt.v[s]));
                QToolTip::showText(he->globalPos(), lines.join(QLatin1Char('\n')), this);
                return true;
            }
        }
        QToolTip::hideText();
        e->ignore();
        return true;
    }
    return QFrame::event(e);
}

// -------------------------------------------------------------------------------- BandwidthPage

BandwidthPage::BandwidthPage(mx::Core *core, MainWindow *mw, QWidget *parent)
    : Page(QStringLiteral("bandwidth"), tr("Bandwidth"), core, mw, parent)
{
    m_row = new SpeedLimitRow(core->settings(), mw, this);   // [bw-1]
    m_split = new FaceSplitter(Qt::Vertical, this);
    m_split->setObjectName(QStringLiteral("bandwidthSplitter"));
    m_split->setHandleWidth(S(7));
    m_split->setChildrenCollapsible(false);
    m_in = new BandwidthGraph(true, m_split);
    m_out = new BandwidthGraph(false, m_split);
    m_split->addWidget(m_in);
    m_split->addWidget(m_out);
    m_in->setPoints(&m_inPts);
    m_out->setPoints(&m_outPts);
    // stored sizes: applied in showEvent, when the page has its final size
    connect(m_split, &QSplitter::splitterMoved, this, [this] {
        this->core()->settings()->setValue(keys::ui::splitterKey(QStringLiteral("bandwidth")), m_split->saveState());
    });

    // the backlog from before the page existed: detailed samples are split like live ones; a core
    // that keeps only totals gets today's proportions
    BandwidthService *bws = core->bandwidth();
    const BandwidthSample now = bws->current();
    const bw::Point shapeIn = bw::split(core, now, true);
    const bw::Point shapeOut = bw::split(core, now, false);
    for (const BandwidthSample &s : bws->history(kMaxPoints)) {
        const bool detailed = bw::isDetailed(s);
        push(m_inPts, m_inRaw, detailed ? bw::split(core, s, true) : bw::reshape(shapeIn, s, true));
        push(m_outPts, m_outRaw, detailed ? bw::split(core, s, false) : bw::reshape(shapeOut, s, false));
    }
    if (!m_inPts.isEmpty()) {
        m_lastIn = m_inPts.last().total();
        m_lastOut = m_outPts.last().total();
    }
    connect(bws, &BandwidthService::sample, this, &BandwidthPage::onSample);
    connect(core->settings(), &Settings::changed, this, [this](const QString &key) {
        if (key.startsWith(QLatin1String("bandwidth/")))
            updateLimits();
    });
    updateLimits();
}

void BandwidthPage::updateLimits()
{
    // [bw-1] limits in force (a schedule may switch them: also re-read every sample)
    const bwlimit::InForce f = speed::inForce(core()->settings());
    m_in->setLimit(int(qMin<qint64>(qint64(f.downKBs) * 1024, INT_MAX)));
    m_out->setLimit(int(qMin<qint64>(qint64(f.upKBs) * 1024, INT_MAX)));
    emit statusChanged();
}

void BandwidthPage::push(QVector<bw::Point> &v, QVector<bw::Point> &raw, const bw::Point &p)
{
    // rolling average over "BW Graph Rolling Avg. Duration" seconds (Settings > Bandwidth Throttle)
    const int n = qBound(1, core()->settings()->i(keys::bandwidth::GraphAverageSec), 30);
    raw.append(p);
    while (raw.size() > n)
        raw.removeFirst();
    bw::Point avg;
    avg.timeMs = p.timeMs;
    for (const bw::Point &q : std::as_const(raw))
        for (int s = 0; s < bw::SeriesCount; ++s)
            avg.v[s] += q.v[s];
    for (int s = 0; s < bw::SeriesCount; ++s)
        avg.v[s] /= int(raw.size());
    v.append(avg);
    if (v.size() > kMaxPoints)
        v.remove(0, v.size() - kMaxPoints);
}

void BandwidthPage::onSample(const BandwidthSample &s)
{
    push(m_inPts, m_inRaw, bw::split(core(), s, true));
    push(m_outPts, m_outRaw, bw::split(core(), s, false));
    m_lastIn = m_inPts.last().total();
    m_lastOut = m_outPts.last().total();
    updateLimits();
    m_row->refresh();
    if (isVisible()) {
        m_in->update();
        m_out->update();
    }
    emit statusChanged();
}

QStringList BandwidthPage::statusFields() const
{
    // [bw-1] the limit in force next to each rate
    QString in = tr("Incoming (top) %1").arg(fmt::rateLong(m_lastIn));
    QString out = tr(" Outgoing (bottom) %1").arg(fmt::rateLong(m_lastOut));
    auto lim = [](int bps) { return fmt::exactSizes() ? fmt::rateLong(bps) : speed::kbsText(qMax(1, qRound(bps / 1024.0))); };
    if (m_in->limit() > 0)
        in += tr(" (limit %1)").arg(lim(m_in->limit()));
    if (m_out->limit() > 0)
        out += tr(" (limit %1)").arg(lim(m_out->limit()));
    return {in, out};
}

void BandwidthPage::saveState()
{
    if (m_splitRestored)
        core()->settings()->setValue(keys::ui::splitterKey(QStringLiteral("bandwidth")), m_split->saveState());
}

void BandwidthPage::showEvent(QShowEvent *e)
{
    Page::showEvent(e);
    if (m_splitRestored)
        return;
    m_splitRestored = true;
    const QByteArray st = core()->settings()->value(keys::ui::splitterKey(QStringLiteral("bandwidth"))).toByteArray();
    if (!st.isEmpty())
        m_split->restoreState(st);
}

void BandwidthPage::resizeEvent(QResizeEvent *)
{
    // [bw-1] the speed-limit row starts 3 px below the page buttons, the graphs follow it and leave
    // 2 px above the status bar
    const int rowH = S(SpeedLimitRow::designHeight());
    m_row->setGeometry(0, S(2), width(), rowH);
    const int top = S(2) + rowH + S(1);
    m_split->setGeometry(0, top, width(), qMax(0, height() - top - S(2)));
}

} // namespace mx::ui

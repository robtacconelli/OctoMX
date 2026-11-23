// SPDX-License-Identifier: GPL-3.0-or-later
#include "bt_pacing.h"

#include <algorithm>

#include <libtorrent/extensions.hpp>
#include <libtorrent/peer_connection.hpp>
#include <libtorrent/peer_connection_handle.hpp>
#include <libtorrent/torrent_handle.hpp>

namespace lt = libtorrent;

namespace mx {
namespace bt {

int pacedQueueBlocks(int downBps, int peers, int maxBlocks, double inflightSec)
{
    if (downBps <= 0) return -1;
    const double bytes = inflightSec * double(downBps) / double(std::max(1, peers));
    return std::clamp(int(bytes / kBlockBytes + 0.5), kMinQueueBlocks, std::max(kMinQueueBlocks, maxBlocks));
}

double adaptInflight(double inflightSec, double rateBps, double budgetBps)
{
    if (budgetBps <= 0) return kInflightSec;
    if (rateBps < 0.93 * budgetBps) inflightSec *= 1.25;         // under the budget: longer pipe
    else if (rateBps >= 0.97 * budgetBps) inflightSec *= 0.93;   // at the budget: the shortest that still fills
    return std::clamp(inflightSec, kInflightMinSec, kInflightMaxSec);
}

int pacedSendBuffer(int upBps, int peers, int maxBytes)
{
    if (upBps <= 0) return -1;
    const double bytes = kInflightSec * double(upBps) / double(std::max(1, peers));
    return std::clamp(int(bytes), kBlockBytes, std::max(kBlockBytes, maxBytes));
}

namespace {

class PacingPeer final : public lt::peer_plugin {
public:
    PacingPeer(lt::peer_connection_handle h, std::shared_ptr<PacingState> st) : m_h(std::move(h)), m_st(std::move(st)) {}
    // every received block and every second (cheap: a few integer reads and one store)
    bool on_piece(lt::peer_request const &, lt::span<char const>) override { apply(); return false; }
    void tick() override { apply(); }
    bool on_extension_handshake(lt::bdecode_node const &) override { apply(); return true; }

private:
    void apply()
    {
        const std::shared_ptr<lt::peer_connection> pc = m_h.native_handle();
        if (!pc) return;
        const int now = pc->max_out_request_queue();
        if (now != m_set) m_orig = now;   // libtorrent (or the peer's reqq) set it: that is the default
        const int blocks = pacedQueueBlocks(m_st->downBps.load(std::memory_order_relaxed),
                                            m_st->downPeers.load(std::memory_order_relaxed), m_orig,
                                            m_st->inflightMs.load(std::memory_order_relaxed) / 1000.0);
        const int want = blocks < 0 ? m_orig : blocks;   // no limit: back to what it had
        if (want != now) pc->max_out_request_queue(want);
        m_set = want;
    }

    lt::peer_connection_handle m_h;
    std::shared_ptr<PacingState> m_st;
    int m_orig = 500;
    int m_set = -1;
};

class PacingTorrent final : public lt::torrent_plugin {
public:
    explicit PacingTorrent(std::shared_ptr<PacingState> st) : m_st(std::move(st)) {}
    std::shared_ptr<lt::peer_plugin> new_connection(lt::peer_connection_handle const &h) override
    {
        if (h.type() != lt::connection_type::bittorrent) return {};   // web seeds keep their pipeline
        return std::make_shared<PacingPeer>(h, m_st);
    }

private:
    std::shared_ptr<PacingState> m_st;
};

class PacingPlugin final : public lt::plugin {
public:
    explicit PacingPlugin(std::shared_ptr<PacingState> st) : m_st(std::move(st)) {}
    std::shared_ptr<lt::torrent_plugin> new_torrent(lt::torrent_handle const &, lt::client_data_t) override
    {
        return std::make_shared<PacingTorrent>(m_st);
    }

private:
    std::shared_ptr<PacingState> m_st;
};

} // namespace

std::shared_ptr<lt::plugin> makePacingPlugin(std::shared_ptr<PacingState> state)
{
    return std::make_shared<PacingPlugin>(std::move(state));
}

} // namespace bt
} // namespace mx

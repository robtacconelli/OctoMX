// SPDX-License-Identifier: GPL-3.0-or-later
// [bw-2] How much libtorrent keeps in flight per peer under a speed limit.
//
// libtorrent hands a peer its share of the limit only once the whole amount the peer asked for is
// assigned, and a peer asks for all it has outstanding. So one peer's outstanding requests are one
// "lump" of reading, and the lumps of all peers add up. The other way round, a peer can deliver at
// most its outstanding bytes per round trip. Rule: all peers we download from together keep about
// kInflightSec of the download budget in flight (a tenth of a second and a bit: one peer then carries
// the whole limit at a 100 ms round trip, and a lump is a tenth of a second of the limit, as two
// blocks are at 300 KB/s), split among them, never less than libtorrent's minimum of 2 blocks and
// never more than libtorrent's default. The time adapts to the peers' round trip (adaptInflight).
//
// libtorrent's setting max_out_request_queue cannot do this: it is read when a connection is made,
// and the peer's extension handshake replaces it with the peer's own "reqq". A peer plugin sets each
// connection's limit instead, again with every received block and every second, so a new limit or a
// new number of peers applies at once. Without a limit the connection gets back what it had.
#pragma once

#include <atomic>
#include <memory>

namespace libtorrent { struct plugin; }

namespace mx {
namespace bt {

constexpr double kInflightSec = 0.12;
constexpr int kBlockBytes = 16 * 1024;
constexpr int kMinQueueBlocks = 2;      // libtorrent's own minimum

// blocks in flight per peer for a download budget (bytes/s, 0 = no limit: -1) and the number of
// peers we download from, for an in-flight time of inflightSec; maxBlocks = libtorrent's default
int pacedQueueBlocks(int downBps, int peers, int maxBlocks, double inflightSec = kInflightSec);
// bytes of send buffer per peer for an upload budget and the number of peers we upload to
int pacedSendBuffer(int upBps, int peers, int maxBytes);

// The in-flight time adapts to the round trip (a bandwidth-delay product, measured, not guessed):
// once a second, while libtorrent moves less than 93 % of its budget the pipe may be too short for
// the peers' round trip: +25 %; while it moves 97 % or more, the shortest pipe that still fills is
// looked for (smaller lumps): -7 %. (The bytes waiting for its quota are no signal: with quota handed
// out per 100 ms tick, even a pipe that is just long enough has a request waiting a third of the
// time.) Peers that cannot deliver more make it grow to kInflightMaxSec: harmless, a slow peer keeps
// only its own second's worth in flight. Between kInflightMinSec and kInflightMaxSec.
constexpr double kInflightMinSec = 0.10;
constexpr double kInflightMaxSec = 0.50;
double adaptInflight(double inflightSec, double rateBps, double budgetBps);

struct PacingState {
    std::atomic<int> downBps{0};     // libtorrent's download limit now (0 = none)
    std::atomic<int> downPeers{1};   // peers with requests outstanding (session counter)
    std::atomic<int> inflightMs{int(kInflightSec * 1000)};
};

std::shared_ptr<libtorrent::plugin> makePacingPlugin(std::shared_ptr<PacingState> state);

} // namespace bt
} // namespace mx

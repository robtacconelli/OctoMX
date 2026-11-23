// SPDX-License-Identifier: GPL-3.0-or-later
// research-librats PoC: see docs/research-librats.md
//
// Modes:
//   rats_poc search   [opts] <keyword>...   join the live Rats Search network
//                                           (protocol "rats-search/4") and run a
//                                           P2P keyword search the way the
//                                           rats-search 2.3.x GUI does.
//   rats_poc dhtprobe [opts] [key...]       standalone mainline-DHT client that
//                                           counts how many endpoints are announced
//                                           under SHA1(key) (population estimate).
//   rats_poc overlay  [opts] --name N       OctoMX overlay test node: GossipSub
//                                           topic + direct JSON private message.
//
// Common options:
//   --data DIR        persistent state dir (identity.key, peers.json, dht tables)
//   --port P          librats listen port (TCP+UDP), default 0 (ephemeral)
//   --dht-port P      DHT UDP port, default 0 (ephemeral)
//   --seconds S       run time, default 300
//   --verbose         librats log level INFO (default WARN); --debug for DEBUG
// overlay options:
//   --name N          node name used in messages
//   --key K           protocol / discovery key (default "octomx-poc/1")
//   --discovery M     mdns | dht | both | none   (default both)
//   --connect H:P     dial this address directly at start
//   --no-default-bootstrap   (dht) use no internet bootstrap nodes
//   --no-pex          do not attach PeerExchange (forces multi-hop gossip)
//
// All output lines start with "+<seconds since start>" so runs can be compared.

#include <librats/crypto/sha1.h>
#include <librats/dht/dht.h>
#include <librats/node/node.h>
#include <librats/peer/peer.h>
#include <librats/peer/peer_id.h>
#include <librats/subsystems/dht_discovery.h>
#include <librats/subsystems/hole_punch.h>
#include <librats/subsystems/mdns_discovery.h>
#include <librats/subsystems/message_json.h>
#include <librats/subsystems/peer_exchange.h>
#include <librats/subsystems/pubsub.h>
#include <librats/subsystems/reconnection.h>
#include <librats/subsystems/relay.h>
#include <librats/util/json.h>
#include <librats/util/logger.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace librats;
using Clock = std::chrono::steady_clock;

namespace {

const Clock::time_point g_t0 = Clock::now();
std::mutex g_out;
std::atomic<bool> g_stop{false};

double since_start() { return std::chrono::duration<double>(Clock::now() - g_t0).count(); }

template <typename... A>
void out(A&&... a) {
    std::ostringstream s;
    char ts[32];
    std::snprintf(ts, sizeof ts, "+%7.2fs ", since_start());
    s << ts;
    (s << ... << a);
    std::lock_guard<std::mutex> l(g_out);
    std::cout << s.str() << std::endl;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string human(int64_t b) {
    const char* u[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(b);
    int i = 0;
    while (v >= 1024 && i < 4) { v /= 1024; ++i; }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f%s", v, u[i]);
    return buf;
}

int64_t jint(const Json& j, const char* k) {
    if (!j.is_object() || !j.contains(k)) return 0;
    const Json& v = j[k];
    if (v.is_number_float()) return static_cast<int64_t>(v.get<double>());
    if (v.is_number()) return v.get<int64_t>();
    return 0;
}
std::string jstr(const Json& j, const char* k) {
    if (!j.is_object() || !j.contains(k) || !j[k].is_string()) return {};
    return j[k].get<std::string>();
}

struct Opts {
    std::string mode;
    std::string data;
    uint16_t port = 0;
    uint16_t dht_port = 0;
    int seconds = 300;
    LogLevel log = LogLevel::WARN;
    std::vector<std::string> args;
    // overlay
    std::string name = "node";
    std::string key = "octomx-poc/1";
    std::string discovery = "both";
    std::string connect;
    bool no_default_bootstrap = false;
    bool no_pex = false;
};

Opts parse(int argc, char** argv) {
    Opts o;
    if (argc < 2) return o;
    o.mode = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--data") o.data = next();
        else if (a == "--port") o.port = static_cast<uint16_t>(std::stoi(next()));
        else if (a == "--dht-port") o.dht_port = static_cast<uint16_t>(std::stoi(next()));
        else if (a == "--seconds") o.seconds = std::stoi(next());
        else if (a == "--verbose") o.log = LogLevel::INFO;
        else if (a == "--debug") o.log = LogLevel::DEBUG;
        else if (a == "--name") o.name = next();
        else if (a == "--key") o.key = next();
        else if (a == "--discovery") o.discovery = next();
        else if (a == "--connect") o.connect = next();
        else if (a == "--no-default-bootstrap") o.no_default_bootstrap = true;
        else if (a == "--no-pex") o.no_pex = true;
        else o.args.push_back(a);
    }
    return o;
}

void sleep_until_done(int seconds, const std::function<void(int)>& tick) {
    for (int s = 1; s <= seconds && !g_stop; ++s) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (tick) tick(s);
    }
}

// ───────────────────────────── search mode ─────────────────────────────────

struct PeerState {
    Clock::time_point connected;
    Clock::time_point query_sent;
    std::string version;
    int64_t torrents = -1;
    int results = 0;
    double first_result_ms = -1;
    std::string addr;
};

int run_search(const Opts& o) {
    std::vector<std::string> keywords = o.args;
    if (keywords.empty()) keywords = {"ubuntu", "big buck bunny"};

    NodeConfig cfg;
    cfg.listen_port = o.port;
    cfg.max_peers = 30;
    cfg.protocol = "rats-search/4";  // exactly as rats-search 2.3.6 src/net/p2p_transport.cpp
    cfg.security = NodeConfig::Security::Noise;
    cfg.data_dir = o.data;
    Node node(cfg);

    std::mutex mu;
    std::unordered_map<std::string, PeerState> peers;      // by peer hex id
    std::map<std::string, std::pair<std::string, Json>> results;  // hash -> (peer, json)
    std::map<std::string, int> per_keyword;
    std::atomic<int> total_peers_seen{0};
    std::atomic<int> dial_failures{0};
    std::set<std::string> details_asked;                 // keywords we fetched details for
    std::map<std::string, Clock::time_point> details_sent;  // hash -> request time

    DhtDiscovery::Config dc;
    dc.dht_port = o.dht_port;
    dc.data_dir = o.data;
    auto* dht = node.add_subsystem(std::make_unique<DhtDiscovery>(dc));
    node.add_subsystem(std::make_unique<MdnsDiscovery>());
    auto* json = node.add_subsystem(std::make_unique<MessageJson>());
    {
        ReconnectionService::Config rc;
        if (!o.data.empty()) rc.store_path = o.data + "/peers.json";
        node.add_subsystem(std::make_unique<ReconnectionService>(rc));
    }
    {
        HolePunch::Config hp;
        hp.enable_relay = true;
        node.add_subsystem(std::make_unique<HolePunch>(hp));
    }
    {
        Relay::Config rl;
        rl.serve = false;
        node.add_subsystem(std::make_unique<Relay>(rl));
    }
    {
        PeerExchange::Config px;
        px.peer_target = 30;
        node.add_subsystem(std::make_unique<PeerExchange>(px));
    }

    auto send_queries = [&](const PeerId& id) {
        // Our handshake: the same client_info record PeerRegistry sends.
        Json info = Json::object();
        info["clientVersion"] = "octomx-research/0.1";
        info["torrents"] = 0;
        info["files"] = 0;
        info["totalSize"] = 0;
        info["peersConnected"] = static_cast<int64_t>(node.peer_count());
        info["connectedAt"] = 0;
        info["databaseSharing"] = false;
        json->send(id, "client_info", info);
        for (const auto& kw : keywords) {
            // Same fields mainwindow.cpp puts in its broadcast.
            Json q = Json::object();
            q["query"] = kw;
            q["text"] = kw;
            q["limit"] = 50;
            q["orderBy"] = "";
            q["orderDesc"] = true;
            q["safeSearch"] = false;
            json->send(id, "searchTorrent", q);
            json->send(id, "searchFiles", q);
        }
        std::lock_guard<std::mutex> l(mu);
        peers[id.to_hex()].query_sent = Clock::now();
    };

    node.on_peer_connected([&](const Peer& p) {
        auto info = p.info();
        std::string addr = info && info->endpoint ? info->endpoint->to_string()
                           : (info && !info->addresses.empty() ? info->addresses.front().to_string() : "?");
        std::string tr = info ? to_string(info->transport) : "?";
        {
            std::lock_guard<std::mutex> l(mu);
            auto& st = peers[p.id().to_hex()];
            st.connected = Clock::now();
            st.addr = addr;
        }
        ++total_peers_seen;
        out("PEER+ ", p.id().short_hex(), " ", addr, " via ", tr, " (dir ",
            info && info->direction == ConnRole::Inbound ? "in" : "out", ") peers=", node.peer_count());
        send_queries(p.id());
    });
    node.on_peer_rerouted([&](const Peer& p) {
        out("PEER~ ", p.id().short_hex(), " rerouted, re-sending queries");
        send_queries(p.id());
    });
    node.on_peer_disconnected([&](const PeerId& id, CloseReason r) {
        out("PEER- ", id.short_hex(), " reason=", to_string(r), " peers=", node.peer_count());
    });
    node.on_dial_failed([&](const auto&...) { ++dial_failures; });

    json->on("client_info", [&](const PeerId& from, const Json& d) {
        std::lock_guard<std::mutex> l(mu);
        auto& st = peers[from.to_hex()];
        st.version = jstr(d, "clientVersion");
        st.torrents = d.contains("torrents") ? jint(d, "torrents") : jint(d, "torrentsCount");
        out("INFO  ", from.short_hex(), " clientVersion=", st.version, " torrents=", st.torrents,
            " files=", jint(d, "files"), " totalSize=", human(jint(d, "totalSize")),
            " peersConnected=", jint(d, "peersConnected"),
            " databaseSharing=", d.contains("databaseSharing") && d["databaseSharing"].is_boolean() &&
                                         d["databaseSharing"].get<bool>() ? "yes" : "no");
    });

    auto on_hit = [&](const char* kind, const PeerId& from, const Json& d) {
        std::string hash = jstr(d, "hash");
        if (hash.empty()) hash = jstr(d, "info_hash");
        std::string name = jstr(d, "name");
        std::string lname = lower(name);
        std::string kw = "?";
        for (const auto& k : keywords) {
            bool all = true;
            std::istringstream ws(lower(k));
            std::string tok;
            while (ws >> tok) if (lname.find(tok) == std::string::npos) all = false;
            if (all) { kw = k; break; }
        }
        double lat = -1;
        bool fresh = false;
        {
            std::lock_guard<std::mutex> l(mu);
            auto& st = peers[from.to_hex()];
            lat = std::chrono::duration<double, std::milli>(Clock::now() - st.query_sent).count();
            st.results++;
            if (st.first_result_ms < 0) st.first_result_ms = lat;
            fresh = results.emplace(hash, std::make_pair(from.short_hex(), d)).second;
            if (fresh) per_keyword[kw]++;
        }
        static std::atomic<int> raw_dumped{0};
        if (raw_dumped.fetch_add(1) < 2) out("RAW   [", kind, "] ", d.dump());
        bool want_details = false;
        {
            std::lock_guard<std::mutex> l(mu);
            if (fresh && kw != "?" && details_asked.insert(kw).second) want_details = true;
        }
        if (want_details) {
            // Same request PeerApi::requestTorrent() sends when the user opens a remote hit.
            Json req = Json::object();
            req["hash"] = hash;
            Json opt = Json::object();
            opt["files"] = true;
            req["options"] = opt;
            json->send(from, "torrent", req);
            std::lock_guard<std::mutex> l(mu);
            details_sent[hash] = Clock::now();
        }
        if (fresh)
            out("HIT   [", kind, "] kw='", kw, "' ", hash, " ", human(jint(d, "size")), " files=", jint(d, "files"),
                " S/L=", jint(d, "seeders"), "/", jint(d, "leechers"), " ", name, "  <- ", from.short_hex(), " ",
                static_cast<int>(lat), "ms");
    };
    json->on("searchTorrent_response", [&](const PeerId& f, const Json& d) { on_hit("torrent", f, d); });
    json->on("searchFiles_response", [&](const PeerId& f, const Json& d) { on_hit("files", f, d); });
    json->on("torrent_response", [&](const PeerId& f, const Json& d) {
        std::string hash = jstr(d, "hash");
        double ms = -1;
        {
            std::lock_guard<std::mutex> l(mu);
            auto it = details_sent.find(hash);
            if (it != details_sent.end())
                ms = std::chrono::duration<double, std::milli>(Clock::now() - it->second).count();
        }
        size_t nfiles = d.contains("files_list") && d["files_list"].is_array() ? d["files_list"].size() : 0;
        std::string keys;
        for (auto it = d.begin(); it != d.end(); ++it) keys += it.key() + ",";
        out("DETAIL ", f.short_hex(), " ", hash, " '", jstr(d, "name"), "' files_list=", nfiles, " keys=[", keys, "] ",
            static_cast<int>(ms), "ms");
        for (size_t i = 0; i < nfiles && i < 4; ++i)
            out("        file: ", jstr(d["files_list"][i], "path"), " ", human(jint(d["files_list"][i], "size")));
        if (d.contains("info")) {
            std::string inf = d["info"].dump();
            if (inf.size() > 300) inf = inf.substr(0, 300) + "...";
            out("        info: ", inf);
        }
    });
    // Requests other peers send us on connect; log them so the evidence shows
    // the full conversation (we answer nothing: our index is empty).
    for (const char* t : {"feed", "randomTorrents", "searchTorrent", "searchFiles", "topTorrents", "torrent"}) {
        std::string type = t;
        json->on(type, [type](const PeerId& f, const Json& d) {
            std::string s = d.dump();
            if (s.size() > 160) s = s.substr(0, 160) + "...";
            out("REQ   ", f.short_hex(), " ", type, " ", s);
        });
    }
    // Not requested, but log if it comes (torrentAnnounce is unsolicited).
    json->on("torrentAnnounce", [](const PeerId& f, const Json& d) {
        out("ANN   ", f.short_hex(), " ", jstr(d, "info_hash"), " ", jstr(d, "name"));
    });

    if (!node.start()) {
        std::cerr << "node start failed\n";
        return 1;
    }
    out("node ", node.local_id().short_hex(), " protocol=rats-search/4 listen=", node.listen_port(),
        " dht=", dht->dht_port(), "/", dht->dht_port_v6(), " discovery_hash=",
        [&] { std::string h; char b[3]; for (auto c : dht->discovery_hash()) { std::snprintf(b, 3, "%02x", c); h += b; } return h; }());
    out("keywords: ", [&] { std::string s; for (auto& k : keywords) s += "'" + k + "' "; return s; }());

    sleep_until_done(o.seconds, [&](int s) {
        if (s % 15 == 0) {
            size_t rt = dht->dht_client() ? dht->dht_client()->get_routing_table_size() : 0;
            std::lock_guard<std::mutex> l(mu);
            out("STAT  peers=", node.peer_count(), " seen=", total_peers_seen.load(), " dial_fail=", dial_failures.load(),
                " dht_rt=", rt, " ext_ip=", dht->external_address(), " nat=", to_string(node.nat_status().udp_mapping()),
                " unique_results=", results.size());
        }
    });

    out("==== SUMMARY ====");
    {
        std::lock_guard<std::mutex> l(mu);
        out("peers ever connected: ", total_peers_seen.load(), "  dial failures: ", dial_failures.load());
        for (auto& [id, st] : peers)
            out("  peer ", id.substr(0, 8), " ", st.addr, " v=", st.version.empty() ? "?" : st.version,
                " torrents=", st.torrents, " results=", st.results, " first_result_ms=",
                static_cast<int>(st.first_result_ms));
        out("unique results: ", results.size());
        for (auto& [k, n] : per_keyword) out("  keyword '", k, "': ", n);
    }
    node.stop();
    return 0;
}

// ───────────────────────────── dhtprobe mode ───────────────────────────────

int run_dhtprobe(const Opts& o) {
    std::vector<std::string> keys = o.args;
    if (keys.empty()) keys = {"rats-search/4", "rats-search/3", "rats-search/2", "librats/1.0"};
    DhtClient dht(o.dht_port, "", o.data, AddressFamily::IPv4);
    if (!dht.start()) { std::cerr << "dht start failed\n"; return 1; }
    dht.bootstrap(DhtClient::get_default_bootstrap_nodes());
    out("dht up on ", dht.get_port());
    std::mutex mu;
    std::map<std::string, std::set<std::string>> found;
    int round = 0;
    sleep_until_done(o.seconds, [&](int s) {
        if (s % 10 == 0) out("STAT  dht_rt=", dht.get_routing_table_size());
        if (s >= 15 && s % 45 == 15) {
            ++round;
            for (const auto& k : keys) {
                InfoHash h = DhtDiscovery::hash_for_key(k);
                dht.find_peers(h, [&, k](const std::vector<Address>& ps, const InfoHash&) {
                    std::lock_guard<std::mutex> l(mu);
                    for (auto& a : ps) found[k].insert(a.to_string());
                });
            }
        }
        if (s % 45 == 0 && round > 0) {
            std::lock_guard<std::mutex> l(mu);
            for (const auto& k : keys)
                out("PROBE round ", round, " key='", k, "' sha1=", SHA1::hash(k), " unique endpoints so far=",
                    found[k].size());
        }
    });
    std::lock_guard<std::mutex> l(mu);
    for (const auto& k : keys) {
        out("FINAL key='", k, "' endpoints=", found[k].size());
        for (auto& a : found[k]) out("   ", a);
    }
    dht.stop();
    return 0;
}

// ───────────────────────────── overlay mode ────────────────────────────────

int run_overlay(const Opts& o) {
    NodeConfig cfg;
    cfg.listen_port = o.port;
    cfg.protocol = o.key;
    cfg.security = NodeConfig::Security::Noise;
    cfg.data_dir = o.data;
    Node node(cfg);

    DhtDiscovery* dht = nullptr;
    if (o.discovery == "dht" || o.discovery == "both") {
        DhtDiscovery::Config dc;
        dc.dht_port = o.dht_port;
        dc.data_dir = o.data;
        dc.search_interval = std::chrono::milliseconds(10000);  // faster than default 30 s for the test
        if (o.no_default_bootstrap) dc.bootstrap_nodes = {{"127.0.0.1", 1}};
        dht = node.add_subsystem(std::make_unique<DhtDiscovery>(dc));
    }
    if (o.discovery == "mdns" || o.discovery == "both")
        node.add_subsystem(std::make_unique<MdnsDiscovery>());
    auto* json = node.add_subsystem(std::make_unique<MessageJson>());
    auto* ps = node.add_subsystem(std::make_unique<PubSub>());
    if (!o.no_pex) node.add_subsystem(std::make_unique<PeerExchange>(PeerExchange::Config{}));

    const std::string topic = "octomx/room/lobby";
    std::atomic<int> got_pub{0}, got_pm{0};
    std::atomic<double> first_peer_at{-1};

    node.on_peer_connected([&](const Peer& p) {
        auto info = p.info();
        std::string addr = info && info->endpoint ? info->endpoint->to_string() : "?";
        if (first_peer_at.load() < 0) first_peer_at = since_start();
        out("[", o.name, "] PEER+ ", p.id().short_hex(), " ", addr, " via ", info ? to_string(info->transport) : "?");
        Json pm = Json::object();
        pm["from"] = o.name;
        pm["text"] = "private hello from " + o.name;
        pm["ts"] = static_cast<int64_t>(since_start() * 1000);
        json->send(p.id(), "pm", pm);
    });
    node.on_peer_disconnected([&](const PeerId& id, CloseReason r) {
        out("[", o.name, "] PEER- ", id.short_hex(), " ", to_string(r));
    });
    json->on("pm", [&](const PeerId& from, const Json& d) {
        ++got_pm;
        out("[", o.name, "] PM from ", from.short_hex(), " (", jstr(d, "from"), "): ", jstr(d, "text"));
    });
    ps->subscribe(topic, [&](const PeerId& from, const std::string& t, ByteView data) {
        ++got_pub;
        out("[", o.name, "] GOSSIP ", t, " from ", from.short_hex(), ": ",
            std::string(reinterpret_cast<const char*>(data.data()), data.size()));
    });

    if (!node.start()) { std::cerr << "node start failed\n"; return 1; }
    out("[", o.name, "] node ", node.local_id().short_hex(), " protocol=", o.key, " listen=", node.listen_port(),
        " discovery=", o.discovery, dht ? " dht_port=" + std::to_string(dht->dht_port()) : std::string());
    if (!o.connect.empty()) {
        auto c = o.connect.rfind(':');
        node.connect(o.connect.substr(0, c), static_cast<uint16_t>(std::stoi(o.connect.substr(c + 1))));
    }
    int n = 0;
    sleep_until_done(o.seconds, [&](int s) {
        if (s % 5 == 0 && node.peer_count() > 0) {
            std::string msg = o.name + " says hi #" + std::to_string(++n);
            ps->publish(topic, ByteView(msg));
        }
        if (s % 15 == 0)
            out("[", o.name, "] STAT peers=", node.peer_count(), " gossip_rx=", got_pub.load(), " pm_rx=", got_pm.load(),
                dht && dht->dht_client() ? " dht_rt=" + std::to_string(dht->dht_client()->get_routing_table_size())
                                         : std::string());
    });
    out("[", o.name, "] SUMMARY first_peer_at=", first_peer_at.load(), "s gossip_rx=", got_pub.load(),
        " pm_rx=", got_pm.load());
    node.stop();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, [](int) { g_stop = true; });
    Opts o = parse(argc, argv);
    Logger::getInstance().set_log_level(o.log);
    if (o.mode == "search") return run_search(o);
    if (o.mode == "dhtprobe") return run_dhtprobe(o);
    if (o.mode == "overlay") return run_overlay(o);
    std::cerr << "usage: rats_poc search|dhtprobe|overlay [options] (see main.cpp header)\n";
    return 2;
}

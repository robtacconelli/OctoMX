// SPDX-License-Identifier: GPL-3.0-or-later
// [diag-1] Connection Test rules (see diag_rules.h). Every sentence is plain English with commas,
// no em dashes, and names one cause plus what to do.
#include "core/diag/diag_rules.h"

#include <QtCore/QRegularExpression>
#include <QtCore/QSet>

namespace mx::diag {

namespace {
QString kbs(int bps) { return QString::number(qMax(0, bps) / 1024.0, 'f', bps < 10240 ? 1 : 0); }
}

QString failureBucket(const QString &errorText)
{
    const QString e = errorText.toLower();
    if (e.isEmpty()) return QStringLiteral("other");
    if (e.contains(QLatin1String("timed out")) || e.contains(QLatin1String("timeout")) || e.contains(QLatin1String("no answer"))
        || e.contains(QLatin1String("no route")) || e.contains(QLatin1String("unreachable")))
        return QStringLiteral("timed out");
    if (e.contains(QLatin1String("refused")) || e.contains(QLatin1String("reset by peer")))
        return QStringLiteral("refused");
    if (e.contains(QLatin1String("encrypt")) || e.contains(QLatin1String("handshake")) || e.contains(QLatin1String("info-hash"))
        || e.contains(QLatin1String("info hash")) || e.contains(QLatin1String("end of file")) || e.contains(QLatin1String("obfusc")))
        return QStringLiteral("handshake");
    if (e.contains(QLatin1String("banned")) || e.contains(QLatin1String("blocked")) || e.contains(QLatin1String("self connection"))
        || e.contains(QLatin1String("filtered")))
        return QStringLiteral("banned");
    return QStringLiteral("other");
}

static bool slotLimited(const DownloadFacts &f, const DownloadContext &c, Verdict *v)
{
    if (f.active || f.finished || f.paused || f.error) return false;
    v->level = DiagLevel::Limited;
    v->sentence = QStringLiteral("Waiting for a free download slot: %1 downloads are active and the limit is %2 "
                                 "(Settings, File Transfers, Max total downloads).")
                      .arg(c.activeDownloads).arg(c.maxDownloads);
    return true;
}

Verdict torrentVerdict(const DownloadFacts &f, const TorrentDiag &t, const DownloadContext &c)
{
    Verdict v;
    if (f.finished || f.paused || t.paused) return v;
    if (slotLimited(f, c, &v)) return v;
    if (t.queued) {
        v.level = DiagLevel::Limited;
        v.sentence = QStringLiteral("Queued by BitTorrent: %1 torrent downloads may run at once "
                                    "(Settings, BitTorrent, Active torrent downloads). It starts when another one finishes or is paused.")
                         .arg(c.btActiveLimit > 0 ? c.btActiveLimit : 5);
        return v;
    }
    const int trackers = int(t.trackers.size());
    int dnsFailed = 0, trackersWorking = 0, trackerPeers = 0, trackersContacted = 0;
    for (const TrackerDiag &tr : t.trackers) {
        if (tr.dnsFailed) ++dnsFailed;
        if (tr.working) ++trackersWorking;
        if (tr.contacted || tr.fails > 0) ++trackersContacted;
        trackerPeers += qMax(0, tr.peersReturned);
    }
    const int failed = t.failedTimeout + t.failedRefused + t.failedHandshake + t.failedBanned + t.failedOther;
    const bool young = t.activeSec < 45;
    const bool dhtDead = t.dhtEnabled && t.dhtNodes == 0 && t.activeSec >= 30;
    const bool rateOk = t.downBps >= 50 * 1024;

    // healthy enough: nothing to say unless a throttle is visible
    if (rateOk) {
        if (c.downLimitKBs > 0 && t.downBps >= c.downLimitKBs * 1024 * 85 / 100) {
            v.level = DiagLevel::Limited;
            v.sentence = QStringLiteral("Your download limit of %1 KB/s is in force (Settings, Bandwidth); without it this download could be faster.")
                             .arg(c.downLimitKBs);
            return v;
        }
        v.level = DiagLevel::Ok;
        return v;
    }
    if (t.connectedNow == 0) {
        if (dhtDead && trackersWorking == 0) {
            v.level = DiagLevel::Broken;
            v.sentence = QStringLiteral("DHT is not answering (0 nodes) and no tracker works: UDP seems blocked on this connection, so no peers can be found.");
            return v;
        }
        if (trackers > 0 && dnsFailed == trackers && t.dhtPeers == 0 && !young) {
            v.level = DiagLevel::Broken;
            v.sentence = QStringLiteral("None of the %1 tracker host names resolve and %2: your provider's DNS may block trackers, try another DNS server (Settings, Internet Connection).")
                             .arg(trackers)
                             .arg(t.dhtEnabled ? QStringLiteral("the DHT found no peers") : QStringLiteral("the DHT is switched off"));
            return v;
        }
        if (t.connectAttempts >= 10 && failed > 0) {
            const int to = t.failedTimeout, rf = t.failedRefused, hs = t.failedHandshake;
            if (to * 10 >= failed * 8) {
                v.level = DiagLevel::Broken;
                v.sentence = QStringLiteral("No peer answered: %1 addresses tried, %2. Outgoing connections may be blocked (firewall, VPN or security program).")
                                 .arg(t.connectAttempts)
                                 .arg(to == failed ? QStringLiteral("all timed out") : QStringLiteral("%1 timed out").arg(to));
                return v;
            }
            if (hs * 2 >= failed) {
                v.level = DiagLevel::Broken;
                v.sentence = QStringLiteral("Peers close the connection during the handshake (%1 of %2 tried): check Settings, BitTorrent, Encryption (enabled works with everybody, forced does not).")
                                 .arg(hs).arg(t.connectAttempts);
                return v;
            }
            if (rf * 2 >= failed) {
                v.level = DiagLevel::Limited;
                v.sentence = QStringLiteral("%1 addresses tried, %2 refused the connection: most peers in the lists are gone, still looking for live ones.")
                                 .arg(t.connectAttempts).arg(rf);
                return v;
            }
        }
        const int known = qMax(0, t.knownPeers);
        if (known == 0 && !young) {
            const bool dhtAsked = !t.dhtEnabled || t.dhtLookups > 0;
            const bool trackersAsked = trackers == 0 || trackersContacted > 0;
            if (f.indexSeeds > 0 && dhtAsked && trackersAsked && trackerPeers == 0 && t.dhtPeers == 0) {
                v.level = DiagLevel::Limited;
                v.sentence = QStringLiteral("%1 reports %2 but neither the trackers nor the DHT know this torrent: the index numbers are probably out of date.")
                                 .arg(f.indexName.isEmpty() ? QStringLiteral("The index") : QStringLiteral("The index (%1)").arg(f.indexName),
                                      countNoun(f.indexSeeds, QStringLiteral("seed")));
                return v;
            }
            if (dhtAsked && trackersAsked && trackersWorking + (t.dhtLookups > 0 ? 1 : 0) > 0) {
                v.level = DiagLevel::Limited;
                v.sentence = QStringLiteral("The trackers and the DHT answer, but nobody shares this torrent right now: still looking.");
                return v;
            }
            if (dhtDead) {
                v.level = DiagLevel::Broken;
                v.sentence = QStringLiteral("DHT is not answering (0 nodes): UDP seems blocked on this connection, and no tracker gave peers yet.");
                return v;
            }
            v.level = DiagLevel::Unknown;
            v.sentence = t.hasMetadata ? QStringLiteral("Looking for peers: no tracker or DHT answer yet.")
                                       : QStringLiteral("Looking for peers to get the torrent details from: no tracker or DHT answer yet.");
            return v;
        }
        if (known > 0 && t.connectAttempts == 0 && !young) {
            v.level = DiagLevel::Limited;
            v.sentence = QStringLiteral("%1 known but no connection attempt yet: the connection limits may be too low (Settings, BitTorrent, Max connections).")
                             .arg(countNoun(known, QStringLiteral("peer")));
            return v;
        }
        if (young) return v;
        v.level = DiagLevel::Unknown;
        v.sentence = QStringLiteral("%1 known, %2 tried, none connected yet.").arg(countNoun(known, QStringLiteral("peer"))).arg(t.connectAttempts);
        return v;
    }
    // connected, but slow
    if (t.interesting > 0 && t.chokingUs >= t.interesting && t.downBps < 10 * 1024) {
        v.level = DiagLevel::Limited;
        v.sentence = QStringLiteral("Connected to %1, but all of them keep us waiting (choked): they are busy uploading to others, it usually starts within a few minutes.")
                         .arg(countNoun(t.connectedNow, QStringLiteral("peer")));
        return v;
    }
    if (!t.hasMetadata && t.activeSec >= 60) {
        v.level = DiagLevel::Limited;
        v.sentence = QStringLiteral("Connected to %1, still waiting for one of them to send the torrent details (metadata).")
                         .arg(countNoun(t.connectedNow, QStringLiteral("peer")));
        return v;
    }
    if (c.downLimitKBs > 0 && t.downBps >= c.downLimitKBs * 1024 * 85 / 100) {
        v.level = DiagLevel::Limited;
        v.sentence = QStringLiteral("Your download limit of %1 KB/s is in force (Settings, Bandwidth).").arg(c.downLimitKBs);
        return v;
    }
    const int known = qMax(t.knownPeers, t.connectedNow);
    if (t.connectedNow <= 3 && known >= 20 && t.connectAttempts >= 20 && !young) {
        v.level = DiagLevel::Limited;
        QString tail;
        if (t.slowPeers >= t.connectedNow) tail = t.connectedNow == 1 ? QStringLiteral(", and it is slow") : t.connectedNow == 2 ? QStringLiteral(", both slow") : QStringLiteral(", all slow");
        v.sentence = QStringLiteral("Only %1 of %2 peers accept connections%3. Being reachable from the internet (port forwarding) helps.")
                         .arg(t.connectedNow).arg(known).arg(tail);
        return v;
    }
    if (t.seedsConnected == 0 && t.connectedNow > 0 && t.downBps < 10 * 1024 && !young && t.interesting == 0) {
        v.level = DiagLevel::Limited;
        v.sentence = QStringLiteral("Connected to %1, but none of them has the pieces we still need: waiting for a seed.")
                         .arg(countNoun(t.connectedNow, QStringLiteral("peer")));
        return v;
    }
    if (t.downBps > 0) {
        v.level = DiagLevel::Ok;
        if (t.downBps < 20 * 1024 && !young)
            v.sentence = QStringLiteral("Downloading slowly (%1 KB/s) from %2: the peers have little upload bandwidth.")
                             .arg(kbs(t.downBps), countNoun(t.connectedNow, QStringLiteral("peer")));
    }
    return v;
}

Verdict rangeVerdict(const DownloadFacts &f, const DownloadContext &c)
{
    Verdict v;
    if (f.finished || f.paused) return v;
    if (slotLimited(f, c, &v)) return v;
    if (f.rateBps >= 20 * 1024) {
        v.level = DiagLevel::Ok;
        return v;
    }
    if (f.sourcesTransferring > 0) {
        v.level = DiagLevel::Ok;
        return v;
    }
    if (f.sourcesKnown == 0) {
        if (f.activeSec < 60) return v;
        v.level = DiagLevel::Unknown;
        v.sentence = QStringLiteral("No sources found yet, the networks are asked again every few minutes.");
        return v;
    }
    if (f.sourcesQueued > 0 && f.sourcesQueued * 2 >= f.sourcesKnown - f.sourcesBanned) {
        v.level = DiagLevel::Limited;
        v.sentence = QStringLiteral("%1 of %2 have us in their upload queue%3: normal for eD2k, the transfer starts when a slot frees.")
                         .arg(f.sourcesQueued).arg(countNoun(f.sourcesKnown, QStringLiteral("source")))
                         .arg(f.bestQueueRank > 0 ? QStringLiteral(" (best place %1)").arg(f.bestQueueRank) : QString());
        return v;
    }
    int timedOut = 0, refused = 0;
    for (auto it = f.failReasons.constBegin(); it != f.failReasons.constEnd(); ++it) {
        const QString b = failureBucket(it.key());
        if (b == QLatin1String("timed out")) timedOut += it.value();
        else if (b == QLatin1String("refused")) refused += it.value();
    }
    const int failed = qMax(f.sourcesFailed, timedOut + refused);
    if (failed > 0 && failed * 2 >= f.sourcesKnown) {
        v.level = DiagLevel::Limited;
        QString why = timedOut >= refused ? QStringLiteral("they did not answer") : QStringLiteral("they refused the connection");
        if (c.weFirewalledEd2k)
            why += QStringLiteral(", and many may be firewalled like you (two LowID users cannot connect to each other)");
        v.sentence = QStringLiteral("%1 of %2 could not be reached: %3.")
                         .arg(failed).arg(countNoun(f.sourcesKnown, QStringLiteral("source")), why);
        return v;
    }
    if (f.activeSec >= 60) {
        v.level = DiagLevel::Unknown;
        v.sentence = QStringLiteral("%1 known, connecting to them.").arg(countNoun(f.sourcesKnown, QStringLiteral("source")));
    }
    return v;
}

Verdict downloadVerdict(const DownloadFacts &f, const TorrentDiag &t, const DownloadContext &c)
{
    if (t.valid && (f.isTorrent || !f.torrentId.isEmpty())) {
        TorrentDiag td = t;
        td.activeSec = qMax(t.activeSec, f.activeSec);   // libtorrent updates active_duration only now and then
        Verdict v = torrentVerdict(f, td, c);
        // other networks still deliver: a torrent without peers is no problem then
        if (v.level == DiagLevel::Broken && f.sourcesTransferring > 0) v.level = DiagLevel::Limited;
        return v;
    }
    if (f.fetchingTorrentFile && !f.finished && !f.paused) {
        Verdict v;
        if (f.activeSec >= 60) {
            v.level = DiagLevel::Limited;
            v.sentence = QStringLiteral("Still fetching the torrent file from %1: the site may be slow or blocked.")
                             .arg(f.indexName.isEmpty() ? QStringLiteral("the index") : f.indexName);
        }
        return v;
    }
    return rangeVerdict(f, c);
}

Verdict reportSummary(const QList<DiagGroup> &groups)
{
    Verdict v;
    const DiagGroup *general = nullptr;
    QStringList broken, limitedReach, limitedOther;
    QStringList netsOk;
    bool anyNet = false, anyReachKnown = false, cgnat = false, dhtDead = false, dnsBad = false;
    for (const DiagGroup &g : groups) {
        if (g.id == QLatin1String("general")) general = &g;
        if (!g.downloadId.isEmpty()) continue;
        for (const DiagItem &it : g.items) {
            if (it.id == QLatin1String("general.nat") && it.result.contains(QLatin1String("carrier-grade"), Qt::CaseInsensitive)) cgnat = true;
            if ((it.id.endsWith(QLatin1String(".dht")) || it.id.endsWith(QLatin1String(".udp"))) && it.level == DiagLevel::Broken && g.net == Net::BitTorrent) dhtDead = true;
            if (it.id.endsWith(QLatin1String(".dns")) && it.level == DiagLevel::Broken) dnsBad = true;
        }
        if (g.net == Net::Unknown) continue;
        if (g.level == DiagLevel::Off) continue;
        anyNet = true;
        if (g.level == DiagLevel::Broken) broken << g.title;
        if (g.level == DiagLevel::Ok) netsOk << g.title;
        bool notReachable = false;
        for (const DiagItem &it : g.items) {
            if (it.id.endsWith(QLatin1String(".reach.tcp")) && it.level != DiagLevel::Unknown) anyReachKnown = true;
            if ((it.id.endsWith(QLatin1String(".reach.tcp")) || it.id.endsWith(QLatin1String(".reach.udp"))) && it.result == QLatin1String("No"))
                notReachable = true;
        }
        if (notReachable) limitedReach << g.title;
        else if (g.level == DiagLevel::Limited) limitedOther << g.title;
    }
    if (!anyNet) {
        v.level = DiagLevel::Off;
        v.sentence = QStringLiteral("No network is enabled: press Connect on the Networks page.");
        return v;
    }
    if (dhtDead) {
        v.level = DiagLevel::Broken;
        v.sentence = QStringLiteral("BitTorrent cannot reach the DHT: UDP seems blocked, torrents from magnet links will not find peers.");
        return v;
    }
    if (dnsBad) {
        v.level = DiagLevel::Broken;
        v.sentence = QStringLiteral("Some server and tracker names do not resolve: your DNS may be blocking them.");
        return v;
    }
    if (!broken.isEmpty()) {
        v.level = DiagLevel::Broken;
        v.sentence = QStringLiteral("Problem with %1: see the red rows below.").arg(broken.join(QStringLiteral(", ")));
        return v;
    }
    if (cgnat) {
        v.level = DiagLevel::Limited;
        v.sentence = QStringLiteral("Works, but your provider uses carrier-grade NAT: you cannot be reached from the internet (slower, fewer sources), port forwarding cannot fix it.");
        return v;
    }
    if (general && general->level == DiagLevel::Broken) {
        v.level = DiagLevel::Broken;
        v.sentence = general->result;
        return v;
    }
    // the reason for "not reachable" comes from the actual state, in this order
    bool noRouter = false, wrongRouter = false, blocked = false, mappingOff = false;
    if (general)
        for (const DiagItem &it : general->items) {
            if (it.id != QLatin1String("general.router")) continue;
            if (it.result == QLatin1String("Not found")) noRouter = true;
            else if (it.level == DiagLevel::Off) mappingOff = true;
            else if (it.level == DiagLevel::Limited) wrongRouter = true;
        }
    for (const DiagGroup &g : groups)
        for (const DiagItem &it : g.items)
            if (it.id.endsWith(QLatin1String(".blocked"))) blocked = true;
    if (!limitedReach.isEmpty()) {
        QString why;
        if (noRouter) why = QStringLiteral(": no router answered the port mapping requests, see General");
        else if (wrongRouter) why = QStringLiteral(": the UPnP router that answered is not your internet router, see General");
        else if (blocked) why = QStringLiteral(": the router forwards the ports, but something before it blocks them, see the yellow rows");
        else if (mappingOff) why = QStringLiteral(": automatic port mapping is off");
        v.level = DiagLevel::Limited;
        v.sentence = QStringLiteral("Works, but you cannot be reached from the internet on %1 (slower, fewer sources)%2.")
                         .arg(limitedReach.join(QStringLiteral(", ")), why);
        return v;
    }
    if (!limitedOther.isEmpty() || (general && general->level == DiagLevel::Limited)) {
        QStringList where = limitedOther;
        if (general && general->level == DiagLevel::Limited) where.prepend(QStringLiteral("General"));
        v.level = DiagLevel::Limited;
        v.sentence = QStringLiteral("Works, some checks need attention: %1 (yellow rows).").arg(where.join(QStringLiteral(", ")));
        return v;
    }
    if (!anyReachKnown) {
        v.level = DiagLevel::Unknown;
        v.sentence = QStringLiteral("Connected, reachability not known yet: press Test Now.");
        return v;
    }
    v.level = DiagLevel::Ok;
    v.sentence = QStringLiteral("Everything works.");
    return v;
}

NatVerdict natVerdict(const QString &routerWan, const QStringList &seenExternal)
{
    NatVerdict v;
    QSet<QString> seen;
    for (const QString &s : seenExternal)
        if (classifyAddress(s) == AddressKind::Public) seen.insert(s);
    if (routerWan == QLatin1String("0.0.0.0")) {
        v.level = DiagLevel::Limited;
        v.kind = QStringLiteral("router without internet address");
        v.text = QStringLiteral("The router that answered reports no internet address (0.0.0.0): it is not the device connected to the internet, so its port mappings do not help. Forward the ports on the router that is.");
        return v;
    }
    const AddressKind wk = classifyAddress(routerWan);
    if (wk == AddressKind::CarrierNat) {
        v.level = DiagLevel::Limited;
        v.kind = QStringLiteral("carrier-grade NAT");
        v.text = QStringLiteral("Your router's internet address %1 is a carrier-grade NAT address (100.64.0.0/10): your provider shares one public address among many customers. Port forwarding cannot work there; downloads still work. Ask your provider for a public IPv4 address if you want to be reachable.")
                     .arg(routerWan);
        return v;
    }
    if (wk == AddressKind::Private) {
        v.level = DiagLevel::Limited;
        v.kind = QStringLiteral("double NAT");
        v.text = QStringLiteral("Your router's internet address %1 is a private address: there is a second router (or your provider's NAT) in front of it. Forward the ports on both, or put the first one in bridge mode; otherwise you cannot be reached from the internet.")
                     .arg(routerWan);
        return v;
    }
    if (wk == AddressKind::Public) {
        if (!seen.isEmpty() && !seen.contains(routerWan)) {
            v.level = DiagLevel::Limited;
            v.kind = QStringLiteral("address mismatch");
            v.text = QStringLiteral("Other networks see you as %1, but your router says its address is %2: the traffic leaves through another router, a VPN or carrier-grade NAT. Port mappings on this router then do not help.")
                         .arg(QStringList(seen.values()).join(QStringLiteral(", ")), routerWan);
            return v;
        }
        v.level = DiagLevel::Ok;
        v.text = QStringLiteral("Your router has the public address %1, port forwarding can work.").arg(routerWan);
        return v;
    }
    if (seen.size() > 1) {
        v.level = DiagLevel::Limited;
        v.kind = QStringLiteral("address mismatch");
        v.text = QStringLiteral("The networks see you under different addresses (%1): several connections or a VPN are in use.")
                     .arg(QStringList(seen.values()).join(QStringLiteral(", ")));
        return v;
    }
    v.text = QStringLiteral("No router answered, so its internet address is unknown.");
    return v;
}

QString discoveryFailureText(const DiscoveryFacts &f)
{
    QStringList tried;
    if (f.upnpOn) {
        const QStringList ad = f.adapters;
        tried << (f.searchSkipped ? QStringLiteral("did not search itself (test switch MX_UPNP_FORCE_WINDOWS)")
                  : ad.isEmpty() ? QStringLiteral("searched for a UPnP router, but no adapter had an IPv4 address")
                               : QStringLiteral("searched for a UPnP router on %1 (%2)").arg(countNoun(ad.size(), QStringLiteral("adapter")), ad.join(QStringLiteral(", "))));
        if (f.unicastTried && !f.gateway.isEmpty()) tried << QStringLiteral("asked the gateway %1 directly").arg(f.gateway);
        if (f.cacheTried) tried << QStringLiteral("tried the router address remembered from before");
        if (f.notifyListening) tried << QStringLiteral("listened for router announcements");
        if (f.winTried) tried << QStringLiteral("asked Windows (%1)").arg(f.winResult);
    }
    if (f.natPmpOn) tried << QStringLiteral("asked NAT-PMP (%1)").arg(f.natPmpResult.isEmpty() ? QStringLiteral("no answer") : f.natPmpResult);
    QString text = QStringLiteral("OctoMX %1. No router answered.").arg(tried.join(QStringLiteral(", ")));
    int otherAnswers = 0;
    for (int a : f.answers) otherAnswers += a;
    const QString gw = f.gateway.isEmpty() ? QStringLiteral("your router") : QStringLiteral("your router (%1)").arg(f.gateway);
    QString reason;
    if (f.firewallChecked && f.firewallOn && f.firewallAllowRules == 0 && f.firewallProfiles.contains(QLatin1String("Public")))
        reason = QStringLiteral("Windows treats this network as Public and OctoMX has no firewall rule, so the routers' answers are blocked: set the network to Private in Windows Settings, Network & internet, or allow OctoMX through the firewall.");
    else if (f.firewallChecked && f.firewallOn && f.firewallAllowRules == 0)
        reason = QStringLiteral("Windows Firewall has no rule for OctoMX, so the routers' answers may be blocked: allow OctoMX in Windows Security, Firewall & network protection, Allow an app through firewall.");
    else if (otherAnswers > 0)
        reason = QStringLiteral("Other devices answered, so the search itself works on this network, but no router offers port mapping: UPnP is probably switched off in %1.").arg(gw);
    else if (f.winTried && f.winFoundNone)
        reason = QStringLiteral("Windows itself found no UPnP router either, so this is not an OctoMX problem: UPnP is probably switched off in %1, or the Wi-Fi access point does not pass these messages to the router.").arg(gw);
    else if (f.defaultIsVirtual)
        reason = QStringLiteral("Your internet route goes through the virtual or VPN adapter %1: the router behind it cannot be asked.").arg(f.defaultAdapter);
    else
        reason = QStringLiteral("No device answered on any adapter: UPnP may be switched off in %1, or the network (for example a Wi-Fi access point) does not pass these messages.").arg(gw);
    text += QStringLiteral(" Likely reason: ") + reason;
    text += QStringLiteral(" What to do: enable UPnP in %1, or forward the ports by hand (see each network below).").arg(gw);
    return text;
}

bool looksVirtualAdapter(const QString &name, const QString &humanName, QString *why)
{
    const QString n = (name + QLatin1Char(' ') + humanName).toLower();
    static const char *const vpn[] = {"wg", "wireguard", "tun", "tap", "vpn", "openvpn", "nordlynx", "tailscale", "zerotier",
                                      "ppp", "proton", "mullvad", "expressvpn", "surfshark", "fortinet", "anyconnect",
                                      "cisco", "globalprotect", "pangp", "utun", "ipsec", "hamachi", "radmin", "wintun",
                                      "nordvpn", "windscribe", "softether", "sstp", "l2tp", "pptp"};
    static const char *const virt[] = {"docker", "veth", "virbr", "vmnet", "vmware", "virtualbox", "vbox", "hyper-v",
                                       "vethernet", "br-", "lxc", "lxd", "podman", "cni", "flannel", "wsl", "loopback",
                                       "virtual adapter", "virtual ethernet", "npcap", "kvm", "parallels", "wi-fi direct"};
    const QStringList words = n.split(QRegularExpression(QStringLiteral("[^a-z0-9-]+")), Qt::SkipEmptyParts);
    for (const char *k : vpn) {
        const QString key = QLatin1String(k);
        for (const QString &w : words) {
            bool digitsAfter = w.startsWith(key) && w.size() > key.size();
            for (qsizetype k = key.size(); digitsAfter && k < w.size(); ++k) digitsAfter = w.at(k).isDigit();
            if (w == key || digitsAfter || w.startsWith(key + QLatin1Char('-')) || (key.size() >= 5 && w.contains(key))) {
                if (why) *why = QStringLiteral("VPN");
                return true;
            }
        }
    }
    for (const char *k : virt) {
        if (n.contains(QLatin1String(k))) {
            if (why) *why = QStringLiteral("virtual");
            return true;
        }
    }
    return false;
}

} // namespace mx::diag

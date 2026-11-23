#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes the synthetic response fixtures in tests/web/data/ used by tst_web.

Every fixture has the *shape* of a real response of the index it stands for (same keys, nesting,
XML/HTML structure and value formats, including the quirks the parser must cope with: null
fields, missing hashes, nested items, double-escaped entities, truncated text), but all content
is invented: titles are obviously fictional ("Example OS", "Sample Open Movie"), info hashes are
repeating hex patterns that cannot belong to any real torrent, and URLs that come from the
response body use example.org / example.net (RFC 2606). Nothing here was captured from a site.

    python3 tests/web/make_fixtures.py        # rewrites tests/web/data/*

The expected values in tst_web.cpp match this output; change both together.
"""
import json
import os

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")
TRK = "udp%3A%2F%2Ftracker.example.org%3A6969%2Fannounce&tr=udp%3A%2F%2Ftracker.example.net%3A1337%2Fannounce"


def h(p, n, upper=False):
    """Synthetic info hash: the two hex digits <provider><record> repeated 20 times."""
    s = ("%x%x" % (p, n)) * 20
    return s.upper() if upper else s


def write(name, text):
    with open(os.path.join(OUT, name), "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def dump(obj):
    return json.dumps(obj, indent=1, ensure_ascii=False) + "\n"


# ---------------------------------------------------------------- apibay (bare JSON array)
names = ["Example OS 22.04 LTS", "Example OS 24.04 LTS Server", "Sample Open Movie 1080p",
         "Fixture Audio Collection (CC0)", "Example OS 20.04 netinst"]
write("apibay.json", dump([{
    "id": str(1000001 + i), "name": n, "info_hash": h(1, i + 1, upper=True),
    "leechers": str([3, 1, 0, 2, 5][i]), "seeders": str([12, 7, 4, 9, 1][i]),
    "size": str([3000000000, 2100000000, 734003200, 52428800, 471859200][i]),
    "num_files": str([1, 1, 2, 14, 1][i]), "username": "fixture", "added": str(1700000000 + i * 86400),
    "status": "member", "category": ["303", "303", "201", "101", "303"][i], "imdb": ""}
    for i, n in enumerate(names)]))
# the index's own "nothing found" sentinel row (all-zero hash)
write("apibay-empty.json", dump([{
    "id": "0", "name": "No results returned", "info_hash": "0" * 40, "leechers": "0", "seeders": "0",
    "num_files": "0", "size": "0", "username": "", "added": "0", "status": "member", "category": "0",
    "imdb": "", "total_found": "1"}]))

# ---------------------------------------------------------------- torrents-csv
write("torrents-csv.json", dump({"torrents": [{
    "infohash": h(2, i + 1), "name": n, "size_bytes": [8000000, 1500000000, 650000000, 120000, 99000000][i],
    "created_unix": 1700000000 + i * 3600, "seeders": [27, 5, 3, 1, 0][i], "leechers": [0, 1, 2, 0, 0][i],
    "completed": 10 * i, "scraped_date": 1790000000, "id": 501 + i}
    for i, n in enumerate(["Example OS Handbook (fictional) 2nd ed", "Example OS 25.10 desktop amd64",
                           "Sample Open Movie 720p", "Fixture README pack", "Example OS 25.10 arm64"])],
    "next": 506}))

# ---------------------------------------------------------------- knaben
hits = []
for i, n in enumerate(["example-os-26.04-desktop-amd64.iso", "example-os-24.04.1-desktop-amd64.iso",
                       "Sample Open Movie 2160p", "example-os-26.04-server-amd64.iso", "Fixture Podcast Episode 12"]):
    hs = h(3, i + 1, upper=True)
    hits.append({
        "bytes": [6000000000, 5900000000, 4100000000, 2600000000, 41000000][i], "cachedOrigin": "Example Index (proxy)",
        "category": ["PC / Unix", "PC / Unix", "Movies", "PC / Unix", "Audio"][i], "categoryId": [4000000, 4004000],
        "date": ["2026-04-25T15:35:00+00:00", "2024-09-07T23:00:00+00:00", "2025-01-02T03:04:05+00:00",
                 "2026-04-24T10:00:00+00:00", "2023-06-01T12:00:00+00:00"][i],
        "description": None if i == 0 else "Uploader: fixture<br>" + n,
        "details": "https://index.example.org/description.php?id=%d" % (7001 + i), "grabs": 3 * i,
        "hash": hs, "id": h(0xc, i + 1), "lastSeen": "2026-11-20T18:11:00+00:00",
        "link": None if i == 0 else "https://index.example.org/download/%d.torrent" % (7001 + i),
        "livePeers": 4, "liveSeeders": 9,
        "magnetUrl": "magnet:?xt=urn:btih:%s&dn=%s&tr=%s" % (hs, n.replace(" ", "+"), TRK),
        "peers": [6, 2, 1, 0, 0][i], "score": None, "seeders": [19, 8, 3, 2, 1][i], "title": n,
        "tracker": "Example Index", "trackerId": "exampleindex", "virusDetection": 0.25})
write("knaben.json", dump({"hits": hits, "max_score": None, "total": {"relation": "eq", "value": 5}}))

# ---------------------------------------------------------------- bitsearch
write("bitsearch.json", dump({"success": True, "query": "example os", "results": [{
    "id": "fixture%017d" % (i + 1), "infohash": h(4, i + 1, upper=True), "title": n,
    "size": [2097152000, 4900000000, 4930000000, 1200000000, 800000000][i], "category": [1, 5, 5, 5, 2][i],
    "subCategory": [7, 4, 4, 4, 1][i], "genres": [], "seeders": [28, 17, 16, 4, 2][i],
    "leechers": [41, 3, 4, 1, 0][i], "downloads": 0, "verified": False, "createdAt": None,
    "updatedAt": "2026-11-12T09:%02d:00.000Z" % i}
    for i, n in enumerate(["example-os-19.04-desktop-amd64.iso", "example-os-22.04.2-desktop-amd64.iso",
                           "example-os-23.04-desktop-amd64.iso", "example-os-23.04-server-amd64.iso",
                           "Sample Open Movie OST"])],
    "pagination": {"page": 1, "perPage": 20, "total": 5, "totalPages": 1, "hasNext": False, "hasPrev": False},
    "took": 3}))

# ---------------------------------------------------------------- yts (movies[] → torrents[])
def yts_torrent(hs, q, size, bytes_, seeds, peers, date, unix):
    return {"url": "https://movies.example.org/torrent/download/" + hs, "hash": hs, "quality": q, "type": "web",
            "is_repack": "0", "video_codec": "x264", "bit_depth": "8", "audio_channels": "2.0", "seeds": seeds,
            "peers": peers, "size": size, "size_bytes": bytes_, "date_uploaded": date, "date_uploaded_unix": unix}


movies = []
for i, (title, year, slug) in enumerate([("Sample Open Movie", 2026, "sample-open-movie-2026"),
                                          ("Another Fictional Short", 2025, "another-fictional-short-2025")]):
    movies.append({
        "id": 9001 + i, "url": "https://movies.example.org/movies/" + slug, "imdb_code": "", "title": title,
        "title_english": title, "title_long": "%s (%d)" % (title, year), "slug": slug, "year": year,
        "rating": 7.0, "runtime": 12, "genres": ["Animation"], "yt_trailer_code": "", "language": "en",
        "mpa_rating": "", "state": "ok",
        "torrents": [yts_torrent(h(5, 2 * i + 1, True), "720p", "650.55 MB", 682151117, 22 - i, 5, "2026-11-05 15:43:48", 1793889828),
                     yts_torrent(h(5, 2 * i + 2, True), "1080p", "1.18 GB", 1267015352, 29 - i, 20, "2026-11-05 16:15:32", 1793891732)],
        "date_uploaded": "2026-11-05 15:43:48", "date_uploaded_unix": 1793889828})
write("yts.json", dump({"status": "ok", "status_message": "Query was successful",
                        "data": {"movie_count": 2, "limit": 20, "page_number": 1, "movies": movies},
                        "@meta": {"api_version": 2, "execution_time": "0.01 ms"}}))

# ---------------------------------------------------------------- nyaa (RSS with a namespace)
hs = h(6, 1)
write("nyaa.xml", """<rss xmlns:atom="http://www.w3.org/2005/Atom" xmlns:nyaa="https://nyaa.si/xmlns/nyaa" version="2.0">
	<channel>
		<title>Index - &#34;example&#34; - Torrent File RSS</title>
		<description>RSS Feed for &#34;example&#34;</description>
		<link>https://rss.example.org/</link>
		<atom:link href="https://rss.example.org/?page=rss" rel="self" type="application/rss+xml" />
		<item>
			<title>Example OS Live CD Release 3 (fictional, x86)</title>
				<link>https://rss.example.org/download/4242.torrent</link>
				<guid isPermaLink="true">https://rss.example.org/view/4242</guid>
				<pubDate>Tue, 03 Nov 2009 07:03:00 -0000</pubDate>

				<nyaa:seeders>0</nyaa:seeders>
				<nyaa:leechers>0</nyaa:leechers>
				<nyaa:downloads>0</nyaa:downloads>
				<nyaa:infoHash>%s</nyaa:infoHash>
			<nyaa:categoryId>6_1</nyaa:categoryId>
			<nyaa:category>Software - Applications</nyaa:category>
			<nyaa:size>624.0 MiB</nyaa:size>
			<nyaa:comments>0</nyaa:comments>
			<nyaa:trusted>No</nyaa:trusted>
			<nyaa:remake>No</nyaa:remake>
			<description><![CDATA[<a href="https://rss.example.org/view/4242">#4242 | Example OS Live CD Release 3 (fictional, x86)</a> | 624.0 MiB | Software - Applications | %s]]></description>
		</item>
	</channel>
</rss>""" % (hs, hs))

# ---------------------------------------------------------------- bt4g (single-line RSS, magnet links)
items = []
for i, (n, size, cat, date) in enumerate([
        ("example-os-22.04.5-desktop-amd64.iso", "4.44GB", "Archive file", "Tue,17 Nov 2026 22:14:06 -0000"),
        ("example-os-26.10-desktop-amd64_2026_10_13.iso", "6.16GB", "Archive file", "Mon,16 Nov 2026 10:00:00 -0000"),
        ("example-os-24.04-desktop-amd64", "320.00MB", "Archive file", "Sun,15 Nov 2026 09:00:00 -0000"),
        ("Sample [ Open ] Movie (fictional)", "1.02GB", "Video", "Sat,14 Nov 2026 08:00:00 -0000"),
        ("Fixture-Studio-1.0.1-Example-OS-x86_64", "516.53MB", "Other", "Fri,13 Nov 2026 07:00:00 -0000")]):
    hs = h(7, i + 1)
    items.append('<item><title>%s</title><link>magnet:?xt=urn:btih:%s&amp;dn=%s&amp;tr=%s</link>'
                 '<guid isPermaLink="true">https://rss.example.net/magnet/fixture%d</guid><pubDate>%s</pubDate>'
                 '<description><![CDATA[%s<br>%s<br>%s<br>%s]]></description></item>'
                 % (n, hs, n.replace(" ", "+"), TRK.replace("&", "&amp;"), i + 1, date, n, size, cat, hs))
write("bt4g.xml", '<rss version="2.0"><channel><title>Index - Search RSS for example</title>'
      '<description>RSS Feed for example</description><link>https://rss.example.net/search?q=example&amp;page=rss</link>'
      + "\n".join(items) + "</channel></rss>\n")

# ---------------------------------------------------------------- limetorrents (RSS 2.0, enclosure)
items = []
for i, (n, date, cat, size, s, l) in enumerate([
        ("[ Fixture ] Example OS Users, Groups and Permissions - Complete Guide (fictional)", "14 Nov 2026 11:40:20 +0200", "Other - Other", 1141536878, 41, 17),
        ("Sample Ensemble - Inspiration (2018, fictional)", "03 Nov 2026 00:25:12 +0200", "Music", 120000000, 5, 1),
        ("Example Tool v5 0 Example OS x64 --&amp;gt; portable", "01 Nov 2026 12:00:00 +0200", "Applications - Linux", 30000000, 2, 0),
        ("Example OS Bible (fictional) 11ed 2025", "28 Oct 2026 08:00:00 +0200", "Other - E-books", 8554738, 9, 2),
        ("The Ultimate Example OS Handbook (fictional), 2025", "20 Oct 2026 18:30:00 +0200", "Other - E-books", 15000000, 3, 0)]):
    hs = h(8, i + 1, upper=True)
    slug = "Fixture-%d" % (i + 1)
    items.append('<item><title>%s</title><pubDate>%s</pubDate><link>https://feed.example.org/%s-torrent-%d.html</link>'
                 '<category>%s</category><size>%d</size><description>Seeds: %d , Leechers %d</description>'
                 '<comments>https://feed.example.org/%s-torrent-%d.html</comments>'
                 '<enclosure url="https://cache.example.org/torrent/%s.torrent?title=%s" type="application/x-bittorrent" /></item>'
                 % (n, date, slug, 3001 + i, cat, size, s, l, slug, 3001 + i, hs, slug))
write("limetorrents.xml", """<?xml version='1.0' encoding='UTF-8' ?>
<rss version="2.0" xmlns:dc="http://purl.org/dc/elements/1.1/">
	<channel>
		<title>Index - Search RSS for example</title>
		<link>https://feed.example.org/</link>
		<description>Synthetic test feed</description>
		<language>en-us</language>
		<pubDate>Fri, 20 Nov 2026 19:58:04 +0200</pubDate>
		<lastBuildDate>Fri, 20 Nov 2026 19:58:04 +0200</lastBuildDate>
		<generator>fixture</generator>
""" + "\n".join(items) + "\n\t</channel>\n</rss>\n")

# ---------------------------------------------------------------- Internet Archive (advancedsearch + metadata/files)
docs = [
    {"btih": h(9, 1), "identifier": "example_short_720p_fixture", "item_size": 2194559, "mediatype": "movies",
     "publicdate": "2025-08-10T06:15:42Z", "title": "Generic image"},
    {"btih": h(9, 2), "identifier": "sample_open_movie_fixture", "item_size": 167352831, "mediatype": "movies",
     "publicdate": "2019-10-02T10:00:46Z", "title": "Sample Open Movie (fictional)"},
    {"identifier": "fixture_radio_20190409", "item_size": 29814013, "mediatype": "audio",
     "publicdate": "2019-04-09T09:24:59Z", "title": "Fixture Radio : April 09, 2019 (fictional)"},
    {"identifier": "fixture_radio_20170906", "item_size": 90379318, "mediatype": "audio",
     "publicdate": "2017-09-06T07:31:33Z", "title": "Fixture Radio : September 06, 2017 (fictional)"},
    {"btih": h(9, 5), "identifier": "example_short_1080p_fixture.mov", "item_size": 1513427684, "mediatype": "movies",
     "publicdate": "2022-10-15T19:10:02Z", "title": "Example Short 1080p"}]
write("internet-archive.json", dump({
    "responseHeader": {"status": 0, "QTime": 12, "params": {
        "query": "(text:sample OR text__reviews:sample)", "qin": "sample open movie",
        "fields": "identifier,title,item_size,mediatype,publicdate,btih", "wt": "json", "rows": 10, "start": 0}},
    "response": {"numFound": 5, "start": 0, "docs": docs}}))
ident = "sample_open_movie_fixture"
write("internet-archive-files.json", dump({"result": [
    {"name": "Sample Open Movie - fictional.mkv", "source": "original", "mtime": "1570010445", "size": "81782463",
     "md5": "0" * 31 + "1", "crc32": "00000001", "sha1": "0" * 39 + "1", "format": "Matroska", "length": "464.14",
     "height": "858", "width": "2048"},
    {"name": "Sample Open Movie - fictional.mp4", "source": "derivative", "format": "h.264",
     "original": "Sample Open Movie - fictional.mkv", "mtime": "1570012489", "size": "48274047",
     "md5": "0" * 31 + "2", "crc32": "00000002", "sha1": "0" * 39 + "2", "length": "464.15", "height": "480", "width": "1146"},
    {"name": "Sample Open Movie - fictional.ogv", "source": "derivative", "format": "Ogg Video",
     "original": "Sample Open Movie - fictional.mkv", "mtime": "1570013630", "size": "36721117",
     "md5": "0" * 31 + "3", "crc32": "00000003", "sha1": "0" * 39 + "3", "length": "464.13", "height": "300", "width": "716"},
    {"name": "__ia_thumb.jpg", "source": "original", "mtime": "1613698669", "size": "4741", "md5": "0" * 31 + "4",
     "crc32": "00000004", "sha1": "0" * 39 + "4", "format": "Item Tile", "rotation": "0"},
    {"name": ident + "_archive.torrent", "source": "metadata", "btih": h(9, 2), "mtime": "1658611141", "size": "8671",
     "md5": "0" * 31 + "5", "crc32": "00000005", "sha1": "0" * 39 + "5", "format": "Archive BitTorrent"},
    {"name": ident + "_files.xml", "source": "original", "format": "Metadata", "md5": "0" * 31 + "6", "summation": "md5"},
    {"name": ident + "_meta.sqlite", "source": "original", "mtime": "1570010515", "size": "11264", "format": "Metadata",
     "md5": "0" * 31 + "7", "crc32": "00000007", "sha1": "0" * 39 + "7"},
    {"name": ident + "_meta.xml", "source": "original", "mtime": "1613698669", "size": "951", "format": "Metadata",
     "md5": "0" * 31 + "8", "crc32": "00000008", "sha1": "0" * 39 + "8"},
    {"name": ident + "_reviews.xml", "source": "original", "mtime": "1658611139", "size": "1051",
     "md5": "0" * 31 + "9", "crc32": "00000009", "sha1": "0" * 39 + "9", "format": "Metadata"}]}))

# ---------------------------------------------------------------- linuxtracker (HTML table, regex provider)
rows = []
for i, (n, cat, size, date, s, l, c) in enumerate([
        ("Example-OS-13.5-amd64.iso", "Example Live", "4.1 GB", "Oct 25, 2026", 7, 0, 30),
        ("Example-OS-13.3-amd64.iso", "Example Live", "4.3 GB", "Oct 25, 2026", 7, 0, 18),
        ("Example-OS-13.2-amd64.iso", "Example Live", "4.3 GB", "Oct 25, 2026", 8, 1, 12),
        ("Fixture-Linux-2.0-arm64.img", "Fixture Linux", "1.2 GB", "Sep 01, 2026", 3, 0, 5),
        ("Fixture-Linux-2.0-x86_64.iso", "Fixture Linux", "2.0 GB", "Sep 01, 2026", 4, 2, 9)]):
    rows.append("""<tr>
          <td class="torrent-name-cell">
            <a class="torrent-name" href="/torrents/%s/">%s</a>
            <span class="torrent-mobile-meta">%s · %s · %s</span>
          </td>
          <td><span class="category-badge">%s</span></td>
          <td class="muted-cell">%s</td>
          <td class="numeric muted-cell">%s</td>
          <td class="numeric seeds stat-stack">
            <strong>%d</strong>

          </td>
          <td class="numeric leeches stat-stack">
            <strong>%d</strong>

          </td>
          <td class="numeric stat-stack">
            <strong>%d</strong>

          </td>
        </tr>""" % (h(0xa, i + 1), n, cat, size, date, cat, date, size, s, l, c))
write("linuxtracker.html", '<html><body><table><thead><tr><th>Name</th></tr></thead>\n<tbody>\n'
      + "\n".join(rows) + "\n</tbody></table></body></html>\n")
print("wrote fixtures to", OUT)

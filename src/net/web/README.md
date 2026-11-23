# mxnet_web: web torrent index search

`mx::createWebSearchPlugin()` (header `web_plugin.h`) returns a search-only `NetworkPlugin`
(`net() == Net::Web`, `capabilities() == CanSearch`). BitTorrent has no keyword search, so this
plugin asks public torrent index sites and APIs (the same idea as qBittorrent's search plugins). It
returns `SearchHit`s that the BitTorrent module downloads from the magnet / info-hash.

## Structure

| file | what |
|---|---|
| `search_backend.h` | abstract `SearchBackend` (async `search`/`cancel`, signals `results`/`finished`/`healthChanged`) + `TorrentRecord`. Non-HTTP backends (rats-search, a local DHT index) implement this and are added with `WebSearchPlugin::addBackend()`. |
| `http_index_provider.*` | `HttpIndexProvider`: a `SearchBackend` driven by one provider definition (rate limit, timeouts, 4 MB cap, paging, per-item detail requests, back-off). Parsing runs on the thread pool. |
| `provider_def.*` | JSON → `ProviderDef`; loading the built-in list plus user overrides. |
| `response_parser.*` | JSON / RSS / XML / regex response → `TorrentRecord`s. These are pure functions. |
| `transforms.*` | size parsing, info-hash (hex/base32/magnet), dates, HTML entities, magnets, bundle rule. |
| `hit_builder.*` | record → `SearchHit`, query filter, adult filter, de-duplication (`HitMerger`). |
| `web_plugin.cpp`, `web_search_plugin.h` | the `NetworkPlugin` facade. |
| `providers/providers.json` | built-in providers, compiled in as `:/mx/web/providers.json`. |

## Where providers come from

Files are loaded in this order. A later file wins by provider `id`:

1. built-in `:/mx/web/providers.json`
2. `<ctx.dataDir>/providers.json` (normally `<profile>/web/providers.json`)
3. `<ctx.dataDir>/providers.d/*.json` (alphabetical). `addServer(pathOrUrl)` installs files here.

An object whose `id` already exists is **merged** into the earlier one, key by key. Nested objects
are merged recursively; arrays and scalars are replaced. So `{"id": "apibay", "enabled": false}`
just disables apibay, and `{"id":"apibay","limits":{"timeoutMs":5000}}` only changes its timeout.
An unknown `id` adds a provider. A file may hold `{"providers": [...]}`, a bare array, or one object.
A malformed provider is skipped and a log message is written. A malformed file never stops the others.

User toggles from the Networks page (`connectServer`/`disconnectServer`) are stored in the setting
`web/providerEnabled` (`{id: bool}`). They override the `enabled` value from the files.
`removeServer` deletes a user file under `providers.d/` that defines only that provider. Any other
provider is disabled instead.

### Which built-in providers are enabled by default (one place)

The top-level `"defaults"` object of the built-in `providers/providers.json` is the only place that
decides this. Each key is a profile, its value the list of provider ids that start enabled
(`["*"]` = all). Every other built-in provider is still loaded and listed, but starts disabled.
The build picks the profile:

```bash
tools/configure-linux.sh build/x                                        # "standard": all enabled (default)
tools/configure-linux.sh build/x -DMX_WEB_DEFAULT_PROFILE=conservative  # only internet-archive + linuxtracker
```

The profile is applied right after the built-in file, before `<dataDir>/providers.json`,
`providers.d/` and the `web/providerEnabled` toggles, so a user can always switch any provider
on or off. To change what a profile enables, edit its list; to add a profile, add a key and pass
its name. An unknown profile name logs a warning and enables everything.

## Provider definition schema

```jsonc
{
  "id": "apibay",                       // required, unique
  "name": "The Pirate Bay (apibay)",    // display name (Networks page, source "connection" column)
  "homepage": "https://apibay.org/",    // ServerEntry.address
  "notes": "free text",
  "enabled": true,                      // default true
  "request": {
    "url": "https://apibay.org/q.php?q={query}&cat={category}",   // required, http(s)
    "method": "GET",                    // GET | POST
    "body": "{\"query\":{query_json}}", // POST body template
    "contentType": "application/json",  // POST only (default application/json)
    "headers": { "Accept": "application/json" },
    "spaceAs": "%20",                   // how spaces in {query} are encoded: "%20" | "+" | "-" ...
    "categories": {                     // value of {category} per mx::MediaType
      "any": "0", "audio": "100", "video": "200", "image": "603",
      "document": "601", "software": "300", "archive": "...", "other": "600"
    },                                  // an unmapped type uses "any" (or "" when it is missing too)
    "strictCategories": false,          // true: types not listed are not searched on this provider at all
    "probeQuery": "ubuntu"              // small query used for the health check
  },
  "paging": { "first": 1, "maxPages": 1, "pageSize": 0 },
      // {page} starts at `first`; {offset} = (page-first)*pageSize. The next page is fetched only while
      // the page was full (>= pageSize items; any item when pageSize is 0) and maxResults is not reached.
  "limits": { "minIntervalMs": 1000, "timeoutMs": 10000, "maxBytes": 4194304 },
      // minimum delay between two requests to this provider; per-request timeout (1-60 s);
      // response size cap (at most 4 MB; bigger responses are aborted)
  "response": {
    "format": "json",                   // json | rss | xml | regex (alias html)
    "items": "data.movies[].torrents[]",// where the result rows are, see below
    "decimalUnits": false,              // "1.4 GB" = 1.4*1024^3 unless true
    "fields": { ... },                  // see "Fields"
    "files": { ... }                    // optional file list, see "Files"
  },
  "details": {                          // optional second request per row (JSON), e.g. a file list
    "url": "https://archive.org/metadata/{identifier|urlencode}/files",
    "key": "details",                   // the parsed document is stored in the row under this key
    "maxItems": 8,                      // only for the first N rows of each page
    "timeoutMs": 8000
  },
  "categoryTypes": [ { "match": "^2\\d\\d$", "type": "video" } ],  // regex on `category` → MediaType
  "adultCategories": [ "^5\\d\\d$" ],   // regexes on `category`; such rows are dropped by search/adultFilter
  "defaultType": "video",               // MediaType when no categoryTypes rule matches
  "matchName": true,                    // drop rows whose name (or a listed file path) lacks a query word
  "trackers": [ "udp://..." ]           // trackers of this index, added to the magnets of its rows
}
```

Request placeholders: `{query}` is URL-encoded in the URL (per `spaceAs`) and raw in the body.
`{query_raw}` is raw. `{query_json}` is a JSON string literal with quotes. The others are `{page}`,
`{offset}`, `{pageSize}`, `{maxResults}`, `{category}` (inserted verbatim) and `{adult_hide}`
(`true`/`false` = `search/adultFilter`).

### Items

* **json**: a dotted path, where `[]` iterates an array. `"[]"` means the root is the array,
  `"hits[]"` iterates the `hits` array, and `"data.movies[].torrents[]"` flattens nested arrays (one
  row per torrent). Each row keeps its *context stack*: root, outer items, then the item itself.
* **rss / xml**: an element name (`item`, `entry`). Every descendant element becomes a key:
  `title`, `nyaa:seeders` (also `seeders` without the prefix). Attributes are keyed
  `element@attr`, for example `enclosure@url`. A truncated document keeps the complete items.
* **regex**: a regular expression run over the whole body (dot matches newline, case-insensitive).
  Each match is one row, with keys `_` (the whole match), `1`, `2`, ... and the named groups.
  Use it as a last resort for HTML.

### Fields

Field names: `name` (required), `infoHash`, `magnet`, `size`, `seeders`, `leechers`, `numFiles`,
`published`, `category`, `torrentUrl`, `httpUrl`, `detailsUrl`. A row needs an info-hash (from
`infoHash` or `magnet`) or an `httpUrl`. Otherwise it is skipped.

A field is either a path string (`"seeders"`) or an object:

```jsonc
{ "path": "description",          // value path: "a.b", "a.0.b", "../title" (outer item), "$.x" (root);
                                  //   plain paths are looked up in the item, then outwards
  "template": "{title_long} [{quality}]",  // instead of path: "{path|filter|...}" expansion;
                                  //   filters: urlencode, urlpath, lower, upper, trim, html, json
  "value": "Movies",              // instead of path: a constant
  "regex": "Seeds:\\s*([0-9,]+)", // optional: applied to the text; named group "v", else group 1
  "transform": "magnet",          // optional (see below)
  "format": "dd.MM.yyyy" }        // Qt date format tried first for `published`
```

Transforms:

* `infoHash` accepts 40 hex or 32 base32 characters (an all-zero hash is rejected). With
  `"transform": "magnet"`, it takes the btih from a magnet link.
* `size` accepts plain bytes or human-readable sizes (`1.4 GiB`, `650.55 MB`, `4.44GB`, `1,234 KB`).
  Units are binary unless `decimalUnits` is set.
* `seeders`, `leechers` and `numFiles` take the leading integer.
* `published` accepts unix seconds or ms, ISO 8601, RFC 2822 (also `Tue,17 Nov 2026 ...`), and
  `Oct 25, 2026`.
* `html` strips tags and decodes entities. `lower` lower-cases.
* Names always get their HTML entities decoded. They also get the "UTF-8 read as cp1252"
  double encoding repaired.
* URL fields (`torrentUrl`, `httpUrl`, `detailsUrl`) may use `{@infoHash}` (hex), `{@INFOHASH}`
  and `{@name}` in templates.

### Files

```jsonc
"files": { "items": "details.result[]",      // item-relative items path
           "path": "name", "size": "size",
           "url": { "template": "https://archive.org/download/{identifier|urlencode}/{name|urlpath}" },
           "filter": { "field": "source", "regex": "^original$" },   // keep matching entries only
           "exclude": "_meta\\.xml$",                                 // drop entries whose path matches
           "maxFiles": 2000 }
```

Without filter/exclude, the list counts as complete. Then `numFiles` comes from it and
`BundleFile.index` is the list position. A filtered list leaves `numFiles` unknown and indexes `-1`.

## Mapping to `mx::SearchHit`

* `fileName` is the torrent name. `size`, `published` and `type` are set (see below).
* `hashes.btih` holds the 20 raw bytes and `btFileIndex = -1`.
* **Bundle rule:** `isBundle` is true when the provider says more than one file. It is false when
  the provider says exactly one. When the count is unknown, it is true only if the name has no known
  file extension (`mx::mediaTypeForFileName(name) == Other`), so a name that looks like a folder.
  When hits are merged, an explicit file count from any provider wins. Without one, a name with an
  extension from any provider makes the hit a single file.
* `files` are listed only for bundles, when the provider lists them.
* `type`: a known extension of the name comes first. Then the provider category mapping. Then the
  largest listed file. Then name keywords (`1080p`/`x264`/`S01E02` → Video, `FLAC`/`MP3` → Audio,
  `epub`/`pdf` → Document, `iso`/`amd64`/`linux` → Software). Otherwise `Other`.
* **One BitTorrent `SearchSource` per provider row**: `net = BitTorrent`, `uri` = a complete
  magnet, `connection` = provider name, `seeds`, `peers` = leechers. `extra` holds `provider`,
  `providerName`, `providers` (every index that knows this torrent), and `torrentUrl`,
  `detailsUrl`, `category` when known.
* The magnet carries btih, `dn`, `xl`, the trackers from `web/trackers` (default
  `bt/extraTrackers`), the provider's `trackers`, and up to 5 trackers from the provider's own
  magnet. It is fixed when the torrent is first seen, so every source of the same torrent has the
  same URI.
* **HTTP sources**: `net = Http`, one for `httpUrl` and one per listed file with a `url` (at most 20).
  `extra.bundlePath` is the path inside the bundle and `extra.size` is the file size. The Internet
  Archive provides these.
* **De-duplication** is done inside the plugin, per search, by info-hash. Sources are merged (one
  per provider; a repeated row replaces that provider's source). Each emission re-sends the whole
  merged hit. BitTorrent sources are sorted by ascending seeds, so when the core collapses
  equal-URI sources the maximum seeds value wins.

## Search behaviour

* The plugin answers when `SearchQuery::networks` is empty or contains `Web` or `BitTorrent`.
* `text` and `text2` are joined for the request. Every word must appear in the name or a listed file
  path, unless the provider sets `matchName: false`.
* Hits are dropped when the type is known and different from the requested type. `extension`
  checks the name, then the listed files, then for a bundle without files a word in the name
  (`[FLAC]`). `minSize`/`maxSize` need a known size.
* `maxResults` is capped by `web/maxResults`. Each backend gets a fair share of the cap, so the
  fastest index cannot fill it alone. A backend may go over its share only by what the backends
  still running leave free.
* Results stream in: one `searchHits` per provider page. `searchDone` comes when every provider has
  finished or failed, or at the deadline (`web/timeoutSec`, default 25 s, at most
  `search/timeoutSec`).
* `cancelSearch` aborts the requests. No `searchDone` follows.
* Health: each answer records the latency (shown as the Networks-page ping). After a failure (HTTP
  error, timeout, unparsable body, too large) the provider is skipped for 30 s, then 1 min, 2 min,
  and so on up to 30 min.
* `status()` is Connected when at least one provider answered in the last 30 minutes. Its text reads
  like "7 of 10 indexes responding".
* A health check runs 3 s after `start()` (`web/probeOnStart`) and every 10 minutes for providers
  not asked in the last 15 minutes.
* Proxy: the `connection/proxy*` settings (http and socks5; Qt has no socks4) are applied to the
  plugin's `QNetworkAccessManager`. `http/verifyTls` is honoured.
* User-Agent comes from `web/userAgent`. The default is a desktop Firefox UA, because several
  indexes reject unknown agents.
* Safe search: `search/adultFilter` drops rows in the provider's `adultCategories` and rows whose
  name or category contains a word from `web/adultKeywords` (built-in list by default).
  Knaben is also asked with `hide_xxx`.

## Settings keys used that the core schema does not define yet

`web/userAgent` (String), `web/providerEnabled` (Map), `web/adultKeywords` (StringList),
`web/timeoutSec` (Int, default 25), `web/probeOnStart` (Bool, default true) and `web/trackers`
(StringList, default empty = `bt/extraTrackers`). The defaults live in the plugin
(`web_search_plugin.h`, namespace `mx::web::wkeys`). The existing keys `web/maxResults`,
`search/adultFilter`, `search/timeoutSec`, `bt/extraTrackers`, `connection/proxy*` and
`http/verifyTls` are used as well. `web/sources` and `web/localIndex` are left to the follow-up
rats/DHT backends.

// SPDX-License-Identifier: GPL-3.0-or-later
// Test data for tst_web.cpp (raw string literals; kept out of moc's sight).
#pragma once

static const char kData1[] = R"({"providers":[
          {"id":"json","name":"JSON idx","request":{"url":"%1/json?q={query}"},"limits":{"minIntervalMs":0},
           "response":{"format":"json","fields":{"name":"name","infoHash":"info_hash","seeders":"seeders","size":"size"}}},
          {"id":"rss","name":"RSS idx","request":{"url":"%1/rss?q={query}"},"limits":{"minIntervalMs":0},
           "response":{"format":"rss","fields":{"name":"title","infoHash":"hash","seeders":"seeds"}}},
          {"id":"slow","name":"Slow idx","request":{"url":"%1/slow?q={query}"},"limits":{"timeoutMs":1000},
           "response":{"format":"json","fields":{"name":"name","infoHash":"h"}}},
          {"id":"huge","name":"Huge idx","request":{"url":"%1/huge?q={query}"},
           "response":{"format":"json","fields":{"name":"name","infoHash":"h"}}}
        ]})";

static const char kData2[] = R"([{"id":"s","request":{"url":"%1/s?q={query}"},
            "response":{"format":"json","fields":{"name":"n","infoHash":"h"}}}])";

static const char kData3[] = R"({"providers":[
            {"id":"apibay","enabled":false,"limits":{"timeoutMs":5000}},
            {"id":"mine","name":"My index","request":{"url":"https://example.org/s?q={query}"},
             "response":{"format":"json","items":"rows[]","fields":{"name":"n","infoHash":"h"}}},
            {"id":"broken","request":{"url":"ftp://x"}},
            {"name":"no id"}
        ]})";

static const char kData4[] = R"([{"id":"mine","name":"My index v2"}])";

static const char kData5[] = R"({"not":"a list"})";

static const char kData6[] = R"([1, "x", null, {"name":"no hash"}, {"info_hash":"2c6b6858d61da9543d4231a71db4b1c9264b0685"},
                                {"name":"ok","info_hash":"2c6b6858d61da9543d4231a71db4b1c9264b0685","size":"n/a","seeders":"-3"},
                                {"name":{"deep":1},"info_hash":[1,2]}])";

static const char kData7[] = R"([{"name":"x","info_hash":"2c6b6858d61da9543d4231a71db4b1c9264b0685","category":"501"}])";

static const char kData8[] = R"([
            {"name":"Big Buck Bunny 1080p","info_hash":"2c6b6858d61da9543d4231a71db4b1c9264b0685","seeders":"10","size":"1000"},
            {"name":"Big Buck Bunny XXX","info_hash":"d540fc48eb12f2833163eed6421d449dd8f1ce1f","seeders":"1","size":"1000"},
            {"name":"Unrelated","info_hash":"9d2afc3b48bfdb0d809e9696c4391edb9b7fd0e5","seeders":"1","size":"1000"}])";

static const char kData9[] = R"({"id":"x","request":{"url":"nope"}})";

static const char kData10[] = R"({"id":"my-index","name":"My Index","request":{"url":"https://example.org/?q={query}"},
                      "response":{"format":"rss","fields":{"name":"title","infoHash":"hash"}}})";

static const char kData11[] = R"({"id":"apibay","name":"TPB renamed"})";

static const char kData12[] = R"([{"id":"yts","enabled":false}])";


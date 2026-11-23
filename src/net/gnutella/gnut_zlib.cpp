// SPDX-License-Identifier: GPL-3.0-or-later
#include "gnut_zlib.h"

#include <zlib.h>

namespace mx::gnut {

struct Inflater::Z { z_stream s{}; };
struct Deflater::Z { z_stream s{}; };

Inflater::Inflater() : m(new Z)
{
    if (inflateInit(&m->s) != Z_OK) m_err = true;
}
Inflater::~Inflater() { inflateEnd(&m->s); }
qint64 Inflater::totalIn() const { return qint64(m->s.total_in); }
qint64 Inflater::totalOut() const { return qint64(m->s.total_out); }

bool Inflater::feed(const char *in, int len, QByteArray *out, int maxOut)
{
    if (m_err) return false;
    if (len <= 0) return true;
    if (m_end) return false;                     // data after the end of the stream
    m->s.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(in));
    m->s.avail_in = uInt(len);
    char buf[16384];
    int produced = 0;
    while (m->s.avail_in > 0) {
        m->s.next_out = reinterpret_cast<Bytef *>(buf);
        m->s.avail_out = sizeof buf;
        const int r = inflate(&m->s, Z_SYNC_FLUSH);
        const int n = int(sizeof buf - m->s.avail_out);
        if (n > 0) {
            produced += n;
            if (produced > maxOut) { m_err = true; return false; }
            out->append(buf, n);
        }
        if (r == Z_STREAM_END) {
            m_end = true;
            if (m->s.avail_in > 0) { m_err = true; return false; }
            break;
        }
        if (r == Z_BUF_ERROR && n == 0) break;   // needs more input
        if (r != Z_OK && r != Z_BUF_ERROR) { m_err = true; return false; }
    }
    // drain pending output if any (avail_in == 0 but output buffer was full)
    while (!m_end && m->s.avail_out == 0) {
        m->s.next_out = reinterpret_cast<Bytef *>(buf);
        m->s.avail_out = sizeof buf;
        const int r = inflate(&m->s, Z_SYNC_FLUSH);
        const int n = int(sizeof buf - m->s.avail_out);
        if (n > 0) {
            produced += n;
            if (produced > maxOut) { m_err = true; return false; }
            out->append(buf, n);
        }
        if (r == Z_STREAM_END) { m_end = true; break; }
        if (r != Z_OK) break;
    }
    return true;
}

Deflater::Deflater(int level) : m(new Z) { deflateInit(&m->s, level); }
Deflater::~Deflater() { deflateEnd(&m->s); }
qint64 Deflater::totalIn() const { return qint64(m->s.total_in); }
qint64 Deflater::totalOut() const { return qint64(m->s.total_out); }

static QByteArray runDeflate(z_stream &s, const char *in, int len, int flush)
{
    QByteArray out;
    s.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(in));
    s.avail_in = uInt(qMax(0, len));
    char buf[16384];
    do {
        s.next_out = reinterpret_cast<Bytef *>(buf);
        s.avail_out = sizeof buf;
        const int r = deflate(&s, flush);
        out.append(buf, int(sizeof buf - s.avail_out));
        if (r == Z_STREAM_ERROR) break;
        if (r == Z_STREAM_END) break;
    } while (s.avail_out == 0 || s.avail_in > 0);
    return out;
}

QByteArray Deflater::compress(const char *in, int len) { return runDeflate(m->s, in, len, Z_SYNC_FLUSH); }
QByteArray Deflater::finish() { return runDeflate(m->s, nullptr, 0, Z_FINISH); }

QByteArray inflateAll(const QByteArray &in, int maxOut, bool *ok)
{
    Inflater inf;
    QByteArray out;
    const bool good = inf.feed(in, &out, maxOut) && inf.finished();
    if (ok) *ok = good;
    return good ? out : QByteArray();
}

QByteArray deflateAll(const QByteArray &in, int level)
{
    uLongf outLen = compressBound(uLong(in.size()));
    QByteArray out(int(outLen), Qt::Uninitialized);
    if (compress2(reinterpret_cast<Bytef *>(out.data()), &outLen, reinterpret_cast<const Bytef *>(in.constData()),
                  uLong(in.size()), level) != Z_OK)
        return {};
    out.resize(int(outLen));
    return out;
}

} // namespace mx::gnut

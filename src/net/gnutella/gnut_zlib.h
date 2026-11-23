// SPDX-License-Identifier: GPL-3.0-or-later
// Gnutella module: streaming zlib (deflate) used for compressed Gnutella connections
// ("Content-Encoding: deflate"), GGEP compression, QRP patches and browse-host bodies.
#pragma once

#include <QtCore/QByteArray>
#include <memory>

namespace mx::gnut {

// Streaming inflater (zlib format, i.e. with the 2-byte header, as Gnutella uses).
class Inflater {
public:
    Inflater();
    ~Inflater();
    Inflater(const Inflater &) = delete;
    Inflater &operator=(const Inflater &) = delete;
    // Inflates `in`, appending to *out, never producing more than maxOut bytes in this call
    // (memory cap). Returns false on a stream error or if the cap would be exceeded.
    bool feed(const char *in, int len, QByteArray *out, int maxOut);
    bool feed(const QByteArray &in, QByteArray *out, int maxOut) { return feed(in.constData(), int(in.size()), out, maxOut); }
    bool finished() const { return m_end; }
    qint64 totalIn() const;
    qint64 totalOut() const;
private:
    struct Z;
    std::unique_ptr<Z> m;
    bool m_end = false;
    bool m_err = false;
};

// Streaming deflater; every compress() call ends with Z_SYNC_FLUSH so the peer can decode
// everything sent so far.
class Deflater {
public:
    explicit Deflater(int level = 6);
    ~Deflater();
    Deflater(const Deflater &) = delete;
    Deflater &operator=(const Deflater &) = delete;
    QByteArray compress(const char *in, int len);
    QByteArray compress(const QByteArray &in) { return compress(in.constData(), int(in.size())); }
    QByteArray finish();                          // Z_FINISH (end of stream)
    qint64 totalIn() const;
    qint64 totalOut() const;
private:
    struct Z;
    std::unique_ptr<Z> m;
};

// One-shot helpers. inflateAll fails (*ok=false) on error, truncated input or output > maxOut.
QByteArray inflateAll(const QByteArray &in, int maxOut, bool *ok = nullptr);
QByteArray deflateAll(const QByteArray &in, int level = 9);

} // namespace mx::gnut

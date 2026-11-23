// SPDX-License-Identifier: GPL-3.0-or-later
// eMule "protocol obfuscation" for TCP streams (RC4 keyed by MD5, first 1024 key-stream bytes
// discarded). Pure state machine, no sockets: the connection feeds received bytes in and sends
// whatever takeOutput() returns.
//
// Client <-> client (A connects to B, A must know B's user hash):
//   keys   A send = B recv = MD5(<B hash 16><34><rand 4>),  A recv = B send = MD5(<B hash 16><203><rand 4>)
//   A -> B  <marker 1, not E3/C5/D4><rand 4> then RC4: <0x835E6FC4 4><methods 1><preferred 1><padlen 1><pad>
//   B -> A  RC4: <0x835E6FC4 4><method 1><padlen 1><pad>      → both sides now encrypt everything
// Client -> server (Diffie-Hellman, 768-bit prime, g = 2):
//   C -> S  <marker 1><g^a 96>[random 0..15]           S -> C <g^b 96> RC4: <magic><methods><pref><padlen><pad>
//   C -> S  RC4: <magic 4><method 1><padlen 1><pad>    keys: C send = MD5(<S 96><34>), C recv = MD5(<S 96><203>)
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QVector>

namespace mx::ed2k {

class Rc4 {
public:
    Rc4() = default;
    Rc4(const QByteArray &key, bool discard1024 = true) { setKey(key, discard1024); }
    void setKey(const QByteArray &key, bool discard1024 = true);
    void process(char *data, qsizetype n);
    QByteArray process(const QByteArray &in) { QByteArray b = in; process(b.data(), b.size()); return b; }
    bool valid() const { return m_valid; }
private:
    quint8 m_s[256];
    quint8 m_x = 0, m_y = 0;
    bool m_valid = false;
};

// minimal unsigned big integer for the 768-bit DH of server obfuscation
class BigUInt {
public:
    BigUInt() = default;
    static BigUInt fromBigEndian(const QByteArray &b);
    static BigUInt fromU32(quint32 v) { BigUInt r; if (v) r.m_l.append(v); return r; }
    QByteArray toBigEndian(int bytes) const;
    static BigUInt powMod(const BigUInt &base, const BigUInt &exp, const BigUInt &mod);
    static BigUInt mulMod(const BigUInt &a, const BigUInt &b, const BigUInt &m);
    int bitLength() const;
    bool bit(int i) const;
    bool isZero() const { return m_l.isEmpty(); }
    static int cmp(const BigUInt &a, const BigUInt &b);
private:
    void trim();
    QVector<quint32> m_l;   // little-endian limbs
};

QByteArray ed2kDhPrime();   // the fixed 96-byte prime (big-endian)

class StreamCrypt {
public:
    enum State { Off, Detecting, Negotiating, Encrypting, Failed };

    // Not obfuscated. Received bytes pass through unchanged.
    void setPlain() { m_state = Off; }
    // Outgoing client connection; we know the target's user hash. Puts the request into output.
    void startClient(const QByteArray &targetUserHash, int maxPadding = 128,
                     quint32 randomKeyPart = 0, bool fixedRandom = false);
    // Outgoing server connection (DH).
    void startServer();
    // Incoming connection: decides from the first byte whether the peer uses obfuscation.
    void startIncoming(const QByteArray &ourUserHash, bool requireObfuscation, int maxPadding = 128);

    // Feed raw received bytes; returns decrypted application bytes (possibly empty).
    QByteArray receive(const QByteArray &raw);
    // Encrypt application bytes (in place semantics: returns the bytes to put on the wire).
    // While still negotiating, the data is held back and released by takeOutput() once ready.
    QByteArray send(const QByteArray &plain);
    // Handshake bytes (and held-back application data) waiting to be written.
    QByteArray takeOutput() { QByteArray o; o.swap(m_out); return o; }
    bool hasOutput() const { return !m_out.isEmpty(); }

    State state() const { return m_state; }
    bool ready() const { return m_state == Off || m_state == Encrypting; }
    bool obfuscated() const { return m_state == Encrypting; }
    bool failed() const { return m_state == Failed; }
    QString error() const { return m_error; }
    bool detectedPlain() const { return m_detectedPlain; }

    static quint8 randomMarker();

private:
    enum Step { None, A_WaitMagic, A_WaitMethodPad, A_WaitPadding,
                B_WaitRandom, B_WaitMagic, B_WaitMethods, B_WaitPadding,
                S_WaitDh, S_WaitMagic, S_WaitMethods, S_WaitPadding };
    void fail(const QString &why);
    void setupClientKeys(const QByteArray &hash, quint32 rnd, bool weAreA);
    void becomeReady();
    QByteArray padding(int maxLen);

    State m_state = Off;
    Step m_step = None;
    Rc4 m_send, m_recv;
    QByteArray m_out;
    QByteArray m_in;          // handshake bytes not yet consumed
    QByteArray m_pending;     // application data queued while negotiating
    QByteArray m_ourHash;
    QByteArray m_dhA;         // our DH secret (big-endian)
    int m_want = 0;
    int m_maxPad = 128;
    bool m_requireObf = false;
    bool m_detectedPlain = false;
    QString m_error;
};

} // namespace mx::ed2k

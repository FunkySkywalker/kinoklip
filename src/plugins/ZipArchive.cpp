// SPDX-License-Identifier: GPL-3.0-or-later
// ZipArchive — see ZipArchive.h. The inflater follows the structure of zlib's public domain
// "puff.c" (canonical Huffman decoding by code length counts), written for clarity, not speed:
// a plugin is a few kilobytes of text.
#include "plugins/ZipArchive.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace kk::plugins::zip {

namespace {

// ---- Inflate (RFC 1951) ----

constexpr int kMaxBits = 15;
constexpr int kLitCodes = 288;
constexpr int kDistCodes = 30;

struct Huffman
{
    short count[kMaxBits + 1] = {};
    short symbol[kLitCodes] = {};
};

struct Inflater
{
    const uchar* in = nullptr;
    qsizetype inLen = 0;
    qsizetype inPos = 0;
    quint32 bitBuf = 0;
    int bitCount = 0;
    QByteArray* out = nullptr;
    qint64 maxBytes = 0;
    bool failed = false;

    int bits(int need)
    {
        quint32 value = bitBuf;
        while (bitCount < need) {
            if (inPos >= inLen) {
                failed = true;
                return 0;
            }
            value |= quint32(in[inPos++]) << bitCount;
            bitCount += 8;
        }
        bitBuf = value >> need;
        bitCount -= need;
        return int(value & ((1u << need) - 1));
    }

    int decode(const Huffman& h)
    {
        int code = 0;
        int first = 0;
        int index = 0;
        for (int len = 1; len <= kMaxBits; ++len) {
            code |= bits(1);
            if (failed)
                return -1;
            const int count = h.count[len];
            if (code - count < first)
                return h.symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        failed = true;
        return -1;
    }
};

/// Builds the canonical code of @p n lengths. Returns the number of unused codes (negative:
/// over-subscribed, positive: incomplete).
int construct(Huffman& h, const short* length, int n)
{
    for (short& c : h.count)
        c = 0;
    for (int i = 0; i < n; ++i)
        ++h.count[length[i]];
    if (h.count[0] == n)
        return 0;
    int left = 1;
    for (int len = 1; len <= kMaxBits; ++len) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0)
            return left;
    }
    short offs[kMaxBits + 1];
    offs[1] = 0;
    for (int len = 1; len < kMaxBits; ++len)
        offs[len + 1] = short(offs[len] + h.count[len]);
    for (int i = 0; i < n; ++i) {
        if (length[i] != 0)
            h.symbol[offs[length[i]]++] = short(i);
    }
    return left;
}

constexpr short kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51,
    59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr short kLenExtra[29]
    = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr short kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257,
    385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr short kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9,
    10, 10, 11, 11, 12, 12, 13, 13};

bool codes(Inflater& s, const Huffman& lit, const Huffman& dist)
{
    for (;;) {
        const int symbol = s.decode(lit);
        if (s.failed || symbol < 0)
            return false;
        if (symbol < 256) {
            if (s.out->size() >= s.maxBytes)
                return false;
            s.out->append(char(symbol));
        } else if (symbol == 256) {
            return true;
        } else {
            const int li = symbol - 257;
            if (li >= 29)
                return false;
            const int len = kLenBase[li] + s.bits(kLenExtra[li]);
            const int ds = s.decode(dist);
            if (s.failed || ds < 0 || ds >= kDistCodes)
                return false;
            const qsizetype d = kDistBase[ds] + s.bits(kDistExtra[ds]);
            if (s.failed || d > s.out->size() || s.out->size() + len > s.maxBytes)
                return false;
            // Byte by byte: the copy may overlap what it writes.
            for (int i = 0; i < len; ++i)
                s.out->append(s.out->at(s.out->size() - d));
        }
    }
}

bool dynamic(Inflater& s)
{
    static constexpr short kOrder[19]
        = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    const int nlen = s.bits(5) + 257;
    const int ndist = s.bits(5) + 1;
    const int ncode = s.bits(4) + 4;
    if (s.failed || nlen > 286 || ndist > kDistCodes)
        return false;
    short lengths[kLitCodes + kDistCodes] = {};
    for (int i = 0; i < ncode; ++i)
        lengths[kOrder[i]] = short(s.bits(3));
    if (s.failed)
        return false;
    Huffman lencode;
    if (construct(lencode, lengths, 19) != 0)
        return false;
    int index = 0;
    short all[kLitCodes + kDistCodes] = {};
    while (index < nlen + ndist) {
        const int symbol = s.decode(lencode);
        if (s.failed || symbol < 0)
            return false;
        if (symbol < 16) {
            all[index++] = short(symbol);
        } else {
            int len = 0;
            int repeat = 0;
            if (symbol == 16) {
                if (index == 0)
                    return false;
                len = all[index - 1];
                repeat = 3 + s.bits(2);
            } else if (symbol == 17) {
                repeat = 3 + s.bits(3);
            } else {
                repeat = 11 + s.bits(7);
            }
            if (s.failed || index + repeat > nlen + ndist)
                return false;
            while (repeat--)
                all[index++] = short(len);
        }
    }
    if (all[256] == 0)
        return false;
    Huffman lit;
    Huffman dist;
    const int litLeft = construct(lit, all, nlen);
    if (litLeft < 0 || (litLeft > 0 && nlen - lit.count[0] != 1))
        return false;
    const int distLeft = construct(dist, all + nlen, ndist);
    if (distLeft < 0 || (distLeft > 0 && ndist - dist.count[0] != 1))
        return false;
    return codes(s, lit, dist);
}

bool fixedBlock(Inflater& s)
{
    static Huffman lit;
    static Huffman dist;
    static const bool built = [] {
        short lengths[kLitCodes];
        int i = 0;
        for (; i < 144; ++i)
            lengths[i] = 8;
        for (; i < 256; ++i)
            lengths[i] = 9;
        for (; i < 280; ++i)
            lengths[i] = 7;
        for (; i < kLitCodes; ++i)
            lengths[i] = 8;
        construct(lit, lengths, kLitCodes);
        short dl[kDistCodes];
        for (short& d : dl)
            d = 5;
        construct(dist, dl, kDistCodes);
        return true;
    }();
    Q_UNUSED(built);
    return codes(s, lit, dist);
}

// ---- Zip structure ----

struct Entry
{
    QString name;
    quint16 method = 0;
    quint32 crc = 0;
    quint32 compressed = 0;
    quint32 size = 0;
    quint32 localOffset = 0;
    bool isDir = false;
};

quint16 rd16(const QByteArray& b, qsizetype o)
{
    return quint16(uchar(b[o]) | (uchar(b[o + 1]) << 8));
}
quint32 rd32(const QByteArray& b, qsizetype o)
{
    return quint32(rd16(b, o)) | (quint32(rd16(b, o + 2)) << 16);
}

/// A name that stays inside the target folder.
bool safeName(const QString& name)
{
    if (name.isEmpty() || name.startsWith(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))
        || name.contains(QLatin1Char('\0')) || name.contains(QLatin1Char(':')))
        return false;
    for (const QString& part : name.split(QLatin1Char('/'))) {
        if (part == QLatin1String(".."))
            return false;
    }
    return true;
}

bool readDirectory(const QByteArray& zip, QVector<Entry>* entries, QString* error)
{
    auto fail = [error](const QString& m) {
        if (error)
            *error = m;
        return false;
    };
    if (zip.size() < 22)
        return fail(QStringLiteral("not a zip file (too short)"));
    qsizetype eocd = -1;
    const qsizetype lowest = std::max<qsizetype>(0, zip.size() - 22 - 65535);
    for (qsizetype i = zip.size() - 22; i >= lowest; --i) {
        if (rd32(zip, i) == 0x06054b50u) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0)
        return fail(QStringLiteral("not a zip file (no end record)"));
    const int count = rd16(zip, eocd + 10);
    const quint32 cdOffset = rd32(zip, eocd + 16);
    if (count > kMaxEntries)
        return fail(QStringLiteral("the archive has more than %1 entries").arg(kMaxEntries));
    qsizetype pos = cdOffset;
    qint64 total = 0;
    for (int i = 0; i < count; ++i) {
        if (pos + 46 > zip.size() || rd32(zip, pos) != 0x02014b50u)
            return fail(QStringLiteral("the zip directory is damaged"));
        Entry e;
        const quint16 flags = rd16(zip, pos + 8);
        e.method = rd16(zip, pos + 10);
        e.crc = rd32(zip, pos + 16);
        e.compressed = rd32(zip, pos + 20);
        e.size = rd32(zip, pos + 24);
        const int nameLen = rd16(zip, pos + 28);
        const int extraLen = rd16(zip, pos + 30);
        const int commentLen = rd16(zip, pos + 32);
        e.localOffset = rd32(zip, pos + 42);
        if (pos + 46 + nameLen > zip.size())
            return fail(QStringLiteral("the zip directory is damaged"));
        e.name = QString::fromUtf8(zip.mid(pos + 46, nameLen));
        pos += 46 + nameLen + extraLen + commentLen;
        if (flags & 1)
            return fail(QStringLiteral("encrypted zip entries are not supported"));
        if (e.size == 0xFFFFFFFFu || e.compressed == 0xFFFFFFFFu || e.localOffset == 0xFFFFFFFFu)
            return fail(QStringLiteral("zip64 archives are not supported"));
        e.isDir = e.name.endsWith(QLatin1Char('/'));
        if (!safeName(e.name))
            return fail(QStringLiteral("unsafe file name in the archive: %1").arg(e.name));
        if (e.size > kMaxEntryBytes)
            return fail(QStringLiteral("%1 is larger than %2 MiB")
                            .arg(e.name)
                            .arg(kMaxEntryBytes / (1024 * 1024)));
        total += e.size;
        if (total > kMaxTotalBytes)
            return fail(QStringLiteral("the archive unpacks to more than %1 MiB")
                            .arg(kMaxTotalBytes / (1024 * 1024)));
        entries->append(e);
    }
    return true;
}

bool unpack(const QByteArray& zip, const Entry& e, QByteArray* data, QString* error)
{
    auto fail = [error, &e](const QString& m) {
        if (error)
            *error = QStringLiteral("%1: %2").arg(e.name, m);
        return false;
    };
    const qsizetype lo = e.localOffset;
    if (lo + 30 > zip.size() || rd32(zip, lo) != 0x04034b50u)
        return fail(QStringLiteral("local header damaged"));
    const qsizetype start = lo + 30 + rd16(zip, lo + 26) + rd16(zip, lo + 28);
    if (start + qsizetype(e.compressed) > zip.size())
        return fail(QStringLiteral("data runs past the end of the file"));
    const QByteArray raw = zip.mid(start, qsizetype(e.compressed));
    if (e.method == 0) {
        if (e.compressed != e.size)
            return fail(QStringLiteral("size mismatch"));
        *data = raw;
    } else if (e.method == 8) {
        if (!inflate(raw, e.size, kMaxEntryBytes, data))
            return fail(QStringLiteral("corrupt compressed data"));
        if (quint32(data->size()) != e.size)
            return fail(QStringLiteral("size mismatch"));
    } else {
        return fail(QStringLiteral("compression method %1 is not supported").arg(e.method));
    }
    if (crc32(*data) != e.crc)
        return fail(QStringLiteral("checksum mismatch"));
    return true;
}

bool readAll(const QString& path, QByteArray* data, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("cannot read %1").arg(path);
        return false;
    }
    if (f.size() > kMaxTotalBytes) {
        if (error)
            *error = QStringLiteral("%1 is too large").arg(path);
        return false;
    }
    *data = f.readAll();
    return true;
}

void put16(QByteArray& b, quint16 v)
{
    b.append(char(v & 0xFF));
    b.append(char(v >> 8));
}
void put32(QByteArray& b, quint32 v)
{
    put16(b, quint16(v & 0xFFFF));
    put16(b, quint16(v >> 16));
}

} // namespace

quint32 crc32(const QByteArray& data)
{
    static const std::array<quint32, 256> table = [] {
        std::array<quint32, 256> t {};
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    quint32 c = 0xFFFFFFFFu;
    for (const char ch : data)
        c = table[(c ^ uchar(ch)) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

bool inflate(const QByteArray& in, qint64 expectedSize, qint64 maxBytes, QByteArray* out)
{
    out->clear();
    out->reserve(int(std::min<qint64>(std::max<qint64>(expectedSize, 0), maxBytes)));
    Inflater s;
    s.in = reinterpret_cast<const uchar*>(in.constData());
    s.inLen = in.size();
    s.out = out;
    s.maxBytes = maxBytes;
    int last = 0;
    do {
        last = s.bits(1);
        const int type = s.bits(2);
        if (s.failed)
            return false;
        if (type == 0) {
            s.bitBuf = 0; // stored block: skip to the byte boundary
            s.bitCount = 0;
            if (s.inPos + 4 > s.inLen)
                return false;
            const quint32 len = quint32(s.in[s.inPos]) | (quint32(s.in[s.inPos + 1]) << 8);
            const quint32 nlen = quint32(s.in[s.inPos + 2]) | (quint32(s.in[s.inPos + 3]) << 8);
            s.inPos += 4;
            if (len != (~nlen & 0xFFFFu) || s.inPos + qsizetype(len) > s.inLen
                || out->size() + qint64(len) > maxBytes)
                return false;
            out->append(reinterpret_cast<const char*>(s.in + s.inPos), qsizetype(len));
            s.inPos += qsizetype(len);
        } else if (type == 1) {
            if (!fixedBlock(s))
                return false;
        } else if (type == 2) {
            if (!dynamic(s))
                return false;
        } else {
            return false;
        }
    } while (!last);
    return true;
}

bool extractAll(const QString& zipPath, const QString& targetDir, QString* error)
{
    QByteArray zip;
    QVector<Entry> entries;
    if (!readAll(zipPath, &zip, error) || !readDirectory(zip, &entries, error))
        return false;
    const QDir target(targetDir);
    const QString root = QDir::cleanPath(target.absolutePath()) + QLatin1Char('/');
    for (const Entry& e : std::as_const(entries)) {
        const QString path = QDir::cleanPath(target.absoluteFilePath(e.name));
        if (!(path + QLatin1Char('/')).startsWith(root)) {
            if (error)
                *error = QStringLiteral("unsafe file name in the archive: %1").arg(e.name);
            return false;
        }
        if (e.isDir) {
            QDir().mkpath(path);
            continue;
        }
        QByteArray data;
        if (!unpack(zip, e, &data, error))
            return false;
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile out(path);
        if (!out.open(QIODevice::WriteOnly) || out.write(data) != data.size()) {
            if (error)
                *error = QStringLiteral("cannot write %1").arg(path);
            return false;
        }
    }
    return true;
}

QStringList listFiles(const QString& zipPath, QString* error)
{
    QByteArray zip;
    QVector<Entry> entries;
    if (!readAll(zipPath, &zip, error) || !readDirectory(zip, &entries, error))
        return {};
    QStringList names;
    for (const Entry& e : std::as_const(entries)) {
        if (!e.isDir)
            names.append(e.name);
    }
    return names;
}

bool readFile(const QString& zipPath, const QString& name, QByteArray* data, QString* error)
{
    QByteArray zip;
    QVector<Entry> entries;
    if (!readAll(zipPath, &zip, error) || !readDirectory(zip, &entries, error))
        return false;
    for (const Entry& e : std::as_const(entries)) {
        if (e.name == name && !e.isDir)
            return unpack(zip, e, data, error);
    }
    if (error)
        *error = QStringLiteral("%1 is not in the archive").arg(name);
    return false;
}

bool createFromFolder(const QString& sourceDir, const QString& zipPath, QString* error)
{
    const QDir dir(sourceDir);
    QStringList names;
    QDirIterator it(sourceDir, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (it.hasNext())
        names.append(dir.relativeFilePath(it.next()));
    std::sort(names.begin(), names.end());
    if (names.size() > kMaxEntries) {
        if (error)
            *error = QStringLiteral("more than %1 files").arg(kMaxEntries);
        return false;
    }
    QByteArray body;
    QByteArray central;
    for (const QString& name : std::as_const(names)) {
        QFile f(dir.absoluteFilePath(name));
        if (!f.open(QIODevice::ReadOnly)) {
            if (error)
                *error = QStringLiteral("cannot read %1").arg(name);
            return false;
        }
        const QByteArray data = f.readAll();
        const QByteArray utf8 = name.toUtf8();
        const quint32 crc = crc32(data);
        const quint32 offset = quint32(body.size());
        // Local header: signature, version 20, flags (UTF-8 names), stored, DOS time 0 / date
        // 1980-01-01, crc, sizes, name length, no extra field.
        put32(body, 0x04034b50u);
        put16(body, 20);
        put16(body, 0x0800);
        put16(body, 0);
        put16(body, 0);
        put16(body, 0x21);
        put32(body, crc);
        put32(body, quint32(data.size()));
        put32(body, quint32(data.size()));
        put16(body, quint16(utf8.size()));
        put16(body, 0);
        body.append(utf8);
        body.append(data);
        put32(central, 0x02014b50u);
        put16(central, 20);
        put16(central, 20);
        put16(central, 0x0800);
        put16(central, 0);
        put16(central, 0);
        put16(central, 0x21);
        put32(central, crc);
        put32(central, quint32(data.size()));
        put32(central, quint32(data.size()));
        put16(central, quint16(utf8.size()));
        put16(central, 0);
        put16(central, 0);
        put16(central, 0);
        put16(central, 0);
        put32(central, 0);
        put32(central, offset);
        central.append(utf8);
    }
    QByteArray end;
    put32(end, 0x06054b50u);
    put16(end, 0);
    put16(end, 0);
    put16(end, quint16(names.size()));
    put16(end, quint16(names.size()));
    put32(end, quint32(central.size()));
    put32(end, quint32(body.size()));
    put16(end, 0);
    QFile out(zipPath);
    if (!out.open(QIODevice::WriteOnly) || out.write(body) != body.size()
        || out.write(central) != central.size() || out.write(end) != end.size()) {
        if (error)
            *error = QStringLiteral("cannot write %1").arg(zipPath);
        return false;
    }
    return true;
}

} // namespace kk::plugins::zip

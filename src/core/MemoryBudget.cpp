// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/MemoryBudget.h"

#include <QByteArray>
#include <QFile>

#include <algorithm>
#include <atomic>

#ifdef __GLIBC__
#include <malloc.h>
#endif

namespace kk::membudget {

namespace {
std::atomic<int> s_limitGiB {0};

int64_t cgroupLimit()
{
    for (const char* path : {"/sys/fs/cgroup/memory.max", "/sys/fs/cgroup/memory/memory.limit_in_bytes"}) {
        QFile f(QString::fromLatin1(path));
        if (f.open(QIODevice::ReadOnly)) {
            bool ok = false;
            const qlonglong v = f.readAll().trimmed().toLongLong(&ok);
            if (ok && v > 0 && v < (1LL << 50))
                return v;
        }
    }
    return 0;
}
} // namespace

int64_t totalFromMeminfo(const QByteArray& meminfo)
{
    for (const QByteArray& line : meminfo.split('\n')) {
        if (line.startsWith("MemTotal:"))
            return line.mid(9).simplified().split(' ').value(0).toLongLong() * 1024;
    }
    return -1;
}

int64_t totalRamBytes()
{
    static const int64_t total = [] {
        int64_t ram = 0;
        QFile f(QStringLiteral("/proc/meminfo"));
        if (f.open(QIODevice::ReadOnly))
            ram = std::max<int64_t>(0, totalFromMeminfo(f.readAll()));
        if (const int64_t cg = cgroupLimit(); cg > 0 && (ram == 0 || cg < ram))
            ram = cg;
        return ram;
    }();
    return total;
}

int64_t limitFor(int64_t ramBytes, int userLimitGiB)
{
    if (userLimitGiB > 0)
        return int64_t(userLimitGiB) << 30;
    return ramBytes > 0 ? ramBytes : kFullSpeedBytes;
}

void setLimitGiB(int gib)
{
    s_limitGiB = std::max(0, gib);
}

int limitGiB()
{
    return s_limitGiB;
}

int64_t limitBytes()
{
    // KINOKLIP_MEMORY_LIMIT_GB: diagnostics and tests (pretend a smaller machine); wins over
    // the preference.
    bool ok = false;
    const int env = qEnvironmentVariableIntValue("KINOKLIP_MEMORY_LIMIT_GB", &ok);
    return limitFor(totalRamBytes(), ok && env > 0 ? env : s_limitGiB.load());
}

double scaleFor(int64_t limit)
{
    return std::clamp(double(limit) / double(kFullSpeedBytes), kMinScale, 1.0);
}

double scale()
{
    return scaleFor(limitBytes());
}

int64_t scaledBytes(int64_t full, int64_t floor)
{
    return std::min(full, std::max(floor, int64_t(double(full) * scale())));
}

int scaledCount(int full, int floor)
{
    return int(std::min<int64_t>(full, std::max<int64_t>(floor, int64_t(std::lround(full * scale())))));
}

void tuneAllocator()
{
#ifdef __GLIBC__
    // mallopt() with M_MMAP_THRESHOLD turns glibc's dynamic threshold off: without it the
    // threshold grows to 32 MB after the first big free and 4K frames then live in the heap,
    // where one small neighbour keeps the whole region resident for good (measured: 950 MB
    // retained after playback). Frames are pooled, so the extra mmap calls are rare.
    if (!qEnvironmentVariableIsSet("GLIBC_TUNABLES")) {
        mallopt(M_MMAP_THRESHOLD, 1 << 20);
        mallopt(M_TRIM_THRESHOLD, 16 << 20);
        mallopt(M_ARENA_MAX, 4);
    }
#endif
}

void trimHeap()
{
#ifdef __GLIBC__
    malloc_trim(0);
#endif
}

QString describe()
{
    return QStringLiteral("memory budget: %1 MiB RAM, limit %2 MiB%3, caches at %4 %")
        .arg(totalRamBytes() >> 20)
        .arg(limitBytes() >> 20)
        .arg(s_limitGiB > 0 ? QStringLiteral(" (set in Preferences)") : QString())
        .arg(int(scale() * 100 + 0.5));
}

} // namespace kk::membudget

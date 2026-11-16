// SPDX-License-Identifier: GPL-3.0-or-later
// MemoryBudget: the scale of the caches follows the machine's memory (or the user's limit), is
// 1 from 8 GB up and never below the floor; meminfo parsing.
#include "core/MemoryBudget.h"

#include <QtTest>

using namespace kk::membudget;

class TestMemBudget : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void parsesMeminfo()
    {
        QCOMPARE(totalFromMeminfo("MemTotal:       16000000 kB\nMemFree: 1 kB\n"),
            int64_t(16000000) * 1024);
        QCOMPARE(totalFromMeminfo("MemFree: 1 kB\n"), int64_t(-1));
    }

    void scaleFollowsTheLimit()
    {
        QCOMPARE(scaleFor(16LL << 30), 1.0);
        QCOMPARE(scaleFor(8LL << 30), 1.0);
        QCOMPARE(scaleFor(4LL << 30), 0.5);
        QCOMPARE(scaleFor(2LL << 30), 0.25);
        QCOMPARE(scaleFor(256LL << 20), kMinScale); // never below the floor
    }

    void aUserLimitWinsOverTheMachine()
    {
        QCOMPARE(limitFor(32LL << 30, 0), int64_t(32LL << 30));
        QCOMPARE(limitFor(32LL << 30, 4), int64_t(4LL << 30));
        QCOMPARE(limitFor(0, 0), kFullSpeedBytes); // unknown RAM: nothing is reduced
    }

    void scaledCapsStayBetweenFloorAndFull()
    {
        setLimitGiB(2); // 25 %
        QCOMPARE(scaledBytes(256 << 20, 24 << 20), int64_t(64 << 20));
        QCOMPARE(scaledBytes(256 << 20, 100 << 20), int64_t(100 << 20)); // floor
        QCOMPARE(scaledCount(8, 3), 3);
        setLimitGiB(64);
        QCOMPARE(scaledBytes(256 << 20, 24 << 20), int64_t(256 << 20)); // never above full
        QCOMPARE(scaledCount(8, 3), 8);
        setLimitGiB(0);
    }
};

QTEST_APPLESS_MAIN(TestMemBudget)
#include "test_membudget.moc"

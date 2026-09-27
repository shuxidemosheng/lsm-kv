// bench.cpp —— 阶段 5：基准测试（数据进 README / 复试素材）
//
// 场景（各 10 万次）：
//   1. 顺序 put（触发 flush/compaction 的真实工作负载）
//   2. 随机 put
//   3. 命中 get（一半在 MemTable 一半在 SST，取决于上一步的量）
//   4. 未命中 get（纯布隆路径）
//   5. sync_wal=true 的 put（对照：持久性代价）
#include "lsm_kv.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>

using Clock = std::chrono::steady_clock;
static double Since(Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

int main() {
    const std::string dir = "/tmp/lsmkv_bench";
    std::filesystem::remove_all(dir);

    lsm::LsmOptions opts;
    opts.dir = dir;
    opts.memtable_limit = 1 << 20;       // 1MB：真实一点的 flush 频率
    opts.l0_compact_trigger = 4;

    lsm::LsmKV* db = lsm::LsmKV::Open(opts, nullptr);
    if (!db) { printf("open failed\n"); return 1; }

    const int kN = 100000;
    std::mt19937 rng(1);

    // 1. 顺序 put
    auto t0 = Clock::now();
    for (int i = 0; i < kN; ++i)
        db->Put("user" + std::to_string(i), std::string(64, 'x'));
    printf("seq put        : %9.0f ops/s\n", kN / Since(t0));

    // 2. 随机 put（覆盖已有 key， MemTable 内存命中率不同）
    t0 = Clock::now();
    for (int i = 0; i < kN; ++i)
        db->Put("user" + std::to_string(rng() % kN), std::string(64, 'y'));
    printf("random put     : %9.0f ops/s\n", kN / Since(t0));

    // 3. 命中 get
    t0 = Clock::now();
    std::string v;
    for (int i = 0; i < kN; ++i)
        db->Get("user" + std::to_string(rng() % kN), &v);
    printf("hit get        : %9.0f ops/s\n", kN / Since(t0));

    // 4. 未命中 get（布隆快速排除）
    t0 = Clock::now();
    for (int i = 0; i < kN; ++i)
        db->Get("ghost" + std::to_string(i), &v);
    printf("miss get(bloom): %9.0f ops/s\n", kN / Since(t0));

    // 5. 范围扫描
    t0 = Clock::now();
    uint64_t rows = 0;
    for (int i = 0; i < 100; ++i)
        rows += db->Scan("user" + std::to_string(i * 500),
                         "user" + std::to_string(i * 500 + 400)).size();
    printf("scan           : %9.0f scans/s (%llu rows)\n",
           100.0 / Since(t0), static_cast<unsigned long long>(rows));

    delete db;

    // 6. sync_wal=true 对照
    lsm::LsmOptions sync_opts = opts;
    sync_opts.sync_wal = true;
    lsm::LsmKV* db2 = lsm::LsmKV::Open(sync_opts, nullptr);
    t0 = Clock::now();
    for (int i = 0; i < 10000; ++i)
        db2->Put("sync" + std::to_string(i), std::string(64, 'z'));
    printf("put (fsync wal): %9.0f ops/s  (10k ops)\n", 10000.0 / Since(t0));
    delete db2;

    return 0;
}

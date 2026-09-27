// test_lsm.cpp —— 阶段 2 验收：引擎 put/delete/get + flush + 布隆 + 重开恢复
#include "lsm_kv.h"

#include <cstdio>
#include <filesystem>
#include <map>
#include <random>
#include <string>

static int g_fail = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "CHECK failed: %s (line %d)\n", #cond, __LINE__); \
            ++g_fail;                                                      \
        }                                                                  \
    } while (0)

int main() {
    const std::string dir = "/tmp/lsmkv_engine";
    std::filesystem::remove_all(dir);   // 清理上次运行的数据（无 shell 参与）

    lsm::LsmOptions opts;
    opts.dir = dir;
    opts.memtable_limit = 64 * 1024;        // 64KB：快速触发 flush
    opts.l0_compact_trigger = 4;
    opts.sync_wal = false;

    std::map<std::string, std::pair<std::string, bool>> oracle;
    std::mt19937 rng(99);

    // 1. 20000 次随机写（put/delete），必然触发多次 flush（可能多次 compaction）
    {
        lsm::LsmKV* db = lsm::LsmKV::Open(opts, nullptr);
        CHECK(db != nullptr);
        for (int i = 0; i < 20000; ++i) {
            std::string k = "k" + std::to_string(rng() % 3000);
            if (rng() % 5 == 0) {
                CHECK(db->Delete(k));
                oracle[k] = {"", true};
            } else {
                std::string v = "v" + std::to_string(i) + "-" + std::string(20, 'x');
                CHECK(db->Put(k, v));
                oracle[k] = {v, false};
            }
            // 每 2000 次抽查
            if (i % 2000 == 0) {
                auto it = oracle.find(k);
                std::string v;
                bool expect = it != oracle.end() && !it->second.second;
                bool got = db->Get(k, &v);
                CHECK(got == expect);
            }
        }
        printf("after writes: l0=%zu mem=%zuKB\n", db->l0_count(),
               db->memtable_size() / 1024);
        delete db;   // 优雅关闭：flush 后删 WAL
    }

    // 2. 重新打开，全量对拍（覆盖 MemTable/L0/L1 三条读路径）
    {
        lsm::LsmKV* db = lsm::LsmKV::Open(opts, nullptr);
        CHECK(db != nullptr);
        for (const auto& [k, vt] : oracle) {
            std::string v;
            bool got = db->Get(k, &v);
            bool expect = !vt.second;
            CHECK(got == expect);
            if (got) CHECK(v == vt.first);
        }
        // 布隆过滤器压力：查大量不存在的 key（验证无误判导致的假阳性）
        for (int i = 0; i < 5000; ++i) {
            std::string v;
            CHECK(db->Get("absent" + std::to_string(i), &v) == false);
        }
        delete db;
    }

    if (g_fail) { printf("FAILED: %d\n", g_fail); return 1; }
    printf("ALL LSM TESTS PASSED (%zu keys)\n", oracle.size());
    return 0;
}

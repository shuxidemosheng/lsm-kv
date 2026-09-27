// test_scan.cpp —— 阶段 5：范围扫描与 oracle 对拍
#include "lsm_kv.h"

#include <cstdio>
#include <filesystem>
#include <map>
#include <random>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "CHECK failed: %s (line %d)\n", #cond, __LINE__); \
            ++g_fail;                                                      \
        }                                                                  \
    } while (0)

int main() {
    const std::string dir = "/tmp/lsmkv_scan";
    std::filesystem::remove_all(dir);

    lsm::LsmOptions opts;
    opts.dir = dir;
    opts.memtable_limit = 32 * 1024;     // 强制多次 flush + compaction
    opts.l0_compact_trigger = 3;
    opts.sync_wal = false;

    std::map<std::string, std::pair<std::string, bool>> oracle;
    std::mt19937 rng(2024);

    {
        lsm::LsmKV* db = lsm::LsmKV::Open(opts, nullptr);
        CHECK(db != nullptr);
        for (int i = 0; i < 30000; ++i) {
            std::string k = "key" + std::to_string(1000000 + rng() % 900000);
            if (rng() % 6 == 0) {
                CHECK(db->Delete(k));
                oracle[k] = {"", true};
            } else {
                std::string v = "v" + std::to_string(i);
                CHECK(db->Put(k, v));
                oracle[k] = {v, false};
            }
        }

        // 500 次随机范围扫描对拍
        for (int q = 0; q < 500; ++q) {
            uint32_t a = 1000000 + rng() % 850000;
            std::string begin = "key" + std::to_string(a);
            std::string end = "key" + std::to_string(a + 1 + rng() % 50000);
            auto got = db->Scan(begin, end);

            // oracle 版答案（只含"存活"记录，升序）
            std::vector<std::pair<std::string, std::string>> expect;
            for (auto it = oracle.lower_bound(begin);
                 it != oracle.end() && it->first < end; ++it)
                if (!it->second.second) expect.push_back({it->first, it->second.first});

            CHECK(got.size() == expect.size());
            if (got.size() == expect.size())
                for (size_t i = 0; i < got.size(); ++i) {
                    CHECK(got[i].key == expect[i].first);
                    CHECK(got[i].value == expect[i].second);
                }
        }
        delete db;
    }

    if (g_fail) { printf("FAILED: %d\n", g_fail); return 1; }
    printf("ALL SCAN TESTS PASSED (%zu keys)\n", oracle.size());
    return 0;
}

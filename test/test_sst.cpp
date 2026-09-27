// test_sst.cpp —— 阶段 1 验收：构建 → 落盘 → 重新打开 → 点查 + 迭代
#include "sst.h"

#include <cassert>
#include <cstdio>
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
    const std::string path = "/tmp/lsmkv_test.sst";
    std::map<std::string, std::pair<std::string, bool>> oracle;
    std::mt19937 rng(7);

    // 1. 用跳表灌 5000 条随机数据（足够产生十几个索引块），flush 成 SST
    {
        lsm::SkipList sl;
        for (int i = 0; i < 5000; ++i) {
            std::string k = "key" + std::to_string(rng() % 8000);
            if (rng() % 10 == 0) {
                sl.Put(k, "", true);                          // 墓碑
                oracle[k] = {"", true};
            } else {
                std::string v = "value-" + std::to_string(i) + "-xxxx";
                sl.Put(k, v);
                oracle[k] = {v, false};
            }
        }
        lsm::SSTBuilder b(path);
        sl.ForEach([&](const lsm::Entry& e) { b.Add(e); });
        b.Finish();
    }

    // 2. 重新打开（模拟进程重启），全量点查对拍
    lsm::SST* sst = lsm::SST::Open(path);
    CHECK(sst != nullptr);
    if (!sst) return 1;
    CHECK(sst->num_entries() == oracle.size());

    for (const auto& [k, vt] : oracle) {
        std::string v;
        bool tomb = false;
        bool got = sst->Get(k, &v, &tomb);
        CHECK(got);
        if (got) {
            CHECK(v == vt.first);
            CHECK(tomb == vt.second);
        }
    }
    // 不存在的 key
    for (int i = 0; i < 1000; ++i) {
        std::string v;
        bool tomb = false;
        CHECK(sst->Get("absent" + std::to_string(i), &v, &tomb) == false);
    }

    // 3. 迭代器顺序性 + 与 oracle 逐条比对
    {
        auto it = sst->NewIter();
        auto mit = oracle.begin();
        size_t n = 0;
        for (; it.Valid(); it.Next(), ++n, ++mit) {
            CHECK(mit != oracle.end());
            CHECK(it.entry().key == mit->first);
            CHECK(it.entry().value == mit->second.first);
            CHECK(it.entry().tombstone == mit->second.second);
        }
        CHECK(n == oracle.size());
    }

    delete sst;
    if (g_fail) { printf("FAILED: %d\n", g_fail); return 1; }
    printf("ALL SST TESTS PASSED (%zu entries, file: %s)\n", oracle.size(), path.c_str());
    return 0;
}

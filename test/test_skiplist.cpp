// test_skiplist.cpp —— 跳表测试
//
// 测试策略（不引 gtest，保持零依赖，用断言宏自建最小测试框架）：
//   1. 基础功能：put/get/覆盖/墓碑/遍历有序性；
//   2. 对拍：随机操作序列与 std::map（标准答案）逐条比对 —— 这是
//      验证数据结构正确性的标准手法；
//   3. 性能：百万级 put 的吞吐，给 README 攒数据。
#include "skiplist.h"

#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <chrono>
#include <random>

static int g_fail = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "CHECK failed: %s (line %d)\n", #cond, __LINE__); \
            ++g_fail;                                                      \
        }                                                                  \
    } while (0)

static void TestBasic() {
    lsm::SkipList sl;
    std::string v;
    bool tomb = false;

    CHECK(sl.Get("missing", &v, &tomb) == false);   // 空表查不到

    sl.Put("b", "2");
    sl.Put("a", "1");
    sl.Put("c", "3");
    CHECK(sl.Get("a", &v, &tomb) && v == "1" && !tomb);
    CHECK(sl.Get("b", &v, &tomb) && v == "2");

    sl.Put("b", "overwritten");                     // 覆盖
    CHECK(sl.Get("b", &v, &tomb) && v == "overwritten");

    sl.Put("b", "", true);                          // 写墓碑
    CHECK(sl.Get("b", &v, &tomb) && tomb);          // 找得到，但要当"已删"处理

    // 有序遍历：flush 成 SST 依赖这个顺序
    std::string keys;
    sl.ForEach([&](const lsm::Entry& e) { keys += e.key; });
    CHECK(keys == "abc");

    printf("[OK] TestBasic\n");
}

static void TestRandomAgainstMap() {
    // 对拍：同样的随机操作序列灌进跳表和 std::map，任何时刻查询结果一致。
    // 操作分布：40% 新 key、30% 覆盖已有 key、20% 删除、10% 重复删。
    lsm::SkipList sl;
    std::map<std::string, std::pair<std::string, bool>> oracle;  // key -> (value, tombstone)
    std::mt19937 rng(42);
    const int kOps = 100000;

    auto rand_key = [&](int space) {
        return "key" + std::to_string(rng() % space);
    };

    for (int i = 0; i < kOps; ++i) {
        int op = rng() % 100;
        if (op < 40) {                              // 插入
            std::string k = "key" + std::to_string(rng() % 1000000);
            std::string v = "v" + std::to_string(i);
            sl.Put(k, v);
            oracle[k] = {v, false};
        } else if (op < 70 && !oracle.empty()) {    // 覆盖
            auto it = oracle.begin();
            std::advance(it, rng() % oracle.size());
            std::string v = "c" + std::to_string(i);
            sl.Put(it->first, v);
            it->second = {v, false};
        } else if (op < 90 && !oracle.empty()) {    // 删除（写墓碑）
            auto it = oracle.begin();
            std::advance(it, rng() % oracle.size());
            sl.Put(it->first, "", true);
            it->second = {"", true};
        } else {                                    // 随机查询并比对
            std::string k = rand_key(1000000);
            std::string v;
            bool tomb = false;
            bool got = sl.Get(k, &v, &tomb);
            auto it = oracle.find(k);
            bool expect = it != oracle.end();
            CHECK(got == expect);
            if (got && expect) {
                CHECK(v == it->second.first);
                CHECK(tomb == it->second.second);
            }
        }
    }

    // 全量对拍 + 遍历有序性
    std::string prev_key;
    bool first = true;
    size_t count = 0;
    sl.ForEach([&](const lsm::Entry& e) {
        auto it = oracle.find(e.key);
        CHECK(it != oracle.end());
        if (it != oracle.end()) {
            CHECK(e.value == it->second.first);
            CHECK(e.tombstone == it->second.second);
        }
        if (!first) CHECK(prev_key < e.key);        // 严格升序
        prev_key = e.key;
        first = false;
        ++count;
    });
    CHECK(count == oracle.size());

    printf("[OK] TestRandomAgainstMap (%zu live keys)\n", oracle.size());
}

static void TestPerf() {
    // 百万级 put 吞吐：跳表应该轻松达到每秒百万次以上
    lsm::SkipList sl;
    const int kN = 1000000;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kN; ++i)
        sl.Put("user" + std::to_string(i), std::string(64, 'x'));
    auto t1 = std::chrono::steady_clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();
    printf("[OK] TestPerf: %d puts in %.3fs = %.2f M puts/s, mem ~%.1f MB\n",
           kN, sec, kN / sec / 1e6, sl.ApproximateSize() / 1e6);
}

int main() {
    TestBasic();
    TestRandomAgainstMap();
    TestPerf();
    if (g_fail) {
        printf("FAILED: %d checks\n", g_fail);
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}

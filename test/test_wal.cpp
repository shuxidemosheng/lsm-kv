// test_wal.cpp —— 阶段 4 验收：kill -9 崩溃后不丢已确认数据
//
// 崩溃模型说明（诚实边界）：
//   - kill -9 杀掉进程：进程死，但内核页缓存还在 → 只要有 write（甚至
//     不用 fsync）数据就不丢。本测试验证的是"进程崩溃"级别的持久性。
//   - 掉电/内核崩溃：需要 fsync 才安全（引擎提供 opts.sync_wal，代价是
//     每次 put 一次磁盘同步）。两个层级的持久性语义必须分开讲。
#include "lsm_kv.h"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
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
    const std::string dir = "/tmp/lsmkv_wal";
    std::filesystem::remove_all(dir);

    lsm::LsmOptions opts;
    opts.dir = dir;
    opts.memtable_limit = 1 << 30;   // 调大：保证崩溃时数据全部留在 MemTable+未落盘
    opts.sync_wal = false;

    // 1. 子进程写入 100 条后直接被 kill -9（没有任何清理/flush 机会）
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        lsm::LsmKV* db = lsm::LsmKV::Open(opts, nullptr);
        if (!db) _exit(2);
        for (int i = 0; i < 100; ++i) {
            if (!db->Put("survivor" + std::to_string(i),
                         "value-" + std::to_string(i)))
                _exit(3);
        }
        db->Delete("survivor42");        // 墓碑也要能在恢复后存活
        pause();                          // 挂起，等父进程 kill -9（保证被信号杀死）
        _exit(0);
    }
    usleep(200000);                       // 给子进程时间把数据写完
    kill(pid, SIGKILL);                   // 模拟崩溃：析构/WAL 清理全被跳过
    int st;
    waitpid(pid, &st, 0);
    CHECK(WIFSIGNALED(st));               // 确认确实是信号杀的

    // 2. 父进程重新打开：WAL 重放应恢复全部 100 条（含那 1 条墓碑）
    {
        lsm::LsmKV* db = lsm::LsmKV::Open(opts, nullptr);
        CHECK(db != nullptr);
        for (int i = 0; i < 100; ++i) {
            std::string v;
            bool got = db->Get("survivor" + std::to_string(i), &v);
            if (i == 42) {
                CHECK(!got);              // 被墓碑删除
            } else {
                CHECK(got);
                if (got) CHECK(v == "value-" + std::to_string(i));
            }
        }
        // 3. 崩溃恢复后的引擎还能继续写（WAL 追加位置正确）
        CHECK(db->Put("post-crash", "still-alive"));
        delete db;                        // 优雅关闭
    }

    // 4. 再开一次：崩溃前 + 崩溃后写入的都在
    {
        lsm::LsmKV* db = lsm::LsmKV::Open(opts, nullptr);
        CHECK(db != nullptr);
        std::string v;
        CHECK(db->Get("survivor0", &v) && v == "value-0");
        CHECK(db->Get("post-crash", &v) && v == "still-alive");
        CHECK(!db->Get("survivor42", &v));
        delete db;
    }

    if (g_fail) { printf("FAILED: %d\n", g_fail); return 1; }
    printf("ALL WAL CRASH TESTS PASSED\n");
    return 0;
}

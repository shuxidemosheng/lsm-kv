// lsm_kv.h —— LSM-KV 引擎：把 MemTable / SST / 读路径串起来（阶段 2）
//
// 引擎结构：
//   Put/Delete → MemTable(跳表) + WAL(阶段 4)
//   MemTable 写满 → flush 成 L0 SST（不可变文件）
//   Get → MemTable → L0（新→旧）→ L1 → 返回
//   L0 个数超阈值 → compaction 合并进 L1（阶段 3）
//
// 版本规则（LSM 读路径的灵魂）：
//   同一个 key 在不同文件里可能都有记录，"最新"的那个版本说了算；
//   如果最新版本是墓碑 → key 已删除，且必须停止向更老的文件查找。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "sst.h"

namespace lsm {

struct LsmOptions {
    std::string dir = "/tmp/lsmkv-data";  // 数据目录
    size_t memtable_limit = 1 << 20;      // MemTable 写满阈值（默认 1MB，便于测试触发 flush）
    size_t l0_compact_trigger = 4;        // L0 文件数达到该值就触发 compaction
    bool sync_wal = false;                // 每次 Put 都 fsync WAL（强持久，性能差）
};

class LsmKV {
public:
    // 打开（或创建）一个库：扫描 SST 文件、重放 WAL、恢复状态
    static LsmKV* Open(const LsmOptions& opts, std::string* err);

    ~LsmKV();

    // 写入 / 删除（删除 = 写墓碑）。返回 false 表示 IO 错误。
    bool Put(const std::string& key, const std::string& value);
    bool Delete(const std::string& key);

    // 查询。true = 存在且未被删除（*value 有效）
    bool Get(const std::string& key, std::string* value);

    // 主动把 MemTable 落盘（Close 前调用可实现优雅关闭）
    void Flush();

    size_t memtable_size() const { return memtable_size_; }
    size_t l0_count() const { return l0_.size(); }

private:
    LsmKV() = default;
    bool LoadDir(std::string* err);       // 扫描目录，恢复 L0/L1
    bool FlushLocked();                   // MemTable → L0 SST，清空 WAL
    bool CompactLocked();                 // L0(+L1) 归并 → 新 L1

    LsmOptions opts_;
    SkipList memtable_;
    size_t memtable_size_ = 0;

    std::vector<SST*> l0_;                // 按 id 降序存（新的在前）
    SST* l1_ = nullptr;                   // 底层全量合并文件（至多一个）
    uint64_t next_file_id_ = 1;

    int wal_fd_ = -1;
};

}  // namespace lsm

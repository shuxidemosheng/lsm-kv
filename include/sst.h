// sst.h —— SST（Sorted String Table）：磁盘上的有序不可变文件
//
// 文件格式（从前往后）：
//   [data 区 ]  记录按 key 升序：
//               u8 tombstone | u32 klen | u32 vlen | key | value
//   [index 区 ]  稀疏索引：每 ~4KB 数据记录一条 (该块首 key, 块起始偏移)
//               u32 klen | key | u64 data_offset
//   [footer ]   固定 40 字节：
//               magic "LKVS" | version u32 | num_entries u64
//               index_offset u64 | num_index u32
//               bloom_offset u64 | bloom_len u32     ← 阶段 2 启用，先占位
//
// 设计动机：
//   - 稀疏索引只常驻内存（每 4KB 一条），点查先二分索引再小块扫描；
//   - SST 不可变：写入只在 builder，读无锁——LSM 一切简单性的来源。
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "bloom.h"
#include "skiplist.h"

namespace lsm {

constexpr uint32_t kSstMagic = 0x53564b4c;   // "LKVS" 小端
constexpr uint32_t kSstVersion = 1;
constexpr uint32_t kSstFooterSize = 40;
constexpr uint32_t kIndexInterval = 4096;    // 每 4KB 数据一个索引点

// 稀疏索引项
struct IndexEntry {
    std::string key;      // 该数据块的第一个 key
    uint64_t offset = 0;  // 该块在文件中的起始偏移
};

// 构建器：按 key 升序 Add，最后 Finish
class SSTBuilder {
public:
    explicit SSTBuilder(std::string path);
    ~SSTBuilder();

    SSTBuilder(const SSTBuilder&) = delete;
    SSTBuilder& operator=(const SSTBuilder&) = delete;

    void Add(const Entry& e);      // 前置条件：key 严格升序（跳表/归并保证）
    void Finish();                 // 写 index + footer，fsync 并关闭

    uint64_t num_entries() const { return num_entries_; }

private:
    void WriteRecord(const Entry& e);

    std::string path_;
    int fd_ = -1;
    uint64_t offset_ = 0;              // data 区当前写入位置
    uint64_t num_entries_ = 0;
    uint64_t bytes_since_index_ = 0;   // 距上个索引点累计的数据量
    std::vector<IndexEntry> index_;
    std::vector<std::string> keys_;    // 暂存全部 key，Finish 时构建布隆
    bool finished_ = false;
};

// 只读视图
class SST {
public:
    // 打开：读 footer + 整个稀疏索引进内存；失败返回 nullptr
    static SST* Open(const std::string& path);

    ~SST();

    // 点查。key 不在本文件 → false；在 → true（*tombstone 区分删除标记）
    // 先过布隆过滤器：不存在的 key 绝大多数在这里被拦下（阶段 2）
    bool Get(const std::string& key, std::string* value, bool* tombstone) const;

    // 顺序遍历 data 区（k 路归并的输入）
    class Iter {
    public:
        explicit Iter(const SST* sst);
        bool Valid() const { return valid_; }
        const Entry& entry() const { return cur_; }
        void Next();

    private:
        void Fill();                   // 把当前 offset 的记录解析进 cur_
        const SST* sst_;
        uint64_t offset_ = 0;
        Entry cur_;
        bool valid_ = false;
    };
    Iter NewIter() const { return Iter(this); }

    const std::string& path() const { return path_; }
    uint64_t num_entries() const { return num_entries_; }

private:
    SST() = default;
    bool ReadExact(uint64_t off, void* buf, size_t len) const;
    // 解析 off 处的一条记录；成功时 *next_off 给出下一条的位置
    bool ParseRecord(uint64_t off, Entry* e, uint64_t* next_off) const;

    std::string path_;
    int fd_ = -1;
    uint64_t num_entries_ = 0;
    uint64_t data_end_ = 0;      // = index_offset，data 区边界
    uint64_t index_offset_ = 0;
    uint64_t bloom_offset_ = 0;  // 阶段 2 启用
    uint32_t bloom_len_ = 0;
    BloomFilter bloom_;          // 从 bloom 区反序列化；空 = 未启用
    bool has_bloom_ = false;
    std::vector<IndexEntry> index_;
    std::string smallest_;   // = 第一个索引点的 key（快速排除用）
};

}  // namespace lsm
